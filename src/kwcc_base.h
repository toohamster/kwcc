/* kwcc_base.h — common infrastructure (config, constants, utilities)
 *
 * All modules (io, http, etc.) include this header.
 * Does NOT depend on mquickjs, microui, or nanovg.
 */
#ifndef KWCC_BASE_H
#define KWCC_BASE_H

/* ── Config system: C↔JS configuration management ── */

#define KWCC_CONFIG_MAX_MODULES 16
#define KWCC_CONFIG_MAX_KEY_LEN 64
#define KWCC_CONFIG_MAX_VALUE_LEN 256

typedef struct {
    char key[KWCC_CONFIG_MAX_KEY_LEN];
    char value[KWCC_CONFIG_MAX_VALUE_LEN];
} kwcc_config_entry_t;

typedef struct {
    char module[KWCC_CONFIG_MAX_KEY_LEN];
    kwcc_config_entry_t entries[16];
    int entry_count;
    int in_use;
} kwcc_config_module_t;

void kwcc_config_set(const char *module, const char *key, const char *value);
const char *kwcc_config_get(const char *module, const char *key, const char *default_value);

#endif
