/* Verify complete handle datasets through the public uncached folder-listing API. */
#include "config.h"
#include "libusb-glue.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct fixture {
    unsigned char *data;
    unsigned int size;
};

static uint16_t send_request(PTPParams *params, PTPContainer *command, int phase)
{
    assert(command->Code == PTP_OC_GetObjectHandles && phase == PTP_DP_GETDATA);
    return PTP_RC_OK;
}

static uint16_t get_data(PTPParams *params, PTPContainer *command, PTPDataHandler *destination)
{
    struct fixture *reply = params->data;
    if (!reply->size)
        return PTP_RC_OK;
    return destination->putfunc(params, destination->priv, reply->size, reply->data);
}

static uint16_t get_response(PTPParams *params, PTPContainer *response)
{
    response->Code = PTP_RC_OK;
    return PTP_RC_OK;
}

static void check_listing(unsigned char *data, unsigned int size, int expected_count)
{
    struct fixture reply = {data, size};
    PTPParams params = {
        .byteorder = PTP_DL_LE, .data = &reply,
        .sendreq_func = send_request, .getdata_func = get_data, .getresp_func = get_response
    };
    LIBMTP_mtpdevice_t device = { .params = &params };
    uint32_t *children = NULL;
    int count = LIBMTP_Get_Children(&device, 1, 7, &children);

    assert(count == expected_count);
    assert((LIBMTP_Get_Errorstack(&device) != NULL) == (expected_count < 0));
    if (expected_count > 0) {
        assert(children && children[0] == 11 && children[1] == 23);
    } else {
        assert(!children);
    }
    free(children);
    LIBMTP_Clear_Errorstack(&device);
}

/* A child can disappear after GetObjectHandles succeeds. Other metadata
 * failures must remain distinguishable on the public file metadata API. */
static uint16_t reject_metadata(PTPParams *params, PTPContainer *command, PTPDataHandler *destination)
{
    assert(command->Param1 == 11);
    return *(uint16_t *)params->data;
}

static uint16_t request_metadata(PTPParams *params, PTPContainer *command, int phase)
{
    uint16_t expected = params->deviceinfo.Operations_len ? PTP_OC_MTP_GetObjPropList : PTP_OC_GetObjectInfo;
    assert(command->Code == expected && phase == PTP_DP_GETDATA);
    return PTP_RC_OK;
}

static void check_metadata_error(uint16_t code, int needs_properties)
{
    PTPParams params = {.data = &code, .byteorder = PTP_DL_LE,
                        .sendreq_func = request_metadata, .getdata_func = reject_metadata};
    LIBMTP_mtpdevice_t device = {.params = &params};
    uint16_t operation = PTP_OC_MTP_GetObjPropList;
    if (needs_properties) {
        params.deviceinfo.Operations = &operation;
        params.deviceinfo.Operations_len = 1;
        params.objects.val = calloc(1, sizeof(PTPObject));
        assert(params.objects.val);
        params.objects.len = 1;
        params.objects.val[0].oid = 11;
        params.objects.val[0].flags = PTPOBJECT_OBJECTINFO_LOADED | PTPOBJECT_STORAGEID_LOADED |
                                      PTPOBJECT_PARENTOBJECT_LOADED;
    } else {
        assert(!LIBMTP_Get_Filemetadata(&device, 11));
    }
    uint16_t response = PTP_RC_OK;
    assert(!LIBMTP_Get_Filemetadata_With_Response(&device, 11, &response));
    assert(response == code);
    if (code == PTP_RC_InvalidObjectHandle)
        assert(params.objects.len == 0);
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "PTP Layer error %04x:", code);
    LIBMTP_error_t *entry = LIBMTP_Get_Errorstack(&device);
    while (entry && (entry->errornumber != LIBMTP_ERROR_PTP_LAYER ||
                     strncmp(entry->error_text, prefix, strlen(prefix)) != 0))
        entry = entry->next;
    assert(entry && "metadata failure must preserve its PTP code");
    LIBMTP_Clear_Errorstack(&device);
    free(params.objects.val);
}

struct metadata_reply {
    unsigned char info[80];
    uint16_t property_result;
};

static uint16_t request_file_metadata(PTPParams *params, PTPContainer *command, int phase)
{
    assert(command->Param1 == 11 && phase == PTP_DP_GETDATA);
    assert(command->Code == PTP_OC_GetObjectInfo || command->Code == PTP_OC_MTP_GetObjPropList);
    return PTP_RC_OK;
}

static uint16_t read_file_metadata(PTPParams *params, PTPContainer *command, PTPDataHandler *destination)
{
    struct metadata_reply *reply = params->data;
    if (command->Code == PTP_OC_GetObjectInfo)
        return destination->putfunc(params, destination->priv, sizeof(reply->info), reply->info);
    return reply->property_result;
}

/* Read ObjectInfo over the wire, then reject the optional all-properties request. */
static void check_object_info(struct metadata_reply *reply, uint32_t device_flags, const char *expected_name)
{
    uint16_t operations[] = { PTP_OC_MTP_GetObjPropList, PTP_OC_MTP_GetObjectPropsSupported };
    PTPParams params = {.byteorder = PTP_DL_LE, .data = reply, .device_flags = device_flags,
                        .sendreq_func = request_file_metadata, .getdata_func = read_file_metadata,
                        .getresp_func = get_response};
    params.deviceinfo.Operations = operations;
    params.deviceinfo.Operations_len = 2;
#if defined(HAVE_ICONV) && defined(HAVE_LANGINFO_H)
    params.cd_ucs2_to_locale = (iconv_t)-1;
#endif
    PTP_USB usb = {0};
    LIBMTP_mtpdevice_t device = {.params = &params, .usbinfo = &usb};
    uint16_t response = 0;
    LIBMTP_file_t *file = LIBMTP_Get_Filemetadata_With_Response(&device, 11, &response);
    if (expected_name) {
        assert(file && response == PTP_RC_OK);
        assert(file->item_id == 11 && file->parent_id == 7 && file->storage_id == 1);
        assert(strcmp(file->filename, expected_name) == 0 && file->filesize == 23);
        assert(!LIBMTP_Get_Errorstack(&device));
    } else {
        assert(!file && response == reply->property_result);
        assert(LIBMTP_Get_Errorstack(&device));
    }
    LIBMTP_destroy_file_t(file);
    LIBMTP_Clear_Errorstack(&device);
    for (unsigned int i = 0; i < params.objects.len; i++)
        free(params.objects.val[i].oi.Filename);
    free(params.objects.val);
}

int main(void)
{
    unsigned char empty[] = {0, 0, 0, 0};
    unsigned char two[] = {2, 0, 0, 0, 11, 0, 0, 0, 23, 0, 0, 0};
    check_listing(empty, sizeof(empty), 0);
    check_listing(two, sizeof(two), 2);
    check_listing(two, sizeof(two) - 1, -1); /* Incomplete final handle. */
    check_listing(empty, sizeof(empty) - 1, -1); /* Incomplete count. */
    check_listing(NULL, 0, -1);
    two[0] = 0;
    check_listing(two, sizeof(two), -1); /* Handles after a declared empty list. */
    for (int needs_properties = 0; needs_properties <= 1; needs_properties++) {
        check_metadata_error(PTP_RC_InvalidObjectHandle, needs_properties);
        check_metadata_error(PTP_RC_AccessDenied, needs_properties);
        check_metadata_error(PTP_ERROR_IO, needs_properties);
        check_metadata_error(PTP_RC_OperationNotSupported, needs_properties);
    }
    struct metadata_reply reply = {.info = { [0] = 1, [8] = 23, [38] = 7, [52] = 2, [53] = 'x' }};
    htod16a(reply.info + 4, PTP_OFC_Undefined);
    uint16_t unsupported[] = {PTP_RC_OperationNotSupported, PTP_RC_ParameterNotSupported,
        PTP_RC_MTP_Invalid_ObjectPropCode, PTP_RC_MTP_ObjectProp_Not_Supported,
        PTP_RC_SpecificationByFormatUnsupported, PTP_RC_MTP_Specification_By_Group_Unsupported,
        PTP_RC_MTP_Specification_By_Depth_Unsupported};
    for (unsigned int i = 0; i < sizeof(unsupported)/sizeof(unsupported[0]); i++) {
        reply.property_result = unsupported[i];
        check_object_info(&reply, 0, "x");
    }
    reply.property_result = PTP_RC_InvalidObjectHandle;
    check_object_info(&reply, 0, NULL);
    reply.property_result = PTP_ERROR_IO;
    check_object_info(&reply, 0, NULL);
    reply.property_result = PTP_RC_AccessDenied;
    check_object_info(&reply, 0, NULL);
    reply.property_result = PTP_RC_OperationNotSupported;
    check_object_info(&reply, DEVICE_FLAG_PROPLIST_OVERRIDES_OI, NULL);
    htod32a(reply.info + 8, UINT32_MAX);
    check_object_info(&reply, 0, NULL);
    htod32a(reply.info + 8, 23);
    reply.info[52] = 0;
    check_object_info(&reply, 0, NULL);
    reply.info[52] = 255; /* Truncated UTF-16 filename. */
    check_object_info(&reply, 0, NULL);
    const char literal_name[] = "<none>";
    reply.info[52] = sizeof(literal_name);
    for (unsigned int i = 0; i < sizeof(literal_name); i++)
        reply.info[53 + 2*i] = literal_name[i];
    check_object_info(&reply, 0, literal_name);
    return 0;
}
