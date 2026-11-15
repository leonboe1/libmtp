/* Exercises the download loop with simulated device transactions, without USB hardware. */
#include "config.h"
#include "libusb-glue.h"
#include "device-flags.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

extern uint16_t ptp_getobject_to_handler_bounded(PTPParams *, uint32_t, uint64_t, PTPDataHandler *);
static PTP_USB usb;
static PTPParams params;
static int supports32, supports64, transactions, legacy, legacy_response_received, in_transaction, cancel_requested;
static int cancel_in_transaction, cancel_on_progress, malformed, short_chunk, writer_error;
static int writer_cancel, writer_calls;
static uint16_t transport_error;
static uint64_t received, last_progress, expected_total, next_offset;
static uint32_t largest_request;
static int progress_inside_transaction;
static int progress_clock_ticks;
int clock_gettime(clockid_t clock, struct timespec *value) {
    assert(clock == CLOCK_MONOTONIC);
    value->tv_sec = progress_clock_ticks / 50;
    value->tv_nsec = (progress_clock_ticks % 50) * 20000000;
    progress_clock_ticks++;
    return 0;
}

/* Completes an entire data-and-response exchange before returning its payload. */
static uint16_t partial(PTPParams *p, uint32_t id, uint64_t offset,
                        uint32_t count, PTPDataHandler *handler) {
    assert(p == &params && id == 42 && !in_transaction);
    assert(offset == next_offset && count > 0 && count <= 32 * 1024 * 1024);
    assert(!usb.callback_active);
    in_transaction = 1;
    transactions++;
    if (count > largest_request) largest_request = count;
    if (cancel_in_transaction) cancel_requested = 1;
    if (!transport_error) {
        uint32_t length = malformed == 1 ? 0 : malformed == 2 ? count + 1 :
                          short_chunk && count > 1 ? count / 2 : count;
        unsigned char bytes[16384];
        memset(bytes, 0x5a, sizeof(bytes));
        for (uint32_t sent = 0; sent < length;) {
            uint32_t block = length - sent < sizeof(bytes) ? length - sent : sizeof(bytes);
            assert(handler->putfunc(p, handler->priv, block, bytes) == PTP_RC_OK);
            sent += block;
        }
        next_offset += length;
    }
    in_transaction = 0;
    return transport_error ? transport_error : PTP_RC_OK;
}

uint16_t ptp_android_getpartialobject64_to_handler(PTPParams *p, uint32_t id, uint64_t offset,
                                                  uint32_t count, PTPDataHandler *handler) {
    assert(supports64);
    return partial(p, id, offset, count, handler);
}

uint16_t ptp_getpartialobject_to_handler(PTPParams *p, uint32_t id, uint32_t offset,
                                        uint32_t count, PTPDataHandler *handler) {
    assert(supports32 && !supports64);
    return partial(p, id, offset, count, handler);
}

uint16_t ptp_getobject_to_handler(PTPParams *p, uint32_t id, PTPDataHandler *handler) {
    (void)p; (void)id; (void)handler;
    legacy++;
    in_transaction = 1;
    if (cancel_in_transaction) cancel_requested = 1;
    /* The real legacy path can call progress after writing the command or
     * reading the data header, before it has a response it can safely abandon. */
    if (usb.current_transfer_callback &&
        usb.current_transfer_callback(0, expected_total, usb.current_transfer_callback_data))
        return PTP_ERROR_CANCEL;
    legacy_response_received++;
    in_transaction = 0;
    return PTP_RC_OK;
}

/* Verifies that only validated, completed payloads reach the destination. */
static uint16_t put(PTPParams *p, void *context, unsigned long count, unsigned char *bytes) {
    assert(p == &params && context == &received);
    assert(bytes[0] == 0x5a && bytes[count - 1] == 0x5a);
    writer_calls++;
    if (writer_cancel) return PTP_ERROR_CANCEL;
    if (writer_error) return PTP_ERROR_IO;
    received += count;
    return PTP_RC_OK;
}

static int progress(uint64_t sent, uint64_t total, void const * const context) {
    assert(context == &received);
    if (in_transaction && !legacy) progress_inside_transaction++;
    assert(total == expected_total && sent >= last_progress && sent <= total);
    assert(sent == received);
    last_progress = sent;
    if (cancel_on_progress && sent > 0) cancel_requested = 1;
    return cancel_requested;
}

static void reset(uint64_t size) {
    memset(&usb, 0, sizeof(usb));
    memset(&params, 0, sizeof(params));
    params.data = &usb;
    supports32 = supports64 = 1;
    transactions = legacy = legacy_response_received = in_transaction = cancel_requested = 0;
    cancel_in_transaction = cancel_on_progress = malformed = short_chunk = writer_error = 0;
    writer_cancel = writer_calls = 0;
    transport_error = 0;
    received = last_progress = next_offset = largest_request = 0;
    expected_total = size;
    progress_inside_transaction = progress_clock_ticks = 0;
    usb.callback_active = 1;
    usb.current_transfer_callback = progress;
    usb.current_transfer_callback_data = &received;
}

static uint16_t download(void) {
    uint16_t operations[2];
    params.deviceinfo.Operations_len = 0;
    if (supports32) operations[params.deviceinfo.Operations_len++] = PTP_OC_GetPartialObject;
    if (supports64) operations[params.deviceinfo.Operations_len++] = PTP_OC_ANDROID_GetPartialObject64;
    params.deviceinfo.Operations = operations;
    PTPDataHandler handler = {0};
    handler.priv = &received;
    handler.putfunc = put;
    uint16_t result = ptp_getobject_to_handler_bounded(&params, 42, expected_total, &handler);
    assert(usb.callback_active == 1);
    assert(!usb.current_transfer_callback || usb.current_transfer_callback == progress);
    assert(usb.current_transfer_callback_data == &received);
    assert(!usb.session_abandoned && !in_transaction);
    return result;
}

int main(void) {
    /* Cancellation latency is at most one chunk, even for an 8 GiB object. */
    reset(UINT64_C(8) * 1024 * 1024 * 1024);
    cancel_in_transaction = 1;
    assert(download() == PTP_ERROR_CANCEL);
    assert(transactions == 1 && legacy == 0 && received <= 32 * 1024 * 1024);

    /* Another transfer on the same session succeeds after cancellation. */
    cancel_requested = cancel_in_transaction = 0;
    received = next_offset = last_progress = 0;
    expected_total = 17;
    assert(download() == PTP_RC_OK && received == 17 && transactions == 2);

    reset(8000000);
    assert(download() == PTP_RC_OK && progress_inside_transaction > 1);

    reset(8000000);
    cancel_requested = 1;
    assert(download() == PTP_ERROR_CANCEL && transactions == 0);
    reset(8000000);
    cancel_on_progress = 1;
    assert(download() == PTP_ERROR_CANCEL && transactions == 1);
    reset(17);
    cancel_on_progress = 1;
    assert(download() == PTP_ERROR_CANCEL && transactions == 1);

    reset(0);
    assert(download() == PTP_RC_OK && transactions == 0 && received == 0);
    reset(8000001);
    short_chunk = 1;
    assert(download() == PTP_RC_OK && received == 8000001 && last_progress == received);
    reset(17);
    usb.current_transfer_callback = NULL;
    assert(download() == PTP_RC_OK && received == 17);

    reset(100);
    malformed = 1;
    assert(download() != PTP_RC_OK && transactions == 1 && received == 0);
    reset(100);
    malformed = 2;
    assert(download() != PTP_RC_OK && transactions == 1 && received == 0);
    reset(8000000);
    transport_error = PTP_RC_OperationNotSupported;
    assert(download() == PTP_RC_OperationNotSupported && legacy == 0 && received == 0);
    reset(8000000);
    writer_error = 1;
    assert(download() == PTP_RC_GeneralError && transactions == 1 && received == 0);

    /* A destination handler's cancellation remains cancellation after the
     * current response drains; no more destination calls or requests follow. */
    reset(8000000);
    writer_cancel = 1;
    assert(download() == PTP_ERROR_CANCEL);
    assert(transactions == 1 && received == 0 && next_offset == 8000000 && writer_calls == 1);
    writer_cancel = 0;
    next_offset = 0;
    expected_total = 17;
    assert(download() == PTP_RC_OK && received == 17 && transactions == 2 && writer_calls == 2);

    reset(100);
    supports32 = supports64 = 0;
    assert(download() == PTP_RC_OK && legacy == 1 && transactions == 0);
    reset(UINT64_C(8) * 1024 * 1024 * 1024);
    supports64 = 0;
    assert(download() == PTP_RC_OK && legacy == 1 && transactions == 0);
    reset(500);
    supports64 = 0;
    params.device_flags = DEVICE_FLAG_SAMSUNG_OFFSET_BUG;
    assert(download() == PTP_RC_OK && received == 500 && transactions == 2);
    assert(largest_request == 499);

    /* Legacy cancellation must finish the response even when requested during
     * command/header progress, where upstream does not recover an early exit. */
    reset(UINT64_C(8) * 1024 * 1024 * 1024);
    supports64 = 0;
    cancel_in_transaction = 1;
    assert(download() == PTP_ERROR_CANCEL && legacy_response_received == 1);
    reset(100);
    supports32 = supports64 = 0;
    cancel_requested = 1;
    assert(download() == PTP_ERROR_CANCEL && legacy == 0);

    /* Complete a >4 GiB object within a 129-transaction budget while preserving
     * 64-bit offsets. Small chunks must not regress per-request overhead. */
    reset(UINT64_C(4) * 1024 * 1024 * 1024 + 17);
    assert(download() == PTP_RC_OK && received == expected_total);
    assert(transactions <= 129 && legacy == 0 && last_progress == expected_total);
    puts("Bounded download regression tests passed.");
    return 0;
}
