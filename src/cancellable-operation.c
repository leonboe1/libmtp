/* Cancellable USB waits. Distributed under libmtp's LGPL-2.0-or-later license. */
#include "config.h"
#include "libusb-glue.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Callers serialize operations per device; TLS isolates scopes on other threads. */
static _Thread_local PTP_USB *operation_usb;
static _Thread_local int (*operation_cancelled)(void *);
static _Thread_local void *operation_context;

/* A scoped override: NULL restores ordinary finite USB waits. */
void LIBMTP_Set_Cancellable_Wait(LIBMTP_mtpdevice_t *device,
                                int (*cancelled)(void *), void *context)
{
    operation_usb = cancelled ? (PTP_USB *)device->usbinfo : NULL;
    operation_cancelled = cancelled;
    operation_context = context;
}

/* The callback owns completion until libusb has stopped touching the caller's buffer. */
static void complete_transfer(struct libusb_transfer *transfer)
{
    int *completed = transfer->user_data;
    *completed = 1;
}

/* Intercepts only the active operation, preserving ordinary USB behavior elsewhere. */
int ptp_usb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint,
                        unsigned char *data, int length, int *transferred,
                        unsigned int timeout)
{
    if (!operation_cancelled || operation_usb->handle != handle)
        return libusb_bulk_transfer(handle, endpoint, data, length, transferred, timeout);
    *transferred = 0;
    if (length < 0) {
        operation_usb->session_abandoned = 1;
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    if (operation_usb->transfer_quarantined) return LIBUSB_ERROR_IO;
    if (operation_cancelled(operation_context)) {
        operation_usb->session_abandoned = 1;
        return LIBUSB_ERROR_INTERRUPTED;
    }

    struct libusb_transfer *transfer = libusb_alloc_transfer(0);
    if (!transfer) {
        operation_usb->session_abandoned = 1;
        return LIBUSB_ERROR_NO_MEM;
    }
    int *completed = calloc(1, sizeof(*completed));
    unsigned char *owned_data = malloc(length > 0 ? (size_t)length : 1);
    if (!completed || !owned_data) {
        operation_usb->session_abandoned = 1;
        free(completed);
        free(owned_data);
        libusb_free_transfer(transfer);
        return LIBUSB_ERROR_NO_MEM;
    }
    if (!(endpoint & LIBUSB_ENDPOINT_IN)) memcpy(owned_data, data, length);
    int cancel_requested = 0;
    int event_error = 0;
    struct timespec cancel_started = {0};
    /* No read deadline: the device can legitimately remain silent for the whole copy.
     * Writes retain their existing finite timeout. Neither path blocks cancellation. */
    libusb_fill_bulk_transfer(transfer, handle, endpoint, owned_data, length,
                             complete_transfer, completed,
                             endpoint & LIBUSB_ENDPOINT_IN ? 0 : timeout);
    int result = libusb_submit_transfer(transfer);
    if (result != LIBUSB_SUCCESS) {
        operation_usb->session_abandoned = 1;
        libusb_free_transfer(transfer);
        free(owned_data);
        free(completed);
        return result;
    }
    while (!*completed) {
        struct timeval interval = {0, 100000};
        int status = LIBMTP_Handle_Events_Timeout_Completed(&interval, completed);
        if (*completed) break; /* A known result wins a simultaneous cancellation. */
        if (status < 0 && status != LIBUSB_ERROR_INTERRUPTED) event_error = status;
        if (!cancel_requested && (event_error || operation_cancelled(operation_context))) {
            operation_usb->session_abandoned = 1;
            int cancellation = libusb_cancel_transfer(transfer);
            if (cancellation != LIBUSB_SUCCESS && cancellation != LIBUSB_ERROR_NOT_FOUND) {
                /* The OS still owns this transfer and may callback arbitrarily late.
                 * Quarantine the device and retain these allocations until process exit:
                 * neither returning caller memory nor closing its handle is safe here.
                 * The completion callback only touches the retained heap flag. */
                operation_usb->transfer_quarantined = 1;
                return cancellation;
            }
            cancel_requested = 1;
            clock_gettime(CLOCK_MONOTONIC, &cancel_started);
        }
        if (cancel_requested) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (double)(now.tv_sec - cancel_started.tv_sec)
                + (double)(now.tv_nsec - cancel_started.tv_nsec) / 1000000000.0;
            /* This grace period starts only AFTER cancellation, never during useful work.
             * A broken event pump must not prevent the user from leaving the operation. */
            if (elapsed >= 1.0) {
                operation_usb->transfer_quarantined = 1;
                return event_error ? event_error : LIBUSB_ERROR_INTERRUPTED;
            }
        }
    }
    if (transfer->actual_length < 0 || transfer->actual_length > length)
        transfer->status = LIBUSB_TRANSFER_OVERFLOW;
    if (transfer->status != LIBUSB_TRANSFER_OVERFLOW) {
        *transferred = transfer->actual_length;
        if ((endpoint & LIBUSB_ENDPOINT_IN) && transfer->actual_length > 0)
            memcpy(data, owned_data, (size_t)transfer->actual_length);
    }
    switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED: result = LIBUSB_SUCCESS; break;
    case LIBUSB_TRANSFER_TIMED_OUT: result = LIBUSB_ERROR_TIMEOUT; break;
    case LIBUSB_TRANSFER_CANCELLED: result = event_error ? event_error : LIBUSB_ERROR_INTERRUPTED; break;
    case LIBUSB_TRANSFER_NO_DEVICE: result = LIBUSB_ERROR_NO_DEVICE; break;
    case LIBUSB_TRANSFER_STALL: result = LIBUSB_ERROR_PIPE; break;
    case LIBUSB_TRANSFER_OVERFLOW: result = LIBUSB_ERROR_OVERFLOW; break;
    default: result = LIBUSB_ERROR_IO; break;
    }
    if (result != LIBUSB_SUCCESS) operation_usb->session_abandoned = 1;
    libusb_free_transfer(transfer);
    free(owned_data);
    free(completed);
    return result;
}
