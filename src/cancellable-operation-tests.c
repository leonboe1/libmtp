/* Tests the real asynchronous wait with a controllable libusb endpoint. */
#include "config.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "libusb-glue.h"

extern void LIBMTP_Set_Cancellable_Wait(LIBMTP_mtpdevice_t *, int (*)(void *), void *);
extern int ptp_usb_bulk_transfer(libusb_device_handle *, unsigned char, unsigned char *, int, int *, unsigned int);

static struct libusb_transfer *pending;
static int polls, complete_after, cancel_after, cancelled, callback_delivered, submitted;
static enum libusb_transfer_status completion_status;
static unsigned int observed_timeout;
static int cancellation_error, suppress_callback, actual_length, allocation_failure, submit_error;
int clock_gettime(clockid_t clock, struct timespec *value) {
    assert(clock == CLOCK_MONOTONIC);
    value->tv_sec = polls / 10;
    value->tv_nsec = (polls % 10) * 100000000;
    return 0;
}

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data,
                        int length, int *transferred, unsigned int timeout)
{
    (void)h; (void)ep; (void)data;
    observed_timeout = timeout;
    *transferred = length;
    return 0;
}
struct libusb_transfer *libusb_alloc_transfer(int packets) {
    assert(packets == 0);
    if (allocation_failure) return NULL;
    return calloc(1, sizeof(struct libusb_transfer));
}
void libusb_free_transfer(struct libusb_transfer *t) {
    assert(!submitted || callback_delivered);
    free(t);
}
int libusb_submit_transfer(struct libusb_transfer *t) {
    if (submit_error) return submit_error;
    pending = t;
    submitted++;
    observed_timeout = t->timeout;
    return 0;
}
int libusb_cancel_transfer(struct libusb_transfer *t) {
    assert(t == pending);
    cancelled++;
    return cancellation_error;
}
int LIBMTP_Handle_Events_Timeout_Completed(struct timeval *tv, int *completed) {
    assert(tv->tv_sec == 0 && tv->tv_usec <= 100000);
    assert(!*completed);
    polls++;
    if (!suppress_callback && (polls >= complete_after || (cancelled && polls >= cancel_after + 3))) {
        pending->status = cancelled ? LIBUSB_TRANSFER_CANCELLED : completion_status;
        pending->actual_length = cancelled ? 0 : actual_length;
        callback_delivered = 1;
        pending->callback(pending);
    }
    return 0;
}
static int should_cancel(void *context) {
    assert(context == &polls);
    return cancel_after > 0 && polls >= cancel_after;
}
static void reset(void) {
    polls = cancelled = callback_delivered = submitted = cancel_after = 0;
    cancellation_error = suppress_callback = allocation_failure = submit_error = 0;
    actual_length = 12;
    complete_after = 10000;
    completion_status = LIBUSB_TRANSFER_COMPLETED;
}
int main(void) {
    PTP_USB usb = {0};
    LIBMTP_mtpdevice_t device = {0};
    device.usbinfo = &usb;
    usb.handle = (libusb_device_handle *)&device;
    unsigned char bytes[512];
    int count;
    reset();
    LIBMTP_Set_Cancellable_Wait(&device, should_cancel, &polls);
    assert(ptp_usb_bulk_transfer(usb.handle, 0x81, bytes, sizeof(bytes), &count, 20000) == 0);
    assert(observed_timeout == 0 && polls == 10000 && count == 12 && submitted == 1);
    assert(!usb.session_abandoned);
    reset();
    cancel_after = 4;
    assert(ptp_usb_bulk_transfer(usb.handle, 0x81, bytes, sizeof(bytes), &count, 20000) == LIBUSB_ERROR_INTERRUPTED);
    assert(cancelled == 1 && callback_delivered && polls == 7 && submitted == 1);
    assert(usb.session_abandoned); /* Teardown must not send CloseSession afterward. */
    reset();
    completion_status = LIBUSB_TRANSFER_NO_DEVICE;
    complete_after = 2;
    assert(ptp_usb_bulk_transfer(usb.handle, 0x81, bytes, sizeof(bytes), &count, 20000) == LIBUSB_ERROR_NO_DEVICE);
    reset();
    complete_after = 2;
    assert(ptp_usb_bulk_transfer(usb.handle, 0x02, bytes, sizeof(bytes), &count, 20000) == 0);
    assert(observed_timeout == 20000); /* Command writes remain bounded. */
    reset();
    complete_after = 2;
    actual_length = sizeof(bytes) + 1;
    assert(ptp_usb_bulk_transfer(usb.handle, 0x81, bytes, sizeof(bytes), &count, 20000) == LIBUSB_ERROR_OVERFLOW);
    assert(count == 0);
    reset();
    complete_after = 2;
    actual_length = -1;
    assert(ptp_usb_bulk_transfer(usb.handle, 0x81, bytes, sizeof(bytes), &count, 20000) == LIBUSB_ERROR_OVERFLOW);
    assert(count == 0);
    reset();
    assert(ptp_usb_bulk_transfer(usb.handle, 0x81, bytes, -1, &count, 20000) == LIBUSB_ERROR_INVALID_PARAM);
    assert(submitted == 0);
    reset();
    usb.session_abandoned = 0;
    allocation_failure = 1;
    assert(ptp_usb_bulk_transfer(usb.handle, 0x81, bytes, sizeof(bytes), &count, 20000) == LIBUSB_ERROR_NO_MEM);
    assert(usb.session_abandoned && submitted == 0);
    reset();
    usb.session_abandoned = 0;
    complete_after = 1;
    assert(ptp_usb_bulk_transfer(usb.handle, 0x02, bytes, sizeof(bytes), &count, 20000) == 0);
    assert(!usb.session_abandoned);
    reset();
    submit_error = LIBUSB_ERROR_NO_DEVICE;
    assert(ptp_usb_bulk_transfer(usb.handle, 0x81, bytes, sizeof(bytes), &count, 20000) == LIBUSB_ERROR_NO_DEVICE);
    assert(usb.session_abandoned && submitted == 0);
    LIBMTP_Set_Cancellable_Wait(&device, NULL, NULL);
    reset();
    assert(ptp_usb_bulk_transfer(usb.handle, 0x81, bytes, sizeof(bytes), &count, 20000) == 0);
    assert(observed_timeout == 20000 && submitted == 0); /* The next ordinary command is unchanged. */
    reset();
    cancel_after = 4;
    cancellation_error = LIBUSB_ERROR_IO;
    LIBMTP_Set_Cancellable_Wait(&device, should_cancel, &polls);
    assert(ptp_usb_bulk_transfer(usb.handle, 0x81, bytes, sizeof(bytes), &count, 20000) == LIBUSB_ERROR_IO);
    assert(usb.transfer_quarantined && !callback_delivered && polls == 4);
    /* The caller can release/reuse its memory before a late OS callback. */
    assert(pending->buffer != bytes);
    pending->status = LIBUSB_TRANSFER_CANCELLED;
    callback_delivered = 1;
    pending->callback(pending);
    usb.transfer_quarantined = 0; /* Simulate a new device for the second failure case. */
    reset();
    cancel_after = 4;
    suppress_callback = 1;
    assert(ptp_usb_bulk_transfer(usb.handle, 0x81, bytes, sizeof(bytes), &count, 20000) == LIBUSB_ERROR_INTERRUPTED);
    assert(usb.transfer_quarantined && !callback_delivered && cancelled == 1 && polls == 14);
    puts("Cancellable operation regression tests passed.");
}
