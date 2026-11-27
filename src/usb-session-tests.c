/* Exercise startup when the previous USB owner left a read outstanding. */
#include "config.h"
#include "libusb-glue.h"
#include <assert.h>
#include <string.h>

enum scenario {
    CLEAN_SESSION, ABANDONED_READ, EMPTY_ENDPOINT, INCOMPLETE_WRITE,
    OTHER_COMMAND, BUSY_ENDPOINT
};

static enum scenario scenario;
static unsigned int writes;
static unsigned int reads;
static unsigned char original_request[16];

/* Simulate the kernel refusing an OUT request until a previous IN response is read. */
int libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint,
                        unsigned char *data, int length, int *transferred,
                        unsigned int timeout)
{
    assert(!handle && timeout > 0);
    if (endpoint == 0x81) {
        assert(writes == 1 && length >= 512);
        reads++;
        if ((scenario == ABANDONED_READ && reads == 3) || scenario == EMPTY_ENDPOINT) {
            *transferred = 0;
            return LIBUSB_ERROR_TIMEOUT;
        }
        assert(scenario == ABANDONED_READ || scenario == BUSY_ENDPOINT);
        *transferred = reads == 1 ? length : 16;
        return LIBUSB_SUCCESS;
    }

    assert(endpoint == 0x01 && length <= (int)sizeof(original_request));
    writes++;
    if (writes == 1) {
        memcpy(original_request, data, length);
        *transferred = scenario == INCOMPLETE_WRITE ? 4 : 0;
        if (scenario != CLEAN_SESSION)
            return LIBUSB_ERROR_TIMEOUT;
    } else {
        assert(scenario == ABANDONED_READ && writes == 2 && reads == 3);
        assert(length == sizeof(original_request) && !memcmp(original_request, data, length));
    }
    *transferred = length;
    return LIBUSB_SUCCESS;
}

static uint16_t send_request(enum scenario next)
{
    scenario = next;
    reads = writes = 0;
    PTP_USB usb = { .inep = 0x81, .outep = 0x01, .inep_maxpacket = 512,
                    .outep_maxpacket = 512, .timeout = 5000 };
    PTPParams params = { .data = &usb };
    PTPContainer command = { .Code = PTP_OC_OpenSession, .Nparam = 1, .Param1 = 1 };
    if (scenario == OTHER_COMMAND) {
        command.Code = PTP_OC_CloseSession;
        command.Nparam = 0;
    }
    return ptp_usb_sendreq(&params, &command, PTP_DP_NODATA);
}

int main(void)
{
    assert(send_request(CLEAN_SESSION) == PTP_RC_OK && writes == 1 && reads == 0);
    assert(send_request(ABANDONED_READ) == PTP_RC_OK && writes == 2 && reads == 3);
    assert(send_request(EMPTY_ENDPOINT) == PTP_ERROR_IO && writes == 1 && reads == 1);
    assert(send_request(INCOMPLETE_WRITE) == PTP_ERROR_IO && writes == 1 && reads == 0);
    assert(send_request(OTHER_COMMAND) == PTP_ERROR_IO && writes == 1 && reads == 0);
    assert(send_request(BUSY_ENDPOINT) == PTP_ERROR_IO && writes == 1 && reads > 0 && reads <= 32);
    return 0;
}
