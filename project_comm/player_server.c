#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fmt.h"
#include "net/gcoap.h"
#include "net/utils.h"
#include "od.h"
#include "flash_utils.h"
#include "saul_reg.h"
#include "saul.h"
#include "gcoap_example.h"

#define ENABLE_DEBUG 0
#include "debug.h"

#ifndef SAUL_DEVICE_COUNT
#define SAUL_DEVICE_COUNT      (2)
#endif

static ssize_t _encode_link(const coap_resource_t *resource, char *buf,
                            size_t maxlen, coap_link_encoder_ctx_t *context);

static ssize_t _assign_color_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                     coap_request_ctx_t *ctx);
static ssize_t _set_winner_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                   coap_request_ctx_t *ctx);
static ssize_t _set_looser_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                   coap_request_ctx_t *ctx);

/* CoAP resources. Must be sorted by path (ASCII order). */
static const coap_resource_t _resources[] = {
    { "/assign_color", COAP_PUT, _assign_color_handler, NULL },
    { "/set_winner", COAP_PUT, _set_winner_handler, NULL },
    { "/set_looser", COAP_PUT, _set_looser_handler, NULL },
};

static const char *_link_params[] = {
    NULL,
    NULL,
    NULL,
};

static gcoap_listener_t _listener = {
    &_resources[0],
    ARRAY_SIZE(_resources),
    GCOAP_SOCKET_TYPE_UNDEF,
    _encode_link,
    NULL,
    NULL
};

/* Adds link format params to resource list */
static ssize_t _encode_link(const coap_resource_t *resource, char *buf,
                            size_t maxlen, coap_link_encoder_ctx_t *context)
{
    ssize_t res = gcoap_encode_link(resource, buf, maxlen, context);

    if (res > 0) {
        if (_link_params[context->link_pos]
            && (strlen(_link_params[context->link_pos]) < (maxlen - res))) {
            if (buf) {
                memcpy(buf + res, _link_params[context->link_pos],
                       strlen(_link_params[context->link_pos]));
            }
            return res + strlen(_link_params[context->link_pos]);
        }
    }

    return res;
}

void notify_observers(void)
{
    size_t len;
    uint8_t buf[CONFIG_GCOAP_PDU_BUF_SIZE];
    coap_pkt_t pdu;

    /* send Observe notification for /cli/stats */
    switch (gcoap_obs_init(&pdu, &buf[0], CONFIG_GCOAP_PDU_BUF_SIZE,
                           &_resources[0])) {
    case GCOAP_OBS_INIT_OK:
        DEBUG("gcoap_cli: creating /cli/stats notification\n");
        coap_opt_add_format(&pdu, COAP_FORMAT_TEXT);
        len = coap_opt_finish(&pdu, COAP_OPT_FINISH_PAYLOAD);
        len += fmt_u16_dec((char *)pdu.payload, req_count);
        gcoap_obs_send(&buf[0], len, &_resources[0]);
        break;
    case GCOAP_OBS_INIT_UNUSED:
        DEBUG("gcoap_cli: no observer for /cli/stats\n");
        break;
    case GCOAP_OBS_INIT_ERR:
        DEBUG("gcoap_cli: error initializing /cli/stats notification\n");
        break;
    }
}

static ssize_t _assign_color_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                     coap_request_ctx_t *ctx)
{
    /* TODO change LED color to payload */

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

static ssize_t _set_winner_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                   coap_request_ctx_t *ctx)
{
    /* TODO change LED color to green */

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

static ssize_t _set_looser_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                   coap_request_ctx_t *ctx)
{
    /* TODO change LED color to red */

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

void referee_server_init(void)
{
    gcoap_register_listener(&_listener);
}
