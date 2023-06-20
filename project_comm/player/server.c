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
#include "server.h"

#define ENABLE_DEBUG 0
#include "debug.h"

#ifndef SAUL_DEVICE_COUNT
#define SAUL_DEVICE_COUNT      (2)
#endif

#define STARTUP_DELAY       (3U)    /* wait 3s before sending first request*/

static uint32_t pushup_count = 0;

static ssize_t _encode_link(const coap_resource_t *resource, char *buf,
                            size_t maxlen, coap_link_encoder_ctx_t *context);

static ssize_t _assign_player_id_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                         coap_request_ctx_t *ctx);
static ssize_t _count_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                              coap_request_ctx_t *ctx);
static ssize_t _set_to_winner_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                      coap_request_ctx_t *ctx);
static ssize_t _set_to_looser_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                      coap_request_ctx_t *ctx);
static ssize_t _fake_pushup_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                    coap_request_ctx_t *ctx);

/* CoAP resources. Must be sorted by path (ASCII order). */
static const coap_resource_t _resources[] = {
    { "/assign_player_id", COAP_PUT, _assign_player_id_handler, NULL },
    { "/count", COAP_GET, _count_handler, NULL },
    { "/set_to_winner", COAP_POST, _set_to_winner_handler, NULL },
    { "/set_to_looser", COAP_POST, _set_to_looser_handler, NULL },
    { "/fake_pushup", COAP_POST, _fake_pushup_handler, NULL },
};

static const char *_link_params[] = {
    ";rt=\"pushups_player\"",
    ";ct=0;rt=\"pushups_player\";obs",
    ";rt=\"pushups_player\"",
    ";rt=\"pushups_player\"",
    ";rt=\"pushups_player\"",
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

void notify_count_observers(void)
{
    size_t len;
    uint8_t buf[CONFIG_GCOAP_PDU_BUF_SIZE];
    coap_pkt_t pdu;

    /* send Observe notification for /count */
    switch (gcoap_obs_init(&pdu, &buf[0], CONFIG_GCOAP_PDU_BUF_SIZE,
                           &_resources[1])) {
    case GCOAP_OBS_INIT_OK:
        printf("Creating /count notification\n");
        coap_opt_add_format(&pdu, COAP_FORMAT_TEXT);
        len = coap_opt_finish(&pdu, COAP_OPT_FINISH_PAYLOAD);
        len += fmt_u32_dec((char *)pdu.payload, pushup_count);
        gcoap_obs_send(&buf[0], len, &_resources[1]);
        break;
    case GCOAP_OBS_INIT_UNUSED:
        printf("No observer for /count\n");
        break;
    case GCOAP_OBS_INIT_ERR:
        printf("Error initializing /count notification\n");
        break;
    }
}

void set_led_color(led_color_t color)
{
    saul_reg_t *dev_red = saul_reg_find_nth(0);
    saul_reg_t *dev_green = saul_reg_find_nth(1);
    saul_reg_t *dev_blue = saul_reg_find_nth(2);

    phydat_t data_off = {
        .val = { 0 }
    };
    phydat_t data_on = {
        .val = { 1 }
    };

    /* turn all off first */
    saul_reg_write(dev_red, &data_off);
    saul_reg_write(dev_green, &data_off);
    saul_reg_write(dev_blue, &data_off);

    /* turn on the required led color */
    switch (color) {
    case LED_COLOR_OFF: break;
    case LED_COLOR_RED: saul_reg_write(dev_red, &data_on); break;
    case LED_COLOR_GREEN: saul_reg_write(dev_green, &data_on); break;
    case LED_COLOR_BLUE: saul_reg_write(dev_blue, &data_on); break;
    }
}

static ssize_t _assign_player_id_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                         coap_request_ctx_t *ctx)
{
    (void)ctx;

    printf("\n\n SET PLAYER ID\n\n");

    // TODO set color based on id in payload

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

static ssize_t _count_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                              coap_request_ctx_t *ctx)
{
    (void)ctx;

    gcoap_resp_init(pdu, buf, len, COAP_CODE_CONTENT);
    coap_opt_add_format(pdu, COAP_FORMAT_TEXT);
    size_t resp_len = coap_opt_finish(pdu, COAP_OPT_FINISH_PAYLOAD);

    /* write the response buffer with the request count value */
    resp_len += fmt_u32_dec((char *)pdu->payload, pushup_count);
    return resp_len;
}

static ssize_t _set_to_winner_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                      coap_request_ctx_t *ctx)
{
    (void)ctx;
    set_led_color(LED_COLOR_GREEN);

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

static ssize_t _set_to_looser_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                      coap_request_ctx_t *ctx)
{
    (void)ctx;
    set_led_color(LED_COLOR_RED);

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

static ssize_t _fake_pushup_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                    coap_request_ctx_t *ctx)
{
    (void)ctx;

    pushup_count++;
    notify_count_observers();

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

void player_server_init(void)
{
    char ep_str[CONFIG_SOCK_URLPATH_MAXLEN];
    uint16_t ep_port;

    puts("Simplified CoRE RD registration example\n");

    /* parse RD address information */
    sock_udp_ep_t rd_ep;

    if (sock_udp_name2ep(&rd_ep, "[ff02::1]") < 0) {
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
