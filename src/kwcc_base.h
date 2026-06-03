/* kwcc_base.h — common infrastructure (config, constants, utilities)
 *
 * All modules (io, etc.) include this header.
 * Does NOT depend on mquickjs, microui, or nanovg.
 */
#ifndef KWCC_BASE_H
#define KWCC_BASE_H

#define KWCC_CONFIG_MAX_MODULES 64
#define KWCC_CONFIG_MAX_KEY_LEN 64

void        kwcc_config_set(const char *module, const char *key, const char *value);
const char *kwcc_config_get(const char *module, const char *key, const char *default_value);
int         kwcc_config_get_int32(const char *module, const char *key, int default_value);

/* ── Memory pool compile-time defaults ── */
#ifndef KWCC_CORE_SIZE
#define KWCC_CORE_SIZE  (32 * 1024)
#endif
#ifndef KWCC_APP_SIZE
#define KWCC_APP_SIZE   (256 * 1024)
#endif
#ifndef KWCC_USER_SIZE
#define KWCC_USER_SIZE  (1 * 1024 * 1024)
#endif
#ifndef KWCC_DEBUG
#define KWCC_DEBUG 0
#endif

#endif
