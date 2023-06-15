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
#include "net/cord/epsim.h"
#include "net/cord/common.h"
#include "net/gnrc/netif.h"
#include "net/sock/util.h"
#include "net/ipv6/addr.h"
#include "xtimer.h"

#define ENABLE_DEBUG 0
#include "debug.h"

#ifndef SAUL_DEVICE_COUNT
#define SAUL_DEVICE_COUNT      (2)
#endif

#define STARTUP_DELAY       (3U)    /* wait 3s before sending first request*/

static ssize_t _encode_link(const coap_resource_t *resource, char *buf,
                            size_t maxlen, coap_link_encoder_ctx_t *context);

static ssize_t _assign_color_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                     coap_request_ctx_t *ctx);
static ssize_t _set_to_winner_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                      coap_request_ctx_t *ctx);
static ssize_t _set_to_looser_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                      coap_request_ctx_t *ctx);

/* CoAP resources. Must be sorted by path (ASCII order). */
static const coap_resource_t _resources[] = {
    { "/assign_color", COAP_PUT, _assign_color_handler, NULL },
    { "/set_to_winner", COAP_PUT, _set_to_winner_handler, NULL },
    { "/set_to_looser", COAP_PUT, _set_to_looser_handler, NULL },
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

static ssize_t _assign_color_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                     coap_request_ctx_t *ctx)
{
    (void)ctx;
    /* TODO change LED color to payload */

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

static ssize_t _set_to_winner_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                      coap_request_ctx_t *ctx)
{
    (void)ctx;
    /* TODO change LED color to green */

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

static ssize_t _set_to_looser_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                      coap_request_ctx_t *ctx)
{
    (void)ctx;
    /* TODO change LED color to red */

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

void referee_server_init(void)
{
    char ep_str[CONFIG_SOCK_URLPATH_MAXLEN];
    uint16_t ep_port;

    puts("Simplified CoRE RD registration example\n");

    /* parse RD address information */
    sock_udp_ep_t rd_ep;

    if (sock_udp_name2ep(&rd_ep, RD_ADDR) < 0) {
        puts("error: unable to parse RD address from RD_ADDR variable");
        return;
    }

    /* if netif is not specified in addr and it's link local */
    if ((rd_ep.netif == SOCK_ADDR_ANY_NETIF) &&
        ipv6_addr_is_link_local((ipv6_addr_t *)&rd_ep.addr.ipv6)) {
        /* if there is only one interface we use that */
        if (gnrc_netif_numof() == 1) {
            rd_ep.netif = (uint16_t)gnrc_netif_iter(NULL)->pid;
        }
        /* if there are many it's an error */
        else {
            puts("error: must specify an interface for a link local address");
            return;
        }
    }

    if (rd_ep.port == 0) {
        rd_ep.port = COAP_PORT;
    }

    sock_udp_ep_fmt(&rd_ep, ep_str, &ep_port);

    /* register resource handlers with gcoap */
    gcoap_register_listener(&_listener);

    /* print RD client information */
    puts("epsim configuration:");
    printf(" RD address: [%s]:%u\n\n", ep_str, ep_port);

    xtimer_sleep(STARTUP_DELAY);

    while (1) {
        int res = cord_epsim_state();
        switch (res) {
        case CORD_EPSIM_OK:
            puts("state: registration active");
            break;
        case CORD_EPSIM_BUSY:
            puts("state: registration in progress");
            break;
        case CORD_EPSIM_ERROR:
        default:
            puts("state: not registered");
            break;
        }

        printf("updating registration with RD [%s]:%u\n", ep_str, ep_port);
        res = cord_epsim_register(&rd_ep);
        if (res == CORD_EPSIM_BUSY) {
            puts("warning: registration already in progress");
        }
        else if (res == CORD_EPSIM_ERROR) {
            puts("error: unable to trigger simple registration process");
        }
        xtimer_sleep(CONFIG_CORD_UPDATE_INTERVAL);
    }
}
