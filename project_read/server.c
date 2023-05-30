/*
 * Copyright (c) 2015-2017 Ken Bannister. All rights reserved.
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     examples
 * @{
 *
 * @file
 * @brief       gcoap CLI support
 *
 * @author      Ken Bannister <kb2ma@runbox.com>
 * @author      Hauke Petersen <hauke.petersen@fu-berlin.de>
 *
 * @}
 */

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

#if IS_USED(MODULE_GCOAP_DTLS)
#include "net/credman.h"
#include "net/dsm.h"
#include "tinydtls_keys.h"

/* Example credential tag for credman. Tag together with the credential type needs to be unique. */
#define GCOAP_DTLS_CREDENTIAL_TAG 10

static const uint8_t psk_id_0[] = PSK_DEFAULT_IDENTITY;
static const uint8_t psk_key_0[] = PSK_DEFAULT_KEY;
static const credman_credential_t credential = {
    .type = CREDMAN_TYPE_PSK,
    .tag = GCOAP_DTLS_CREDENTIAL_TAG,
    .params = {
        .psk = {
            .key = { .s = psk_key_0, .len = sizeof(psk_key_0) - 1, },
            .id = { .s = psk_id_0, .len = sizeof(psk_id_0) - 1, },
        }
    },
};
#endif

#ifndef SAUL_DEVICE_COUNT
#define SAUL_DEVICE_COUNT      (2)
#endif

static ssize_t _encode_link(const coap_resource_t *resource, char *buf,
                            size_t maxlen, coap_link_encoder_ctx_t *context);

/* CoAP resources. Must be sorted by path (ASCII order). */
static coap_resource_t _resources[SAUL_DEVICE_COUNT];

static char *_link_params[SAUL_DEVICE_COUNT];

static gcoap_listener_t _listener;

/* Adds link format params to resource list */
static ssize_t _encode_link(const coap_resource_t *resource, char *buf,
                            size_t maxlen, coap_link_encoder_ctx_t *context) {
    ssize_t res = gcoap_encode_link(resource, buf, maxlen, context);
    if (res > 0) {
        if (_link_params[context->link_pos]
                && (strlen(_link_params[context->link_pos]) < (maxlen - res))) {
            if (buf) {
                memcpy(buf+res, _link_params[context->link_pos],
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

size_t phydat_to_str(const phydat_t *data, const uint8_t dim, char* buf, const size_t buf_len)
{
    int buf_pos = 0;
    if (data == NULL || dim > PHYDAT_DIM) {
        buf_pos += snprintf(buf + buf_pos, buf_len, "Unable to display data object\n");
        return buf_pos;
    }

    if (data->unit == UNIT_TIME) {
        assert(dim == 3);
        buf_pos += snprintf(buf + buf_pos, buf_len,"%02d:%02d:%02d\n",
               data->val[2], data->val[1], data->val[0]);
        return buf_pos;
    }
    if (data->unit == UNIT_DATE) {
        assert(dim == 3);
        buf_pos += snprintf(buf + buf_pos, buf_len,"%04d-%02d-%02d\n",
               data->val[2], data->val[1], data->val[0]);
        return buf_pos;
    }

    for (uint8_t i = 0; i < dim; i++) {
        char scale_prefix;

        switch (data->unit) {
        case UNIT_UNDEF:
        case UNIT_NONE:
        case UNIT_M2:
        case UNIT_M3:
        case UNIT_PERCENT:
        case UNIT_TEMP_C:
        case UNIT_TEMP_F:
        case UNIT_DBM:
            /* no string conversion */
            scale_prefix = '\0';
            break;
        default:
            scale_prefix = phydat_prefix_from_scale(data->scale);
        }

        if (dim > 1) {
            buf_pos += snprintf(buf + buf_pos, buf_len,"[%u] ", (unsigned int)i);
        }
        if (scale_prefix) {
            buf_pos += snprintf(buf + buf_pos, buf_len,"%11d %c", (int)data->val[i], scale_prefix);
        }
        else if (data->scale == 0) {
            buf_pos += snprintf(buf + buf_pos, buf_len,"%11d ", (int)data->val[i]);
        }
        else if ((data->scale > -6) && (data->scale < 0)) {
            char num[9];
            size_t len = fmt_s16_dfp(num, data->val[i], data->scale);
            assert(len < 9);
            num[len] = '\0';
            buf_pos += snprintf(buf + buf_pos, buf_len,"%11s ", num);
        }
        else {
            char num[12];
            snprintf(num, sizeof(num), "%ie%i",
                     (int)data->val[i], (int)data->scale);
            buf_pos += snprintf(buf + buf_pos, buf_len,"%11s ", num);
        }

        if ((data->unit != UNIT_NONE) && (data->unit != UNIT_UNDEF)
            && (data->unit != UNIT_BOOL)) {
            buf_pos += snprintf(buf + buf_pos, buf_len,"%s", phydat_unit_to_str(data->unit));
        }
    }

    return buf_pos;
}

/* static const char *_devname(saul_reg_t *dev) {
    if (dev->name == NULL) {
        return "(no name)";
    } else {
        return dev->name;
    }
} */
static ssize_t _saul_get_handler(coap_pkt_t *pdu, uint8_t *buf, size_t len, coap_request_ctx_t *ctx)
{
    (void)ctx;
    /* write the RIOT board name in the response buffer */
    saul_reg_t *dev = saul_reg_find_name(ctx->resource->path + 1);

    int buf_pos = 0;
    
    /* read coap method type in packet */
    unsigned method_flag = coap_method2flag(coap_get_code_detail(pdu));
    switch (method_flag) {
        case COAP_GET: {
            gcoap_resp_init(pdu, buf, len, COAP_CODE_CONTENT);
            coap_opt_add_format(pdu, COAP_FORMAT_TEXT);
            size_t resp_len = coap_opt_finish(pdu, COAP_OPT_FINISH_PAYLOAD);
            phydat_t res;
            int dim = saul_reg_read(dev, &res);
            buf_pos = phydat_to_str(&res, dim, (char*)pdu->payload, pdu->payload_len);
            return resp_len + buf_pos;
        }
        case COAP_PUT: {
            phydat_t data;
            data.val[0] = 1;
            saul_reg_write(dev, &data);
            return gcoap_response(pdu, buf, len, COAP_CODE_CHANGED);
        }
    }
    
    return 0;
}

void server_init(void)
{
#if IS_USED(MODULE_GCOAP_DTLS)
    int res = credman_add(&credential);
    if (res < 0 && res != CREDMAN_EXIST) {
        /* ignore duplicate credentials */
        printf("gcoap: cannot add credential to system: %d\n", res);
        return;
    }
    sock_dtls_t *gcoap_sock_dtls = gcoap_get_sock_dtls();
    res = sock_dtls_add_credential(gcoap_sock_dtls, GCOAP_DTLS_CREDENTIAL_TAG);
    if (res < 0) {
        printf("gcoap: cannot add credential to DTLS sock: %d\n", res);
    }
#endif

    int i = 0;
    for (saul_reg_t *dev = saul_reg; dev != NULL; dev = dev->next) {
        // Create the device type string
        // const char *dev_type = saul_class_to_str(dev->driver->type);

        // Create the resource_uri string
        int resource_uri_len = snprintf(NULL, 0, "/%s", dev->name) + 1;
        char *resource_uri = malloc(sizeof(char)*resource_uri_len);
        snprintf(resource_uri, resource_uri_len, "/%s", dev->name);

        // Init array items inside resources and link_params arrays
        _resources[i] = (coap_resource_t){ resource_uri, COAP_GET | COAP_PUT, _saul_get_handler, NULL };
        _link_params[i] = NULL;

        // Increase dev counter
        i++;
    }

    // coap get [fe80::e8e4:4534:4649:f34b]:5683 /.well-known/core

    _listener = (gcoap_listener_t) {
        &_resources[0],
        i,
        GCOAP_SOCKET_TYPE_UNDEF,
        _encode_link,
        NULL,
        NULL
    };

    gcoap_register_listener(&_listener);
}
