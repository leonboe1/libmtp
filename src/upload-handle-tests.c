/* Exercise upload cleanup's handle contract through the public file upload API. */
#include "config.h"
#include "libusb-glue.h"
#include <assert.h>
#include <stdio.h>

static uint16_t creation_result;
static uint32_t response_handle;
static unsigned int creation_requests, object_sends;

/* Count the metadata and data commands issued by the real transaction code. */
static uint16_t send_request(PTPParams *params, PTPContainer *request, int phase)
{
    switch (request->Code) {
    case PTP_OC_MTP_GetObjectPropsSupported:
        break;
    case PTP_OC_MTP_SendObjectPropList:
        assert(request->Param3 == PTP_OFC_Undefined);
        creation_requests++;
        break;
    case PTP_OC_SendObjectInfo:
        creation_requests++;
        break;
    case PTP_OC_SendObject:
        object_sends++;
        break;
    default:
        assert(0 && "unexpected command");
    }
    return PTP_RC_OK;
}

static uint16_t send_data(PTPParams *params, PTPContainer *request, uint64_t size,
                          PTPDataHandler *source)
{
    return PTP_RC_OK;
}

static uint16_t get_data(PTPParams *params, PTPContainer *request, PTPDataHandler *destination)
{
    unsigned char empty_property_list[4] = {0};
    assert(request->Code == PTP_OC_MTP_GetObjectPropsSupported);
    return destination->putfunc(params, destination->priv, sizeof(empty_property_list), empty_property_list);
}

/* USB leaves request parameters in place when the device rejects a command. */
static uint16_t get_response(PTPParams *params, PTPContainer *response)
{
    if (response->Code == PTP_OC_SendObject)
        return PTP_RC_StoreFull;
    if (response->Code != PTP_OC_MTP_GetObjectPropsSupported) {
        if (creation_result != PTP_RC_OK)
            return creation_result;
        response->Param3 = response_handle;
    }
    response->Code = PTP_RC_OK;
    return PTP_RC_OK;
}

static void check_upload(uint16_t operation, uint16_t creation_status, uint32_t handle,
                         uint32_t expected_handle, unsigned int expected_sends)
{
    PTP_USB usb = {0};
    PTPParams params = {
        .sendreq_func = send_request, .senddata_func = send_data,
        .getdata_func = get_data, .getresp_func = get_response, .data = &usb,
        .byteorder = PTP_DL_LE,
        .deviceinfo = { .Operations = &operation, .Operations_len = 1 }
    };
#if defined(HAVE_ICONV) && defined(HAVE_LANGINFO_H)
    params.cd_locale_to_ucs2 = (iconv_t)-1;
#endif
    LIBMTP_mtpdevice_t device = { .params = &params, .usbinfo = &usb };
    LIBMTP_file_t file = {
        .filename = "upload.bin", .filetype = LIBMTP_FILETYPE_UNKNOWN,
        .storage_id = 1, .item_id = 123
    };
    creation_result = creation_status;
    response_handle = handle;
    creation_requests = object_sends = 0;

    FILE *source = tmpfile();
    assert(source);
    int result = LIBMTP_Send_File_From_File_Descriptor(&device, fileno(source), &file, NULL, NULL);
    fclose(source);
    assert(result == -1);
    assert(creation_requests == 1);
    assert(file.item_id == expected_handle);
    assert(object_sends == expected_sends);
    LIBMTP_Clear_Errorstack(&device);
}

int main(void)
{
    const uint16_t operations[] = {PTP_OC_MTP_SendObjectPropList, PTP_OC_SendObjectInfo};
    for (unsigned int i = 0; i < sizeof(operations) / sizeof(operations[0]); i++) {
        check_upload(operations[i], PTP_RC_AccessDenied, 0, 0, 0);
        check_upload(operations[i], PTP_RC_OK, 0, 0, 0);
        check_upload(operations[i], PTP_RC_OK, UINT32_MAX, 0, 0);
        /* A later SendObject failure must preserve the assigned handle, even if
         * it happens to equal the format code used in the original request. */
        check_upload(operations[i], PTP_RC_OK, PTP_OFC_Undefined, PTP_OFC_Undefined, 1);
    }
    return 0;
}
