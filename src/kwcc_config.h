/* kwcc_config.h — Config layer C interface (no mquickjs dependency)
 *
 * C 业务模块存取配置的统一接口，自动拼 "a." / "c." 前缀。
 */
#ifndef KWCC_CONFIG_H
#define KWCC_CONFIG_H

#include <stdint.h>

/* ═══ App 域（自动拼 "a." 前缀）═══ */

void    kwcc_config_set_app_int(const char *key, int32_t val);
void    kwcc_config_set_app_string(const char *key, const char *val);
void    kwcc_config_set_app_bool(const char *key, int val);
void    kwcc_config_set_app_tlv(const char *key, const uint8_t *tlv_data, uint32_t tlv_len);
const char *kwcc_config_get_app(const char *key, const char *default_value);
void    kwcc_config_release_app(const char *key);
void    kwcc_config_release_app_prefix(const char *key);

/* ═══ Core 域（自动拼 "c." 前缀）═══ */

void    kwcc_config_set_core_tlv(const char *key, const uint8_t *tlv_data, uint32_t tlv_len);
const char *kwcc_config_get_core(const char *key, const char *default_value);
void  *kwcc_config_get_core_slot(const char *key);   /* returns kwcc_mempool_slot_t* */
void    kwcc_config_release_core(const char *key);

/* ═══ 通用 ═══ */

void    kwcc_config_set_max_pools(int type, int max);

#endif /* KWCC_CONFIG_H */
