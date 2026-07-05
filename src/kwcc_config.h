/* kwcc_config.h — Config layer C interface (no mquickjs dependency)
 *
 * C 业务模块存取配置的统一接口，自动拼 "a." / "c." 前缀。
 */
#ifndef KWCC_CONFIG_H
#define KWCC_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include "kwcc_base.h"

/* ═══ App 域（自动拼 "a." 前缀）═══ */

void    kwcc_config_set_app_int(const char *key, int32_t val);
void    kwcc_config_set_app_string(const char *key, const char *val);
void    kwcc_config_set_app_bool(const char *key, int val);
void    kwcc_config_set_app_json(const char *key, const char *json_val);
void    kwcc_config_set_app_tlv(const char *key, const uint8_t *tlv_data, uint32_t tlv_len);
const char *kwcc_config_get_app(const char *key, const char *default_value);
void    kwcc_config_release_app(const char *key);
void    kwcc_config_release_app_prefix(const char *key);

void   *kwcc_config_get_app_slot(const char *key);   /* returns kwcc_mempool_slot_t* */

/* ═══ Core 域（自动拼 "c." 前缀）═══ */

void    kwcc_config_set_core_tlv(const char *key, const uint8_t *tlv_data, uint32_t tlv_len);
const char *kwcc_config_get_core(const char *key, const char *default_value);
void  *kwcc_config_get_core_slot(const char *key);   /* returns kwcc_mempool_slot_t* */
void    kwcc_config_release_core(const char *key);

/* ── Core TLV view ── */

typedef struct {
    const uint8_t *data;   /* slot internal TLV data pointer (no free needed) */
    size_t         size;   /* TLV data length */
} kwcc_config_tlv_t;

/* Get TLV object from core domain */
kwcc_config_tlv_t kwcc_config_get_core_tlv(const char *key);

/* Get sub-field value (returns kwcc_base_str_t, needs free) */
kwcc_base_str_t kwcc_config_tlv_get_field(const kwcc_config_tlv_t *tlv, const char *path);

/* Get sub-field TLV type (FIELD/OBJECT/ARRAY) */
uint8_t kwcc_config_tlv_get_type(const kwcc_config_tlv_t *tlv, const char *path);

/* Get nested object (returns sub-view) */
kwcc_config_tlv_t kwcc_config_tlv_get_object(const kwcc_config_tlv_t *tlv, const char *path);

/* ═══ 通用 ═══ */

void    kwcc_config_set_max_pools(int type, int max);

#endif /* KWCC_CONFIG_H */
