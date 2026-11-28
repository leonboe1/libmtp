/* Exercise the rename diagnostics used to decide whether overwrite rollback is safe. */
#include "config.h"
#include "libusb-glue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct rename_fixture {
    uint16_t write_operation, failing_operation, error;
    unsigned int writes;
};

static uint16_t send_request(PTPParams *params, PTPContainer *request, int phase)
{
    struct rename_fixture *fixture = params->data;
    assert(request->Code == PTP_OC_MTP_GetObjectPropDesc || request->Code == fixture->write_operation);
    return PTP_RC_OK;
}

static uint16_t get_descriptor(PTPParams *params, PTPContainer *request, PTPDataHandler *destination)
{
    /* Writable ObjectFileName string, empty default value, no group or form. */
    unsigned char descriptor[] = {0x07, 0xdc, 0xff, 0xff, 1, 0, 0, 0, 0, 0, 0};
    return destination->putfunc(params, destination->priv, sizeof(descriptor), descriptor);
}

static uint16_t send_rename(PTPParams *params, PTPContainer *request, uint64_t size, PTPDataHandler *source)
{
    struct rename_fixture *fixture = params->data;
    assert(request->Code == fixture->write_operation && size > 0);
    fixture->writes++;
    return PTP_RC_OK;
}

static uint16_t get_response(PTPParams *params, PTPContainer *response)
{
    struct rename_fixture *fixture = params->data;
    if (response->Code == fixture->failing_operation)
        return fixture->error;
    response->Code = PTP_RC_OK;
    return PTP_RC_OK;
}

static void check_rename(uint16_t write_operation, uint16_t failing_operation, uint16_t error)
{
    struct rename_fixture fixture = {write_operation, failing_operation, error, 0};
    PTP_USB usb = {0};
    PTPParams params = {
        .byteorder = PTP_DL_LE, .data = &fixture,
        .deviceinfo = {.Operations = &write_operation, .Operations_len = 1},
        .sendreq_func = send_request, .getdata_func = get_descriptor,
        .senddata_func = send_rename, .getresp_func = get_response
    };
#if defined(HAVE_ICONV) && defined(HAVE_LANGINFO_H)
    params.cd_locale_to_ucs2 = (iconv_t)-1;
#endif
    LIBMTP_mtpdevice_t device = {.params = &params, .usbinfo = &usb};
    LIBMTP_file_t file = {.item_id = 42, .filetype = LIBMTP_FILETYPE_UNKNOWN};
    assert(LIBMTP_Set_File_Name(&device, &file, ".idroid-backup-original") == -1);
    assert(fixture.writes == (failing_operation == write_operation ? 1 : 0));

    char prefix[32];
    snprintf(prefix, sizeof(prefix), "PTP Layer error %04x:", error);
    LIBMTP_error_t *entry = LIBMTP_Get_Errorstack(&device);
    while (entry && (entry->errornumber != LIBMTP_ERROR_PTP_LAYER ||
                     strncmp(entry->error_text, prefix, strlen(prefix)) != 0))
        entry = entry->next;
    assert(entry && "the caller must receive the original device or transport error");
    LIBMTP_Clear_Errorstack(&device);
}

int main(void)
{
    check_rename(PTP_OC_MTP_SetObjPropList, PTP_OC_MTP_SetObjPropList, PTP_ERROR_IO);
    check_rename(PTP_OC_MTP_SetObjPropList, PTP_OC_MTP_SetObjPropList, PTP_RC_AccessDenied);
    check_rename(PTP_OC_MTP_SetObjPropList, PTP_OC_MTP_GetObjectPropDesc, PTP_ERROR_TIMEOUT);
    check_rename(PTP_OC_MTP_SetObjectPropValue, PTP_OC_MTP_SetObjectPropValue, PTP_ERROR_IO);
    return 0;
}
