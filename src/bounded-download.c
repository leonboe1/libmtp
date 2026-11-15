/* Bounded downloads. Distributed under libmtp's LGPL-2.0-or-later license. */
#include "config.h"
#include "libusb-glue.h"
#include "device-flags.h"
#include <stdlib.h>
#include <time.h>

/* Legacy GetObject cannot safely cancel from every progress callback site. */
struct bounded_download_fallback {
    LIBMTP_progressfunc_t callback;
    void const *data;
    int cancelled;
};

/* Record cancellation while allowing the whole legacy response to finish. */
static int fallback_progress(uint64_t sent, uint64_t total, void const * const data)
{
    struct bounded_download_fallback *context = (struct bounded_download_fallback *)data;
    if (context->callback(sent, total, context->data)) context->cancelled = 1;
    return 0;
}

/* Keep consuming the current response after a local writer error, so the next
 * command never sees leftover file data. No full-chunk allocation is needed. */
struct bounded_download_sink {
    PTPParams *params;
    PTPDataHandler *destination;
    uint64_t received;
    uint32_t limit;
    uint16_t result;
    PTP_USB *usb;
    uint64_t offset;
    uint64_t total;
    int cancelled;
    int reported;
    struct timespec last_report;
};

static uint16_t bounded_download_put(PTPParams *params, void *private,
                                      unsigned long length, unsigned char *bytes)
{
    struct bounded_download_sink *sink = private;
    (void)params;
    sink->received += length;
    if (sink->received > sink->limit) sink->result = PTP_ERROR_IO;
    if (sink->result == PTP_RC_OK) {
        uint16_t local_result = sink->destination->putfunc(sink->params, sink->destination->priv, length, bytes);
        /* The USB response is still consumed. A local disk failure must not be
         * classified as PTP_ERROR_IO, which would invalidate a healthy session. */
        if (local_result != PTP_RC_OK)
            sink->result = local_result == PTP_ERROR_CANCEL ? PTP_ERROR_CANCEL : PTP_RC_GeneralError;
    }
    if (sink->result == PTP_RC_OK && !sink->cancelled && sink->usb->current_transfer_callback) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - sink->last_report.tv_sec)
            + (double)(now.tv_nsec - sink->last_report.tv_nsec) / 1000000000.0;
        if (!sink->reported || elapsed >= 1.0 / 30.0 || sink->received == sink->limit) {
            /* Report actual destination bytes while retaining ownership of the
             * whole response. A callback may request cancellation but must never
             * stop the data handler before the response has been consumed. */
            sink->cancelled = sink->usb->current_transfer_callback(
                sink->offset + sink->received, sink->total,
                sink->usb->current_transfer_callback_data) != 0;
            sink->last_report = now;
            sink->reported = 1;
        }
    }
    return PTP_RC_OK;
}

/* Finish each partial response before honoring cancellation.
 * This keeps the session synchronized without draining a multi-GB GetObject. */
uint16_t ptp_getobject_to_handler_bounded(PTPParams *params, uint32_t id,
                                        uint64_t size, PTPDataHandler *handler)
{
    PTP_USB *usb = (PTP_USB *)params->data;
    const int use64 = ptp_operation_issupported(params, PTP_OC_ANDROID_GetPartialObject64);
    /* Amortize request latency while retaining a bounded cancellation wait. */
    const uint32_t chunk_size = 32 * 1024 * 1024;
    uint64_t offset = 0;
    uint16_t result = PTP_RC_OK;
    int callback_active = usb->callback_active;

    /* Choose compatibility fallback before writing any bytes. Never truncate an
     * offset or restart a failed partial transfer using a whole-object request. */
    if (!use64 && (!ptp_operation_issupported(params, PTP_OC_GetPartialObject) || size > UINT32_MAX)) {
        struct bounded_download_fallback context = {
            usb->current_transfer_callback, usb->current_transfer_callback_data, 0
        };
        if (context.callback && context.callback(0, size, context.data))
            return PTP_ERROR_CANCEL;
        if (context.callback) {
            usb->current_transfer_callback = fallback_progress;
            usb->current_transfer_callback_data = &context;
        }
        result = ptp_getobject_to_handler(params, id, handler);
        usb->current_transfer_callback = context.callback;
        usb->current_transfer_callback_data = context.data;
        return result == PTP_RC_OK && context.cancelled ? PTP_ERROR_CANCEL : result;
    }

    usb->callback_active = 0;
    usb->current_transfer_total = size;
    usb->current_transfer_complete = 0;
    for (;;) {
        if (usb->current_transfer_callback &&
            usb->current_transfer_callback(offset, size, usb->current_transfer_callback_data)) {
            result = PTP_ERROR_CANCEL;
            break;
        }
        if (offset == size) break;

        uint32_t count = size - offset > chunk_size ? chunk_size : (uint32_t)(size - offset);

        /* Preserve LIBMTP_GetPartialObject's Samsung final-packet workaround. */
        if ((params->device_flags & DEVICE_FLAG_SAMSUNG_OFFSET_BUG) &&
            count % PTP_USB_BULK_HS_MAX_PACKET_LEN_READ ==
                PTP_USB_BULK_HS_MAX_PACKET_LEN_READ - PTP_USB_BULK_HDR_LEN)
            count--;

        struct bounded_download_sink sink = {
            .params = params, .destination = handler, .limit = count,
            .result = PTP_RC_OK, .usb = usb, .offset = offset, .total = size
        };
        PTPDataHandler bounded_handler = {0};
        bounded_handler.priv = &sink;
        bounded_handler.putfunc = bounded_download_put;
        result = use64 ? ptp_android_getpartialobject64_to_handler(params, id, offset, count, &bounded_handler)
                       : ptp_getpartialobject_to_handler(params, id, (uint32_t)offset, count, &bounded_handler);
        if (result == PTP_RC_OK)
            result = sink.received == 0 ? PTP_ERROR_IO : sink.result;
        if (result != PTP_RC_OK) break;
        offset += sink.received;
        usb->current_transfer_complete = offset;
        if (sink.cancelled) {
            result = PTP_ERROR_CANCEL;
            break;
        }
    }
    usb->callback_active = callback_active;
    return result;
}
