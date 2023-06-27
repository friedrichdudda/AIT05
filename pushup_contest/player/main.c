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
#include "thread.h"
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

#define SAUL_LED_RED_ID (0)
#define SAUL_LED_GREEN_ID (1)
#define SAUL_LED_BLUE_ID (2)
#define SAUL_ACCELEROMETER_NAME ("mma8x5x")

typedef enum {
    LED_COLOR_OFF,
    LED_COLOR_RED,
    LED_COLOR_GREEN,
    LED_COLOR_BLUE,
} led_color_t;

static uint32_t pushup_count = 0;
static led_color_t player_color = 0;
static bool reset = false;

static void run_pushup_detection(void);
void *pushup_detection_thread(void *arg);
char pushup_detection_thread_stack[THREAD_STACKSIZE_MAIN];


static ssize_t _encode_link(const coap_resource_t *resource, char *buf,
                            size_t maxlen, coap_link_encoder_ctx_t *context);

static ssize_t _assign_color_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                     coap_request_ctx_t *ctx);
static ssize_t _start_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                              coap_request_ctx_t *ctx);
static ssize_t _count_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                              coap_request_ctx_t *ctx);
static ssize_t _set_to_winner_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                      coap_request_ctx_t *ctx);
static ssize_t _set_to_looser_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                      coap_request_ctx_t *ctx);
static ssize_t _fake_pushup_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                    coap_request_ctx_t *ctx);
static ssize_t _reset_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                              coap_request_ctx_t *ctx);

/* CoAP resources. Must be sorted by path (ASCII order). */
static const coap_resource_t _resources[] = {
    { "/assign_color", COAP_PUT, _assign_color_handler, NULL },
    { "/start", COAP_POST, _start_handler, NULL },
    { "/count", COAP_GET, _count_handler, NULL },
    { "/set_to_winner", COAP_POST, _set_to_winner_handler, NULL },
    { "/set_to_looser", COAP_POST, _set_to_looser_handler, NULL },
    { "/fake_pushup", COAP_POST, _fake_pushup_handler, NULL },
    { "/reset", COAP_POST, _reset_handler, NULL },
};

static const char *_link_params[] = {
    ";rt=\"pushups_player\"",
    ";rt=\"pushups_player\"",
    ";ct=0;rt=\"pushups_player\";obs",
    ";rt=\"pushups_player\"",
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
                           &_resources[2])) {
    case GCOAP_OBS_INIT_OK:
        printf("Creating /count notification\n");
        coap_opt_add_format(&pdu, COAP_FORMAT_TEXT);
        len = coap_opt_finish(&pdu, COAP_OPT_FINISH_PAYLOAD);
        len += fmt_u32_dec((char *)pdu.payload, pushup_count);
        gcoap_obs_send(&buf[0], len, &_resources[2]);
        break;
    case GCOAP_OBS_INIT_UNUSED:
        printf("No observer for /count\n");
        break;
    case GCOAP_OBS_INIT_ERR:
        printf("Error initializing /count notification\n");
        break;
    }
}

static void set_led_color(led_color_t color)
{
    saul_reg_t *red = saul_reg_find_nth(SAUL_LED_RED_ID);
    saul_reg_t *green = saul_reg_find_nth(SAUL_LED_GREEN_ID);
    saul_reg_t *blue = saul_reg_find_nth(SAUL_LED_BLUE_ID);

    phydat_t off = {
        .val = { 0 }
    };
    phydat_t on = {
        .val = { 1 }
    };

    /* turn all off first */
    saul_reg_write(red, &off);
    saul_reg_write(green, &off);
    saul_reg_write(blue, &off);

    /* turn on the required led color */
    switch (color) {
    case LED_COLOR_OFF: break;
    case LED_COLOR_RED: saul_reg_write(red, &on); break;
    case LED_COLOR_GREEN: saul_reg_write(green, &on); break;
    case LED_COLOR_BLUE: saul_reg_write(blue, &on); break;
    }
}

static ssize_t _assign_color_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                     coap_request_ctx_t *ctx)
{
    (void)ctx;

    printf("COAP: Set Color\n");

    printf("PLAYER COLOR: %d\n", atoi((char *)pdu->payload));

    player_color = (led_color_t)atoi((char *)pdu->payload);
    set_led_color(player_color);

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

static ssize_t _start_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                              coap_request_ctx_t *ctx)
{
    (void)ctx;

    printf("COAP: Start\n");

    reset = false;

    /* run pushup detection in its own thread */
    thread_create(pushup_detection_thread_stack, sizeof(pushup_detection_thread_stack),
                  THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST,
                  pushup_detection_thread, NULL, "pushup_detection_thread");

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

static ssize_t _count_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                              coap_request_ctx_t *ctx)
{
    (void)ctx;
    printf("COAP: Count\n");

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

    printf("COAP: Set to winner\n");

    thread_create(pushup_detection_thread_stack, sizeof(pushup_detection_thread_stack),
                  THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST,
                  pushup_detection_thread, NULL, "pushup_detection_thread");

    while (!reset) {
        set_led_color(LED_COLOR_OFF);
        xtimer_msleep(200);
        set_led_color(player_color);
        xtimer_msleep(200);
    }

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

static ssize_t _set_to_looser_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                      coap_request_ctx_t *ctx)
{
    printf("COAP: Set to looser\n");
    (void)ctx;
    set_led_color(LED_COLOR_OFF);

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

static ssize_t _fake_pushup_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                                    coap_request_ctx_t *ctx)
{
    (void)ctx;
    printf("COAP: Fake pushup\n");

    set_led_color(LED_COLOR_OFF);

    xtimer_msleep(1000);

    set_led_color(LED_COLOR_BLUE);

    pushup_count++;
    notify_count_observers();

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

static ssize_t _reset_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len,
                              coap_request_ctx_t *ctx)
{
    (void)ctx;

    reset = true;
    pushup_count = 0;

    notify_count_observers();

    return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
}

void *pushup_detection_thread(void *arg)
{
    printf("Started pushup detection thread\n");
    (void)arg;

    run_pushup_detection();

    return NULL;
}

static void run_pushup_detection(void)
{
    printf("Started pushup detection\n");
    saul_reg_t *dev = saul_reg_find_name(SAUL_ACCELEROMETER_NAME);

    int *data_storage = malloc(sizeof(int) * 1000);

    int sum = 0;
    int cnt = 0;
    bool down_detected = false;

    set_led_color(player_color);
    phydat_t res;
    saul_reg_read(dev, &res);

    int start_value = res.val[2];

    while (true) {
        if (reset) {
            printf("PUSHUP_DETECTION_THREAD_YIELDS\n");
            break;
        }
        for (int i = 0; i < 150; i++) {
            if (reset) {
                printf("PUSHUP_DETECTION_THREAD_YIELDS\n");
                break;
            }

            saul_reg_read(dev, &res);

            // data_storage[0][i] = res.val[0];
            // data_storage[1][i] = res.val[1];
            data_storage[i] = res.val[2];

            printf("%d\n", res.val[2] - start_value);
            sum += (res.val[2] - start_value);
            cnt++;


            if (sum < -250) {
                printf("\ndown\n");
                sum = 0;
                down_detected = true;
            }
            else if (sum > 250) {
                printf("\nup\n");
                sum = 0;
                if (down_detected) {
                    printf("\n****Repetition****\n\n");
                    down_detected = false;
                    set_led_color(LED_COLOR_OFF);
                    cnt = 0;

                    /* update pushups counter and notify observers */
                    pushup_count++;
                    notify_count_observers();
                }
            }
            if (cnt == 4) {
                cnt = 0;
                sum = 0;
                set_led_color(player_color);
            }

            xtimer_msleep(200);
        }
        free(data_storage);
        printf("\n");
    }

    thread_yield();
}

int main(void)
{
    char ep_str[CONFIG_SOCK_URLPATH_MAXLEN];
    uint16_t ep_port;

    set_led_color(LED_COLOR_BLUE);

    puts("Simplified CoRE RD registration example\n");

    /* parse RD address information */
    sock_udp_ep_t rd_ep;

    if (sock_udp_name2ep(&rd_ep, "[ff02::1]") < 0) {
        puts("error: unable to parse RD address from RD_ADDR variable");
        return -1;
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
            return -1;
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

    return 0;
}
