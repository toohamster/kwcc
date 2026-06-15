/* kwcc_mempool.h — multi-pool Slab memory pool with TLV serialization
 * Pure C, no mquickjs dependency.
 *
 * @note Thread Safety
 *   This library is NOT thread-safe. All calls must be made from
 *   the same thread (typically the main loop thread).
 *   Concurrent access without external locking is undefined behavior.
 *
 * Pool types (by data length):
 *   L0=8B, L1=32B, L2=128B, L3=512B, L4=1KB, L5=4KB, L6=16KB, L7=dynamic malloc
 */
#ifndef KWCC_MEMPOOL_H
#define KWCC_MEMPOOL_H

#include <stdint.h>
#include <stddef.h>

/* ═══ Pool types ═══ */

enum {
    KWCC_MEMPOOL_L0 = 0,   /* 8B      int/float/bool */
    KWCC_MEMPOOL_L1 = 1,   /* 32B     very short strings */
    KWCC_MEMPOOL_L2 = 2,   /* 128B    short strings */
    KWCC_MEMPOOL_L3 = 3,   /* 512B    medium strings */
    KWCC_MEMPOOL_L4 = 4,   /* 1KB     longer strings */
    KWCC_MEMPOOL_L5 = 5,   /* 4KB     long strings */
    KWCC_MEMPOOL_L6 = 6,   /* 16KB    large strings */
    KWCC_MEMPOOL_L7 = 7,   /* dynamic malloc */
    KWCC_MEMPOOL_MAX_TYPES = 8,
};

/* ═══ Slot data types ═══ */

enum {
    KWCC_MEMPOOL_TYPE_STRING = 0,
    KWCC_MEMPOOL_TYPE_INT32  = 1,
    KWCC_MEMPOOL_TYPE_INT64  = 2,
    KWCC_MEMPOOL_TYPE_FLOAT  = 3,
    KWCC_MEMPOOL_TYPE_DOUBLE = 4,
    KWCC_MEMPOOL_TYPE_JSON   = 5,
    KWCC_MEMPOOL_TYPE_TLV    = 6,
    KWCC_MEMPOOL_TYPE_CONST  = 7,
};

/* ═══ Const table (16 entries) ═══ */

enum {
    KWCC_MEMPOOL_CONST_NULL       = 0,   /* "null" */
    KWCC_MEMPOOL_CONST_EMPTY      = 1,   /* "" */
    KWCC_MEMPOOL_CONST_ZERO       = 2,   /* "0" */
    KWCC_MEMPOOL_CONST_ONE        = 3,   /* "1" */
    KWCC_MEMPOOL_CONST_TRUE       = 4,   /* "true" */
    KWCC_MEMPOOL_CONST_FALSE      = 5,   /* "false" */
    KWCC_MEMPOOL_CONST_TRUE_BOOL  = 6,   /* true (INT32) */
    KWCC_MEMPOOL_CONST_FALSE_BOOL = 7,   /* false (INT32) */
    KWCC_MEMPOOL_CONST_M1         = 8,   /* -1 (INT32) */
    KWCC_MEMPOOL_CONST_0_INT      = 9,   /* 0 (INT32) */
    KWCC_MEMPOOL_CONST_1_INT      = 10,  /* 1 (INT32) */
    /* 11-15 reserved */
    KWCC_MEMPOOL_CONST_TABLE_SIZE = 16,
};

typedef struct {
    uint8_t  value[8];
    uint8_t  real_type;
    uint8_t  size;
} kwcc_mempool_const_t;

extern kwcc_mempool_const_t g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_TABLE_SIZE];

int kwcc_mempool_const_lookup(const void *data, size_t len, uint8_t type);

/* ═══ Slot structure (≈88B) ═══ */

typedef struct kwcc_mempool_slot {
    uint32_t    hash;
    char        key_buf[32];
    const char *key;
    uint8_t    *data;
    uint32_t    capacity;
    uint32_t    size;
    uint32_t    alloc_time;
    uint32_t    last_access;
    uint32_t    timeout_sec;
    uint16_t    ref_count;
    uint8_t     pool_type;
    uint8_t     type;
    uint8_t     pool_idx;
    uint8_t     slot_idx;
    uint8_t     in_use;
} kwcc_mempool_slot_t;

/* ═══ key_map (32768 entries) ═══ */

#define KWCC_MEMPOOL_KEY_MAP_SIZE 32768

typedef struct {
    uint32_t hash;
    uint8_t  pool_type;
    uint8_t  pool_idx;
    uint16_t slot_idx;
    uint8_t  in_use;
} kwcc_mempool_keymap_t;

extern kwcc_mempool_keymap_t g_kwcc_mempool_key_map[KWCC_MEMPOOL_KEY_MAP_SIZE];

/* ═══ Pool structure ═══ */

#define KWCC_MEMPOOL_SLOTS_PER_POOL 128
#define KWCC_MEMPOOL_L7_SLOTS       128

typedef struct {
    uint8_t  *memory;
    uint32_t  chunk_size;
    uint32_t  chunk_count;
    uint16_t  free_head;
} kwcc_mempool_slab_t;

typedef struct {
    kwcc_mempool_slab_t   slab;
    kwcc_mempool_slot_t   slots[KWCC_MEMPOOL_SLOTS_PER_POOL];
    uint8_t               type;
    uint8_t               idx;
    uint8_t               in_use;
} kwcc_mempool_pool_t;

typedef struct {
    kwcc_mempool_slot_t slots[KWCC_MEMPOOL_L7_SLOTS];
    uint8_t             idx;
    uint8_t             in_use;
} kwcc_mempool_l7_pool_t;

/* ═══ Pool manager ═══ */

typedef struct {
    kwcc_mempool_pool_t      *pools[KWCC_MEMPOOL_MAX_TYPES][16];
    kwcc_mempool_l7_pool_t   *l7_pools[2];
    int                       pool_count[KWCC_MEMPOOL_MAX_TYPES];
    int                       max_pools[KWCC_MEMPOOL_MAX_TYPES];
} kwcc_mempool_manager_t;

extern kwcc_mempool_manager_t g_kwcc_mempool_mgr;
extern const int g_kwcc_mempool_max_pools[KWCC_MEMPOOL_MAX_TYPES];
extern uint64_t g_kwcc_mempool_l7_used;

/* ═══ API ═══ */

void kwcc_mempool_init(void);
void kwcc_mempool_shutdown(void);

kwcc_mempool_slot_t *kwcc_mempool_alloc(uint8_t data_type, const char *key,
                                         uint32_t size, uint32_t timeout_sec);
kwcc_mempool_slot_t *kwcc_mempool_alloc_dynamic(const char *key,
                                                 uint32_t cap, uint32_t timeout_sec);
kwcc_mempool_slot_t *kwcc_mempool_get(const char *key);
void kwcc_mempool_set(kwcc_mempool_slot_t *slot, const void *data, uint32_t size);

/**
 * @param key            key to look up in the mempool
 * @param default_value  returned when key is not found (not copied).
 *                       Caller must ensure default_value remains valid
 *                       while the returned pointer is in use.
 * @return               pointer to internal slot data (do NOT free),
 *                       or default_value if key not found.
 */
const char *kwcc_mempool_get_str(const char *key, const char *default_value);

void kwcc_mempool_acquire(kwcc_mempool_slot_t *slot);
void kwcc_mempool_release(kwcc_mempool_slot_t *slot);
void kwcc_mempool_invalidate(kwcc_mempool_slot_t *slot);

void kwcc_mempool_gc(void);
void kwcc_mempool_gc_force(void);
void kwcc_mempool_gc_auto(void);

int kwcc_mempool_get_keys(const char *prefix, const char **out_keys, int max_keys);
void kwcc_mempool_set_max_pools(int pool_type, int max);

/* ═══ TLV (pure C, no JS dependency) ═══ */

enum {
    KWCC_MEMPOOL_TLV_FIELD  = 0x01,
    KWCC_MEMPOOL_TLV_OBJECT = 0x02,
    KWCC_MEMPOOL_TLV_ARRAY  = 0x03,
};

typedef int (*kwcc_mempool_tlv_pack_cb)(const char **key, const char **value,
                                         uint8_t *type, size_t *value_len, void *user_data);
typedef int (*kwcc_mempool_tlv_iter_cb)(const char *name, const uint8_t *value,
                                         size_t value_len, uint8_t type, void *user_data);

uint8_t *kwcc_mempool_tlv_build(kwcc_mempool_tlv_pack_cb cb, void *user_data,
                                 size_t *out_len);
int kwcc_mempool_tlv_iter(const uint8_t *tlv_data, size_t tlv_len,
                           kwcc_mempool_tlv_iter_cb cb, void *user_data);
const char *kwcc_mempool_tlv_get_path(const uint8_t *tlv_data, size_t tlv_len,
                                       const char *path, size_t *out_len);
char *kwcc_mempool_tlv_to_json(const uint8_t *tlv_data, size_t tlv_len, size_t *out_len);
void kwcc_mempool_tlv_free_json(char *ptr);

/* ═══ Debug dump (KWCC_DEBUG only) ═══ */

#ifdef KWCC_DEBUG
void kwcc_mempool_dump_stats(void);
void kwcc_mempool_dump_all(const char *filepath, int show_content);
#endif

#endif /* KWCC_MEMPOOL_H */
