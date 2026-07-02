/* kwcc_js_http.c — HTTP Plugin module implementation
 *
 * Operates through kwcc_js_ops_t, never touches mquickjs directly.
 * Dependencies: kwcc_js.h + kwcc_http.h + llog.h
 */
#include "kwcc_js_http.h"
#include "kwcc_http.h"
#include "llog.h"

#include <string.h>
#include <stdlib.h>

/* ── Module-level ops pointer ────────────────────────────────── */

static kwcc_js_ops_t *s_ops;

/* ── http_load — create $http object + inject bridge methods ── */

static void http_load(kwcc_js_ops_t *ops) {
    s_ops = ops;

    /* Create $http object via C API (consistent with $config style) */
    kwcc_js_val_t http_obj = ops->new_object(ops);
    ops->set_str_prop(ops, ops->global_obj, "$http", http_obj);

    /* Inject C→JS bridge methods — pure JS logic goes in http.js */
    const char *code =
        "$http.state = { activeRequests: 0 };\n"
        "$http.cancel = function(reqId) {\n"
        "    kwcc_js_call_c('http', 'cancel', reqId);\n"
        "};\n"
        "$http.config = function(key, value) {\n"
        "    $config.coreSetTlv('http/' + key, value);\n"
        "};\n";
    ops->eval(ops, code, strlen(code), "<$http>", JS_EVAL_REPL);
}

/* ── http_apis — ops-signature handlers registered via dispatch ── */

static kwcc_js_val_t js_http_request(kwcc_js_ops_t *ops, int argc, kwcc_js_val_t *argv) {
    if (argc < 2) return ops->undefined;

    /* method */
    kwcc_js_cstr_buf_t mbuf;
    const char *method = ops->to_cstring(ops, argv[0], &mbuf);
    if (!method) method = "GET";

    /* url */
    kwcc_js_cstr_buf_t ubuf;
    const char *url = ops->to_cstring(ops, argv[1], &ubuf);
    if (!url) return ops->undefined;

    /* headers: argv[2] is an array of "Key: Value" strings */
    const char *headers[16];
    int header_count = 0;
    char *header_copies[16];
    memset(header_copies, 0, sizeof(header_copies));
    if (argc >= 3 && ops->get_class_id(ops, argv[2]) == 1 /* JS_CLASS_ARRAY */) {
        int len = ops->array_length(ops, argv[2]);
        for (int i = 0; i < len && i < 16; i++) {
            kwcc_js_val_t item = ops->array_get(ops, argv[2], (uint32_t)i);
            kwcc_js_cstr_buf_t ibuf;
            const char *s = ops->to_cstring(ops, item, &ibuf);
            if (s) {
                header_copies[header_count] = strdup(s);
                headers[header_count] = header_copies[header_count];
                header_count++;
            }
        }
    }

    /* body: argv[3] is a string */
    const char *body = NULL;
    int body_len = 0;
    kwcc_js_cstr_buf_t bbuf;
    if (argc >= 4) {
        body = ops->to_cstring(ops, argv[3], &bbuf);
        if (body) body_len = (int)strlen(body);
    }

    const char *req_id = kwcc_http_request(method, url, headers, header_count,
                                             body, body ? body_len : 0);

    /* Free header copies */
    for (int i = 0; i < 16; i++) free(header_copies[i]);

    if (!req_id) return ops->undefined;
    return ops->new_string(ops, req_id);
}

static kwcc_js_val_t js_http_cancel(kwcc_js_ops_t *ops, int argc, kwcc_js_val_t *argv) {
    if (argc < 1) return ops->undefined;
    kwcc_js_cstr_buf_t rbuf;
    const char *req_id = ops->to_cstring(ops, argv[0], &rbuf);
    if (!req_id) return ops->undefined;
    kwcc_http_cancel(req_id);
    return ops->undefined;
}

/* ── http_on_bus_event — route bus events to JS via $notify ── */

/* Extract req_id from topic string like "http/end/req_0" */
static const char *kwcc_js_http_extract_id(const char *topic) {
    /* Skip "http/" prefix, find second '/' */
    if (strncmp(topic, "http/", 5) != 0) return NULL;
    const char *p = topic + 5;
    while (*p && *p != '/') p++;
    if (*p != '/') return NULL;
    return p + 1;
}

static void http_on_bus_event(const char *topic, const void *data, size_t len,
                               kwcc_js_ops_t *ops) {
    if (strncmp(topic, "http/", 5) != 0) return;

    const char *id = kwcc_js_http_extract_id(topic);
    if (!id) return;

    /* Determine event type from topic */
    const char *event = NULL;
    if (strncmp(topic + 5, "end/", 4) == 0) {
        event = "end";
    } else if (strncmp(topic + 5, "error/", 6) == 0) {
        event = "error";
    } else if (strncmp(topic + 5, "cancel/", 7) == 0) {
        event = "cancel";
    } else if (strncmp(topic + 5, "progress/", 9) == 0) {
        event = "progress";
    } else {
        return;  /* unknown event type */
    }

    if (strcmp(event, "end") == 0 || strcmp(event, "error") == 0) {
        /* Read parsed result and build JSValue response object */
        kwcc_http_result_t result;
        if (kwcc_http_get_result(id, &result) != 0) {
            /* Can't read result — notify error */
            kwcc_js_val_t err_obj = ops->new_object(ops);
            ops->set_str_prop(ops, err_obj, "error",
                              ops->new_string(ops, "result unavailable"));
            ops->set_str_prop(ops, err_obj, "reqId", ops->new_string(ops, id));
            ops->notify_js(ops, "http", event, id, err_obj,
                           kwcc_http_cleanup_by_req_id);
            return;
        }

        /* Build response object — data copied into GC heap via new_string_len */
        kwcc_js_val_t resp = ops->new_object(ops);

        ops->set_str_prop(ops, resp, "status",
                          ops->new_int32(ops, result.status));

        if (result.error) {
            ops->set_str_prop(ops, resp, "error",
                              ops->new_string(ops, "request failed"));
        }

        if (result.body && result.body_len > 0) {
            ops->set_str_prop(ops, resp, "body",
                              ops->new_string_len(ops, result.body, result.body_len));
        } else {
            ops->set_str_prop(ops, resp, "body", ops->new_string(ops, ""));
        }

        ops->set_str_prop(ops, resp, "reqId", ops->new_string(ops, id));

        /* Headers — iterate via header access API */
        int hcount = kwcc_http_result_header_count(id);
        if (hcount > 0) {
            kwcc_js_val_t headers_obj = ops->new_object(ops);
            for (int i = 0; i < hcount; i++) {
                const char *hname = NULL, *hvalue = NULL;
                size_t nlen = 0, vlen = 0;
                if (kwcc_http_result_get_header(id, i, &hname, &nlen, &hvalue, &vlen) == 0
                    && hname && nlen > 0) {
                    /* Header name: not guaranteed null-terminated, copy to buffer */
                    char key_buf[128];
                    size_t copy_len = nlen < sizeof(key_buf) - 1 ? nlen : sizeof(key_buf) - 1;
                    memcpy(key_buf, hname, copy_len);
                    key_buf[copy_len] = '\0';
                    kwcc_js_val_t val = hvalue
                        ? ops->new_string_len(ops, hvalue, vlen)
                        : ops->new_string(ops, "");
                    ops->set_str_prop(ops, headers_obj, key_buf, val);
                }
            }
            ops->set_str_prop(ops, resp, "headers", headers_obj);
        }

        ops->notify_js(ops, "http", event, id, resp, kwcc_http_cleanup_by_req_id);

    } else if (strcmp(event, "cancel") == 0) {
        /* Cancel: kwcc_http_cancel already called cleanup, ack_cleanup = NULL */
        kwcc_js_val_t data_obj = ops->new_object(ops);
        ops->set_str_prop(ops, data_obj, "error",
                          ops->new_string(ops, "cancelled"));
        ops->set_str_prop(ops, data_obj, "reqId", ops->new_string(ops, id));
        ops->notify_js(ops, "http", "cancel", id, data_obj, NULL);

    } else if (strcmp(event, "progress") == 0) {
        /* Progress: read from bus data (kwcc_http_progress_t) */
        kwcc_js_val_t data_obj = ops->new_object(ops);
        if (data && len == sizeof(kwcc_http_progress_t)) {
            const kwcc_http_progress_t *prog = (const kwcc_http_progress_t *)data;
            ops->set_str_prop(ops, data_obj, "loaded",
                              ops->new_int32(ops, prog->loaded));
            ops->set_str_prop(ops, data_obj, "total",
                              ops->new_int32(ops, prog->total));
        }
        ops->set_str_prop(ops, data_obj, "reqId", ops->new_string(ops, id));
        ops->notify_js(ops, "http", "progress", id, data_obj, NULL);
    }
}

/* ── Module descriptor ───────────────────────────────────────── */

static const kwcc_js_api_t http_apis[] = {
    { "request", js_http_request },
    { "cancel",  js_http_cancel },
    { NULL, NULL }
};

kwcc_js_module_t kwcc_js_http_module = {
    .name = "http",
    .load = http_load,
    .apis = http_apis,
    .on_bus_event = http_on_bus_event,
    .unload = NULL,
};
