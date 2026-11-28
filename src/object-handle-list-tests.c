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
    assert(command->Code == PTP_OC_GetObjectInfo && command->Param1 == 11);
    return *(uint16_t *)params->data;
}

static uint16_t request_metadata(PTPParams *params, PTPContainer *command, int phase)
{
    assert(command->Code == PTP_OC_GetObjectInfo && phase == PTP_DP_GETDATA);
    return PTP_RC_OK;
}

static void check_metadata_error(uint16_t code)
{
    PTPParams params = {.data = &code, .byteorder = PTP_DL_LE,
                        .sendreq_func = request_metadata, .getdata_func = reject_metadata};
    LIBMTP_mtpdevice_t device = {.params = &params};
    assert(!LIBMTP_Get_Filemetadata(&device, 11));
    uint16_t response = PTP_RC_OK;
    assert(!LIBMTP_Get_Filemetadata_With_Response(&device, 11, &response));
    assert(response == code);
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
    check_metadata_error(PTP_RC_InvalidObjectHandle);
    check_metadata_error(PTP_RC_AccessDenied);
    check_metadata_error(PTP_ERROR_IO);
    return 0;
}
