/* kwcc_config.c — Config layer C interface
 *
 * 所有函数自动拼 "a." / "c." 前缀，底层委托 kwcc_mempool_*。
 */
#include <stdio.h>
#include <string.h>
#include "kwcc_config.h"
#include "kwcc_mempool.h"
#include "kwcc_base.h"

/* ── 前缀拼接辅助 ── */

static void kwcc_config_build_key(char *buf, size_t cap, const char *prefix, const char *key) {
    int n = snprintf(buf, cap, "%s.%s", prefix, key);
    if (n < 0 || n >= (int)cap) buf[0] = '\0';
}

/* ═══ App 域（"a." 前缀）═══ */

void kwcc_config_set_app_int(const char *key, int32_t val) {
    if (!key) return;
    char full[256];
    kwcc_config_build_key(full, sizeof(full), "a", key);
    if (!full[0]) return;

    kwcc_mempool_slot_t *s = kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_INT32, full, sizeof(val), 0);
    if (!s) return;
    kwcc_mempool_set(s, &val, sizeof(val));
}

void kwcc_config_set_app_string(const char *key, const char *val) {
    if (!key || !val) return;
    char full[256];
    kwcc_config_build_key(full, sizeof(full), "a", key);
    if (!full[0]) return;

    size_t len = strlen(val) + 1;
    kwcc_mempool_slot_t *s = kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_STRING, full, (uint32_t)len, 0);
    if (!s) return;
    kwcc_mempool_set(s, val, (uint32_t)len);
}

void kwcc_config_set_app_bool(const char *key, int val) {
    if (!key) return;
    char full[256];
    kwcc_config_build_key(full, sizeof(full), "a", key);
    if (!full[0]) return;

    int32_t v = val ? 1 : 0;
    kwcc_mempool_slot_t *s = kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_INT32, full, sizeof(v), 0);
    if (!s) return;
    kwcc_mempool_set(s, &v, sizeof(v));
}

void kwcc_config_set_app_json(const char *key, const char *json_val) {
    if (!key || !json_val) return;
    char full[256];
    kwcc_config_build_key(full, sizeof(full), "a", key);
    if (!full[0]) return;

    size_t len = strlen(json_val) + 1;
    kwcc_mempool_slot_t *s = kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_JSON, full, (uint32_t)len, 0);
    if (!s) return;
    kwcc_mempool_set(s, json_val, (uint32_t)len);
}

void kwcc_config_set_app_tlv(const char *key, const uint8_t *tlv_data, uint32_t tlv_len) {
    if (!key || !tlv_data || tlv_len == 0) return;
    char full[256];
    kwcc_config_build_key(full, sizeof(full), "a", key);
    if (!full[0]) return;

    kwcc_mempool_slot_t *s = kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_TLV, full, tlv_len, 0);
    if (!s) return;
    kwcc_mempool_set(s, tlv_data, tlv_len);
}

const char *kwcc_config_get_app(const char *key, const char *default_value) {
    if (!key) return default_value;
    char full[256];
    kwcc_config_build_key(full, sizeof(full), "a", key);
    if (!full[0]) return default_value;

    kwcc_mempool_slot_t *slot = kwcc_mempool_get(full);
    if (!slot || !slot->data) return default_value;

    static __thread char buf[32];
    if (slot->type == KWCC_MEMPOOL_TYPE_INT32 || slot->type == KWCC_MEMPOOL_TYPE_CONST) {
        snprintf(buf, sizeof(buf), "%d", *(int32_t *)slot->data);
        return buf;
    }
    return kwcc_mempool_get_str(full, default_value);
}

void kwcc_config_release_app(const char *key) {
    if (!key) return;
    char full[256];
    kwcc_config_build_key(full, sizeof(full), "a", key);
    if (!full[0]) return;
    kwcc_mempool_slot_t *s = kwcc_mempool_get(full);
    if (s) kwcc_mempool_release(s);
}

void kwcc_config_release_app_prefix(const char *key) {
    if (!key) return;
    char prefix[256];
    int n = snprintf(prefix, sizeof(prefix), "a.%s/", key);
    if (n < 0 || n >= (int)sizeof(prefix)) return;

    const char *keys[256];
    int count = kwcc_mempool_get_keys(prefix, keys, 256);
    for (int i = 0; i < count; i++) {
        kwcc_mempool_slot_t *s = kwcc_mempool_get(keys[i]);
        if (s) kwcc_mempool_release(s);
    }
}

void *kwcc_config_get_app_slot(const char *key) {
    if (!key) return NULL;
    char full[256];
    kwcc_config_build_key(full, sizeof(full), "a", key);
    if (!full[0]) return NULL;
    return (void *)kwcc_mempool_get(full);
}

/* ═══ Core 域（"c." 前缀）═══ */

void kwcc_config_set_core_tlv(const char *key, const uint8_t *tlv_data, uint32_t tlv_len) {
    if (!key || !tlv_data || tlv_len == 0) return;
    char full[256];
    kwcc_config_build_key(full, sizeof(full), "c", key);
    if (!full[0]) return;

    kwcc_mempool_slot_t *s = kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_TLV, full, tlv_len, 0);
    if (!s) return;
    kwcc_mempool_set(s, tlv_data, tlv_len);
}

const char *kwcc_config_get_core(const char *key, const char *default_value) {
    if (!key) return default_value;
    char full[256];
    kwcc_config_build_key(full, sizeof(full), "c", key);
    if (!full[0]) return default_value;

    kwcc_mempool_slot_t *slot = kwcc_mempool_get(full);
    if (!slot || !slot->data) return default_value;

    static __thread char buf[32];
    if (slot->type == KWCC_MEMPOOL_TYPE_INT32 || slot->type == KWCC_MEMPOOL_TYPE_CONST) {
        snprintf(buf, sizeof(buf), "%d", *(int32_t *)slot->data);
        return buf;
    }
    return kwcc_mempool_get_str(full, default_value);
}

void *kwcc_config_get_core_slot(const char *key) {
    if (!key) return NULL;
    char full[256];
    kwcc_config_build_key(full, sizeof(full), "c", key);
    if (!full[0]) return NULL;
    return (void *)kwcc_mempool_get(full);
}

void kwcc_config_release_core(const char *key) {
    if (!key) return;
    char full[256];
    kwcc_config_build_key(full, sizeof(full), "c", key);
    if (!full[0]) return;
    kwcc_mempool_slot_t *s = kwcc_mempool_get(full);
    if (s) kwcc_mempool_release(s);
}

/* ═══ 通用 ═══ */

void kwcc_config_set_max_pools(int type, int max) {
    kwcc_mempool_set_max_pools(type, max);
}
