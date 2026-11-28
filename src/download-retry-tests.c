/* Exercise a same-session retry through libmtp's public download APIs and object cache. */
#include "config.h"
#include "libusb-glue.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char contents[] = "file changed on the phone";
static unsigned int device_size;

static uint16_t send_request(PTPParams *params, PTPContainer *command, int phase)
{
    assert(phase == PTP_DP_GETDATA && command->Param1 == 42);
    return PTP_RC_OK;
}

static uint16_t get_data(PTPParams *params, PTPContainer *command, PTPDataHandler *destination)
{
    if (command->Code == PTP_OC_GetObjectInfo) {
        /* ObjectInfo with the UTF-16 filename "x" and empty optional strings. */
        unsigned char info[60] = { [52] = 2, [53] = 'x' };
        htod32a(info, 1);
        htod16a(info + 4, PTP_OFC_Undefined);
        htod32a(info + 8, device_size);
        return destination->putfunc(params, destination->priv, sizeof(info), info);
    }
    assert(command->Code == PTP_OC_GetPartialObject);
    unsigned int offset = command->Param2;
    assert(offset <= device_size);
    unsigned int count = device_size - offset;
    if (count > command->Param3) count = command->Param3;
    return destination->putfunc(params, destination->priv, count, contents + offset);
}

static uint16_t get_response(PTPParams *params, PTPContainer *response)
{
    response->Code = PTP_RC_OK;
    return PTP_RC_OK;
}

static uint16_t put(void *params, void *file, uint32_t count, unsigned char *data, uint32_t *written)
{
    *written = fwrite(data, 1, count, file);
    return *written == count ? LIBMTP_HANDLER_RETURN_OK : LIBMTP_HANDLER_RETURN_ERROR;
}

static void check_retry(unsigned int listed_size, unsigned int current_size, int use_handler, int cached)
{
    uint16_t operation = PTP_OC_GetPartialObject;
    PTP_USB usb = {0};
    PTPParams params = { .byteorder = PTP_DL_LE, .data = &usb,
        .sendreq_func = send_request, .getdata_func = get_data, .getresp_func = get_response };
    params.deviceinfo.Operations = &operation;
    params.deviceinfo.Operations_len = 1;
#if defined(HAVE_ICONV) && defined(HAVE_LANGINFO_H)
    params.cd_ucs2_to_locale = (iconv_t)-1;
#endif
    LIBMTP_mtpdevice_t device = { .params = &params, .usbinfo = &usb };

    device_size = listed_size;
    LIBMTP_file_t *listing = LIBMTP_Get_Filemetadata(&device, 42);
    assert(listing && listing->filesize == listed_size);
    LIBMTP_destroy_file_t(listing);
    device.cached = cached;
    device_size = current_size;

    for (int attempt = 0; attempt < 2; attempt++) {
        FILE *output = tmpfile();
        assert(output);
        int result = use_handler
            ? LIBMTP_Get_File_To_Handler(&device, 42, put, output, NULL, NULL)
            : LIBMTP_Get_File_To_File_Descriptor(&device, 42, fileno(output), NULL, NULL);
        if (attempt == 0) {
            assert(result == -1 && LIBMTP_Get_Errorstack(&device));
            LIBMTP_Clear_Errorstack(&device);
        } else {
            assert(result == 0);
            rewind(output);
            unsigned char bytes[sizeof(contents)];
            assert(fread(bytes, 1, sizeof(bytes), output) == current_size);
            assert(!memcmp(bytes, contents, current_size));
        }
        fclose(output);
    }
    for (unsigned int i = 0; i < params.objects.len; i++)
        ptp_free_object(&params.objects.val[i]);
    free(params.objects.val);
}

int main(void)
{
    for (int use_handler = 0; use_handler <= 1; use_handler++) {
        for (int cached = 0; cached <= 1; cached++) {
            check_retry(0, 23, use_handler, cached);
            check_retry(7, 23, use_handler, cached);
            check_retry(23, 7, use_handler, cached);
        }
    }
    return 0;
}
