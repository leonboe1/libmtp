/* Verify complete handle datasets through the public uncached folder-listing API. */
#include "config.h"
#include "libusb-glue.h"
#include <assert.h>
#include <stdlib.h>

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
    return 0;
}
