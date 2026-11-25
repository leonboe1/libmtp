/* Exercise response-only device rejections through the USB data reader. */
#include "config.h"
#include "libusb-glue.h"
#include <assert.h>
#include <stdlib.h>

static PTPUSBBulkContainer reply;
static unsigned int reply_size;
static uint32_t fresh_size;
static int info_queries;

/* Supply the first USB read for each request through the packet buffer. */
static uint16_t send_request(PTPParams *params, PTPContainer *command, int phase)
{
    PTPUSBBulkContainer packet = {0};
    unsigned int size;
    assert(phase == PTP_DP_GETDATA && !params->response_packet);
    if (command->Code == PTP_OC_MTP_GetObjectPropValue) {
        assert(command->Param2 == PTP_OPC_ObjectSize);
        packet = reply;
        size = reply_size;
    } else {
        assert(command->Code == PTP_OC_GetObjectInfo && info_queries == 0);
        info_queries++;
        size = PTP_USB_BULK_HDR_LEN + 60;
        packet.length = htod32(size);
        packet.type = htod16(PTP_USB_CONTAINER_DATA);
        packet.code = htod16(command->Code);
        packet.trans_id = htod32(command->Transaction_ID);
        htod16a(packet.payload.data + 4, PTP_OFC_Undefined);
        htod32a(packet.payload.data + 8, fresh_size);
    }
    params->response_packet = malloc(size);
    assert(params->response_packet);
    memcpy(params->response_packet, &packet, size);
    params->response_packet_size = size;
    return PTP_RC_OK;
}

/* Only the ObjectInfo data phase has a separate final response. */
static uint16_t finish_request(PTPParams *params, PTPContainer *command)
{
    assert(command->Code == PTP_OC_GetObjectInfo && !params->response_packet);
    command->Code = PTP_RC_OK;
    return PTP_RC_OK;
}

/* Verify a listed empty file, including the fresh ObjectInfo size when used. */
static void check_verification(uint16_t expected, int expected_info_queries)
{
    uint16_t operations[] = { PTP_OC_GetPartialObject, PTP_OC_MTP_GetObjectPropValue };
    PTP_USB usb = { .inep_maxpacket = 512 };
    PTPParams params = {
        .data = &usb, .transaction_id = 42,
        .sendreq_func = send_request, .getdata_func = ptp_usb_getdata,
        .getresp_func = finish_request,
        .deviceinfo = { .Operations = operations, .Operations_len = 2 }
    };
    PTPDataHandler destination = {0};
    info_queries = 0;
    uint16_t result = ptp_getobject_to_handler_bounded(&params, 7, 0, &destination);
    assert(result == expected);
    assert(info_queries == expected_info_queries && !params.response_packet);
}

int main(void)
{
    reply_size = PTP_USB_BULK_HDR_LEN;
    reply.length = htod32(reply_size);
    reply.type = htod16(PTP_USB_CONTAINER_RESPONSE);
    reply.trans_id = htod32(42);
    uint16_t unsupported[] = { PTP_RC_OperationNotSupported,
        PTP_RC_MTP_Invalid_ObjectPropCode, PTP_RC_MTP_ObjectProp_Not_Supported };
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        reply.code = htod16(unsupported[i]);
        fresh_size = 0;
        check_verification(PTP_RC_OK, 1);
        fresh_size = 9;
        check_verification(PTP_RC_IncompleteTransfer, 1);
    }

    /* A rejection may carry response parameters. */
    reply_size += sizeof(uint32_t);
    reply.length = htod32(reply_size);
    reply.code = htod16(PTP_RC_AccessDenied);
    check_verification(PTP_RC_AccessDenied, 0);
    reply.code = htod16(PTP_OC_GetObjectInfo);
    check_verification(PTP_ERROR_DATA_EXPECTED, 0);
    reply.code = htod16(PTP_RC_OK);
    check_verification(PTP_ERROR_DATA_EXPECTED, 0);

    reply.code = htod16(PTP_RC_MTP_ObjectProp_Not_Supported);
    reply.trans_id = htod32(41);
    check_verification(PTP_ERROR_DATA_EXPECTED, 0);
    reply.trans_id = htod32(42);
    reply_size--;
    check_verification(PTP_ERROR_DATA_EXPECTED, 0);
    reply.length = htod32(reply_size);
    check_verification(PTP_ERROR_DATA_EXPECTED, 0);
    return 0;
}
