/* kwcc_mempool.c — multi-pool Slab memory pool implementation
 * Per design doc requirements/mempool-design.md (v7)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "kwcc_mempool.h"
#include "kwcc_base.h"
#include "llog.h"

/* ═══ Slot counts per pool type (per spec) ═══ */

#define KWCC_MEMPOOL_SLOTS_L0 128
#define KWCC_MEMPOOL_SLOTS_L1 128
#define KWCC_MEMPOOL_SLOTS_L2 128
#define KWCC_MEMPOOL_SLOTS_L3 128
#define KWCC_MEMPOOL_SLOTS_L4 128
#define KWCC_MEMPOOL_SLOTS_L5 32
#define KWCC_MEMPOOL_SLOTS_L6 8
#define KWCC_MEMPOOL_SLOTS_L7 128

static const int kwcc_mempool_slots_per_pool[KWCC_MEMPOOL_MAX_TYPES] = {
    KWCC_MEMPOOL_SLOTS_L0, KWCC_MEMPOOL_SLOTS_L1, KWCC_MEMPOOL_SLOTS_L2, KWCC_MEMPOOL_SLOTS_L3,
    KWCC_MEMPOOL_SLOTS_L4, KWCC_MEMPOOL_SLOTS_L5, KWCC_MEMPOOL_SLOTS_L6, KWCC_MEMPOOL_SLOTS_L7
};

/* ═══ Chunk sizes per pool type ═══ */

static const uint32_t kwcc_mempool_chunk_sizes[KWCC_MEMPOOL_MAX_TYPES] = {
    8,       /* L0: int/float/bool */
    32,      /* L1: very short strings */
    128,     /* L2: short strings */
    512,     /* L3: medium strings */
    1024,    /* L4: longer strings */
    4096,    /* L5: long strings */
    16384,   /* L6: large strings */
    0,       /* L7: dynamic */
};

/* ═══ Const table ═══ */

kwcc_mempool_const_t g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_TABLE_SIZE];

static void kwcc_mempool_const_init(void) {
    memset(g_kwcc_mempool_const_table, 0, sizeof(g_kwcc_mempool_const_table));

    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_NULL].real_type = KWCC_MEMPOOL_TYPE_STRING;
    memcpy(g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_NULL].value, "null", 5);
    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_NULL].size = 5;

    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_EMPTY].real_type = KWCC_MEMPOOL_TYPE_STRING;
    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_EMPTY].size = 1;

    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_ZERO].real_type = KWCC_MEMPOOL_TYPE_STRING;
    memcpy(g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_ZERO].value, "0", 2);
    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_ZERO].size = 2;

    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_ONE].real_type = KWCC_MEMPOOL_TYPE_STRING;
    memcpy(g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_ONE].value, "1", 2);
    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_ONE].size = 2;

    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_TRUE].real_type = KWCC_MEMPOOL_TYPE_STRING;
    memcpy(g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_TRUE].value, "true", 5);
    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_TRUE].size = 5;

    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_FALSE].real_type = KWCC_MEMPOOL_TYPE_STRING;
    memcpy(g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_FALSE].value, "false", 6);
    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_FALSE].size = 6;

    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_TRUE_BOOL].real_type = KWCC_MEMPOOL_TYPE_INT32;
    *(int32_t *)g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_TRUE_BOOL].value = 1;
    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_TRUE_BOOL].size = 4;

    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_FALSE_BOOL].real_type = KWCC_MEMPOOL_TYPE_INT32;
    *(int32_t *)g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_FALSE_BOOL].value = 0;
    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_FALSE_BOOL].size = 4;

    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_M1].real_type = KWCC_MEMPOOL_TYPE_INT32;
    *(int32_t *)g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_M1].value = -1;
    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_M1].size = 4;

    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_0_INT].real_type = KWCC_MEMPOOL_TYPE_INT32;
    *(int32_t *)g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_0_INT].value = 0;
    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_0_INT].size = 4;

    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_1_INT].real_type = KWCC_MEMPOOL_TYPE_INT32;
    *(int32_t *)g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_1_INT].value = 1;
    g_kwcc_mempool_const_table[KWCC_MEMPOOL_CONST_1_INT].size = 4;
}

int kwcc_mempool_const_lookup(const void *data, size_t len, uint8_t type) {
    if (!data || len == 0) return -1;
    for (int i = 0; i < KWCC_MEMPOOL_CONST_TABLE_SIZE; i++) {
        kwcc_mempool_const_t *c = &g_kwcc_mempool_const_table[i];
        if (c->real_type != type) continue;
        if (c->size != len) continue;
        if (c->size == 1 && c->value[0] == '\0') return i;
        if (memcmp(c->value, data, len) == 0) return i;
    }
    return -1;
}

/* ═══ FNV-1a hash ═══ */

uint32_t kwcc_mempool_fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

/* ═══ key_map ═══ */

kwcc_mempool_keymap_t g_kwcc_mempool_key_map[KWCC_MEMPOOL_KEY_MAP_SIZE];

static int kwcc_mempool_keymap_find(uint32_t hash) {
    uint32_t idx = hash % KWCC_MEMPOOL_KEY_MAP_SIZE;
    uint32_t start = idx;
    do {
        if (g_kwcc_mempool_key_map[idx].in_use && g_kwcc_mempool_key_map[idx].hash == hash)
            return (int)idx;
        idx = (idx + 1) % KWCC_MEMPOOL_KEY_MAP_SIZE;
    } while (idx != start);
    return -1;
}

static void kwcc_mempool_keymap_insert(uint32_t hash, uint8_t pool_type,
                                        uint8_t pool_idx, uint8_t slot_idx) {
    uint32_t idx = hash % KWCC_MEMPOOL_KEY_MAP_SIZE;
    uint32_t start = idx;
    do {
        if (!g_kwcc_mempool_key_map[idx].in_use) {
            g_kwcc_mempool_key_map[idx].hash = hash;
            g_kwcc_mempool_key_map[idx].pool_type = pool_type;
            g_kwcc_mempool_key_map[idx].pool_idx = pool_idx;
            g_kwcc_mempool_key_map[idx].slot_idx = slot_idx;
            g_kwcc_mempool_key_map[idx].in_use = 1;
            return;
        }
        if (g_kwcc_mempool_key_map[idx].hash == hash) {
            g_kwcc_mempool_key_map[idx].pool_type = pool_type;
            g_kwcc_mempool_key_map[idx].pool_idx = pool_idx;
            g_kwcc_mempool_key_map[idx].slot_idx = slot_idx;
            return;
        }
        idx = (idx + 1) % KWCC_MEMPOOL_KEY_MAP_SIZE;
    } while (idx != start);
}

static void kwcc_mempool_keymap_remove(uint32_t hash) {
    int idx = kwcc_mempool_keymap_find(hash);
    if (idx >= 0)
        memset(&g_kwcc_mempool_key_map[idx], 0, sizeof(kwcc_mempool_keymap_t));
}

/* ═══ Pool manager ═══ */

kwcc_mempool_manager_t g_kwcc_mempool_mgr;
const int g_kwcc_mempool_max_pools[KWCC_MEMPOOL_MAX_TYPES] = {16,8,8,4,4,4,2,2};
uint64_t g_kwcc_mempool_l7_used = 0;

/* ═══ Slab operations ═══ */

static uint16_t kwcc_mempool_slab_alloc(kwcc_mempool_slab_t *slab, int n_slots) {
    (void)n_slots;
    if (slab->free_head == 0xFFFF) return 0xFFFF;
    uint16_t idx = slab->free_head;
    uint16_t *next = (uint16_t *)(slab->memory + (size_t)idx * slab->chunk_size);
    slab->free_head = *next;
    return idx;
}

static void kwcc_mempool_slab_free(kwcc_mempool_slab_t *slab, uint16_t idx) {
    uint16_t *next = (uint16_t *)(slab->memory + (size_t)idx * slab->chunk_size);
    *next = slab->free_head;
    slab->free_head = idx;
}

static void kwcc_mempool_slab_init(kwcc_mempool_slab_t *slab, int n_slots) {
    assert(slab->chunk_size > 0);
    for (int i = 0; i < n_slots - 1; i++) {
        uint16_t *next = (uint16_t *)(slab->memory + (size_t)i * slab->chunk_size);
        *next = (uint16_t)(i + 1);
    }
    *(uint16_t *)(slab->memory + (size_t)(n_slots - 1) * slab->chunk_size) = 0xFFFF;
    slab->free_head = 0;
}

/* ═══ Pool creation ═══ */

static kwcc_mempool_pool_t *kwcc_mempool_create_pool(uint8_t type, uint8_t idx) {
    uint32_t csz = kwcc_mempool_chunk_sizes[type];
    int n = kwcc_mempool_slots_per_pool[type];
    size_t slab_bytes = (size_t)csz * n;

    kwcc_mempool_pool_t *pool = calloc(1, sizeof(kwcc_mempool_pool_t));
    if (!pool) return NULL;
    pool->slab.memory = malloc(slab_bytes);
    if (!pool->slab.memory) { free(pool); return NULL; }
    pool->slab.chunk_size = csz;
    kwcc_mempool_slab_init(&pool->slab, n);
    pool->type = type;
    pool->idx = idx;
    pool->in_use = 1;
    return pool;
}

static kwcc_mempool_l7_pool_t *kwcc_mempool_create_l7_pool(uint8_t idx) {
    kwcc_mempool_l7_pool_t *pool = calloc(1, sizeof(kwcc_mempool_l7_pool_t));
    if (pool) { pool->idx = idx; pool->in_use = 1; }
    return pool;
}

/* ═══ Initialization ═══ */

void kwcc_mempool_init(void) {
    kwcc_mempool_const_init();
    memset(&g_kwcc_mempool_mgr, 0, sizeof(g_kwcc_mempool_mgr));
    memset(g_kwcc_mempool_key_map, 0, sizeof(g_kwcc_mempool_key_map));
    memcpy(g_kwcc_mempool_mgr.max_pools, g_kwcc_mempool_max_pools,
           sizeof(g_kwcc_mempool_max_pools));

    for (int t = 0; t < KWCC_MEMPOOL_L7; t++) {
        kwcc_mempool_pool_t *p = kwcc_mempool_create_pool((uint8_t)t, 0);
        if (p) {
            g_kwcc_mempool_mgr.pools[t][0] = p;
            g_kwcc_mempool_mgr.pool_count[t] = 1;
        }
    }

    kwcc_mempool_l7_pool_t *l7 = kwcc_mempool_create_l7_pool(0);
    if (l7) {
        g_kwcc_mempool_mgr.l7_pools[0] = l7;
        g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L7] = 1;
    }

    log_info("mempool: initialized (1 pool per type L0-L7, ~530KB)");
}

void kwcc_mempool_shutdown(void) {
    for (int t = 0; t < KWCC_MEMPOOL_L7; t++) {
        for (int i = 0; i < g_kwcc_mempool_mgr.pool_count[t]; i++) {
            kwcc_mempool_pool_t *p = g_kwcc_mempool_mgr.pools[t][i];
            if (p) { free(p->slab.memory); free(p); }
        }
    }
    for (int i = 0; i < g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L7]; i++) {
        kwcc_mempool_l7_pool_t *p = g_kwcc_mempool_mgr.l7_pools[i];
        if (p) {
            for (int j = 0; j < KWCC_MEMPOOL_SLOTS_L7; j++)
                if (p->slots[j].in_use && p->slots[j].data) free(p->slots[j].data);
            free(p);
        }
    }
    g_kwcc_mempool_l7_used = 0;
    memset(&g_kwcc_mempool_mgr, 0, sizeof(g_kwcc_mempool_mgr));
    memset(g_kwcc_mempool_key_map, 0, sizeof(g_kwcc_mempool_key_map));
    log_info("mempool: shutdown complete");
}

/* ═══ Pool expansion ═══ */

static int kwcc_mempool_expand(uint8_t type) {
    if (type >= KWCC_MEMPOOL_MAX_TYPES) return -1;
    int count = g_kwcc_mempool_mgr.pool_count[type];
    int max = g_kwcc_mempool_mgr.max_pools[type];
    if (count >= max) return -1;
    kwcc_mempool_pool_t *p = kwcc_mempool_create_pool(type, (uint8_t)count);
    if (!p) return -1;
    g_kwcc_mempool_mgr.pools[type][count] = p;
    g_kwcc_mempool_mgr.pool_count[type] = count + 1;
    log_info("mempool: expanded L%d to pool #%d (total %d)", type, count, count + 1);
    return 0;
}

static int kwcc_mempool_expand_l7(void) {
    int count = g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L7];
    int max = g_kwcc_mempool_mgr.max_pools[KWCC_MEMPOOL_L7];
    if (count >= max) return -1;
    kwcc_mempool_l7_pool_t *p = kwcc_mempool_create_l7_pool((uint8_t)count);
    if (!p) return -1;
    g_kwcc_mempool_mgr.l7_pools[count] = p;
    g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L7] = count + 1;
    return 0;
}

/* ═══ Find free slot across all pools of a type ═══ */

static kwcc_mempool_slot_t *kwcc_mempool_find_free_slot(uint8_t type, uint8_t *out_pool_idx,
                                                         uint8_t *out_slot_idx) {
    int count = g_kwcc_mempool_mgr.pool_count[type];
    int n = kwcc_mempool_slots_per_pool[type];
    for (int pi = 0; pi < count; pi++) {
        kwcc_mempool_pool_t *p = g_kwcc_mempool_mgr.pools[type][pi];
        if (!p || !p->in_use) continue;
        if (p->slab.free_head == 0xFFFF) continue; /* pool full */
        uint16_t idx = kwcc_mempool_slab_alloc(&p->slab, n);
        if (idx == 0xFFFF) continue;
        *out_pool_idx = p->idx;
        *out_slot_idx = (uint8_t)idx;
        return &p->slots[idx];
    }
    return NULL;
}

/* ═══ Slot allocation ═══ */

static void kwcc_mempool_init_slot(kwcc_mempool_slot_t *s, uint8_t data_type, const char *key,
                                    uint32_t capacity, uint32_t timeout_sec,
                                    uint8_t pool_type, uint8_t pool_idx, uint8_t slot_idx,
                                    uint8_t *data_ptr) {
    memset(s, 0, sizeof(*s));
    s->data = data_ptr;
    s->capacity = capacity;
    s->pool_type = pool_type;
    s->pool_idx = pool_idx;
    s->slot_idx = slot_idx;
    s->type = data_type;
    s->ref_count = 1;   /* caller holds implicit reference */
    s->in_use = 1;
    s->alloc_time = (uint32_t)time(NULL);
    s->last_access = s->alloc_time;
    s->timeout_sec = timeout_sec;

    size_t klen = strlen(key);
    if (klen < sizeof(s->key_buf)) {
        memcpy(s->key_buf, key, klen);
        s->key_buf[klen] = '\0';
        s->key = s->key_buf;
    } else {
        memcpy(s->key_buf, key, sizeof(s->key_buf) - 1);
        s->key_buf[sizeof(s->key_buf) - 1] = '\0';
        s->key = s->key_buf;
    }
    s->hash = kwcc_mempool_fnv1a(key);
    kwcc_mempool_keymap_insert(s->hash, pool_type, pool_idx, slot_idx);
}

kwcc_mempool_slot_t *kwcc_mempool_alloc(uint8_t data_type, const char *key,
                                         uint32_t size, uint32_t timeout_sec) {
    if (!key) return NULL;

    uint32_t now = (uint32_t)time(NULL);
    uint32_t h = kwcc_mempool_fnv1a(key);

    /* Check if key already exists */
    int km_idx = kwcc_mempool_keymap_find(h);
    if (km_idx >= 0) {
        kwcc_mempool_keymap_t *km = &g_kwcc_mempool_key_map[km_idx];
        if (km->pool_type < KWCC_MEMPOOL_L7) {
            kwcc_mempool_pool_t *p = g_kwcc_mempool_mgr.pools[km->pool_type][km->pool_idx];
            if (p && p->in_use) {
                kwcc_mempool_slot_t *s = &p->slots[km->slot_idx];
                if (s->in_use && s->hash == h && strcmp(s->key, key) == 0) {
                    s->last_access = now;
                    return s;
                }
            }
        } else {
            kwcc_mempool_l7_pool_t *p = g_kwcc_mempool_mgr.l7_pools[km->pool_idx];
            if (p && p->in_use) {
                kwcc_mempool_slot_t *s = &p->slots[km->slot_idx];
                if (s->in_use && s->hash == h && strcmp(s->key, key) == 0) {
                    s->last_access = now;
                    return s;
                }
            }
        }
    }

    /* ── Route to pool type by size ── */
    uint8_t target_type;
    if (size <= kwcc_mempool_chunk_sizes[KWCC_MEMPOOL_L0]) {
        target_type = KWCC_MEMPOOL_L0;
    } else if (size <= kwcc_mempool_chunk_sizes[KWCC_MEMPOOL_L1]) {
        target_type = KWCC_MEMPOOL_L1;
    } else if (size <= kwcc_mempool_chunk_sizes[KWCC_MEMPOOL_L2]) {
        target_type = KWCC_MEMPOOL_L2;
    } else if (size <= kwcc_mempool_chunk_sizes[KWCC_MEMPOOL_L3]) {
        target_type = KWCC_MEMPOOL_L3;
    } else if (size <= kwcc_mempool_chunk_sizes[KWCC_MEMPOOL_L4]) {
        target_type = KWCC_MEMPOOL_L4;
    } else if (size <= kwcc_mempool_chunk_sizes[KWCC_MEMPOOL_L5]) {
        target_type = KWCC_MEMPOOL_L5;
    } else if (size <= kwcc_mempool_chunk_sizes[KWCC_MEMPOOL_L6]) {
        target_type = KWCC_MEMPOOL_L6;
    } else {
        return kwcc_mempool_alloc_dynamic(key, size, timeout_sec);
    }

    /* Try existing pools */
    uint8_t pi = 0, si = 0;
    kwcc_mempool_slot_t *s = kwcc_mempool_find_free_slot(target_type, &pi, &si);
    if (!s) {
        if (kwcc_mempool_expand(target_type) == 0) {
            s = kwcc_mempool_find_free_slot(target_type, &pi, &si);
        }
    }
    if (!s) {
        log_warn("mempool: L%d full, alloc failed key=%s size=%u", target_type, key, size);
        return NULL;
    }

    uint8_t *data_ptr = g_kwcc_mempool_mgr.pools[target_type][pi]->slab.memory
                        + (size_t)si * kwcc_mempool_chunk_sizes[target_type];
    kwcc_mempool_init_slot(s, data_type, key, kwcc_mempool_chunk_sizes[target_type],
                           timeout_sec, target_type, pi, si, data_ptr);
    return s;
}

kwcc_mempool_slot_t *kwcc_mempool_alloc_dynamic(const char *key,
                                                 uint32_t cap, uint32_t timeout_sec) {
    if (!key || cap == 0) return NULL;

    uint32_t now = (uint32_t)time(NULL);
    uint32_t h = kwcc_mempool_fnv1a(key);

    /* Check if key already exists in L7 */
    int km_idx = kwcc_mempool_keymap_find(h);
    if (km_idx >= 0) {
        kwcc_mempool_keymap_t *km = &g_kwcc_mempool_key_map[km_idx];
        if (km->pool_type == KWCC_MEMPOOL_L7) {
            kwcc_mempool_l7_pool_t *p = g_kwcc_mempool_mgr.l7_pools[km->pool_idx];
            if (p && p->in_use) {
                kwcc_mempool_slot_t *s = &p->slots[km->slot_idx];
                if (s->in_use && s->hash == h && strcmp(s->key, key) == 0) {
                    s->last_access = now;
                    return s;
                }
            }
        }
    }

    /* Find free L7 slot */
    int l7_count = g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L7];
    for (int pi = 0; pi < l7_count; pi++) {
        kwcc_mempool_l7_pool_t *p = g_kwcc_mempool_mgr.l7_pools[pi];
        if (!p || !p->in_use) continue;
        for (int si = 0; si < KWCC_MEMPOOL_SLOTS_L7; si++) {
            if (!p->slots[si].in_use) {
                uint8_t *data = malloc(cap);
                if (!data) continue;
                g_kwcc_mempool_l7_used += cap;
                memset(&p->slots[si], 0, sizeof(kwcc_mempool_slot_t));
                kwcc_mempool_init_slot(&p->slots[si], KWCC_MEMPOOL_TYPE_STRING, key,
                                       cap, timeout_sec, KWCC_MEMPOOL_L7,
                                       (uint8_t)pi, (uint8_t)si, data);
                return &p->slots[si];
            }
        }
    }

    /* Expand L7 */
    if (kwcc_mempool_expand_l7() == 0) {
        int new_pi = g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L7] - 1;
        kwcc_mempool_l7_pool_t *p = g_kwcc_mempool_mgr.l7_pools[new_pi];
        uint8_t *data = malloc(cap);
        if (data) {
            g_kwcc_mempool_l7_used += cap;
            kwcc_mempool_init_slot(&p->slots[0], KWCC_MEMPOOL_TYPE_STRING, key,
                                   cap, timeout_sec, KWCC_MEMPOOL_L7,
                                   (uint8_t)new_pi, 0, data);
            return &p->slots[0];
        }
    }

    log_warn("mempool: L7 full, alloc failed key=%s cap=%u", key, cap);
    return NULL;
}

/* ═══ Lookup ═══ */

kwcc_mempool_slot_t *kwcc_mempool_get(const char *key) {
    if (!key) return NULL;
    uint32_t h = kwcc_mempool_fnv1a(key);
    uint32_t now = (uint32_t)time(NULL);
    int km_idx = kwcc_mempool_keymap_find(h);
    if (km_idx < 0) return NULL;

    kwcc_mempool_keymap_t *km = &g_kwcc_mempool_key_map[km_idx];
    if (km->pool_type < KWCC_MEMPOOL_L7) {
        kwcc_mempool_pool_t *p = g_kwcc_mempool_mgr.pools[km->pool_type][km->pool_idx];
        if (!p || !p->in_use) return NULL;
        kwcc_mempool_slot_t *s = &p->slots[km->slot_idx];
        if (s->in_use && s->hash == h && strcmp(s->key, key) == 0) {
            s->last_access = now;
            return s;
        }
    } else {
        kwcc_mempool_l7_pool_t *p = g_kwcc_mempool_mgr.l7_pools[km->pool_idx];
        if (!p || !p->in_use) return NULL;
        kwcc_mempool_slot_t *s = &p->slots[km->slot_idx];
        if (s->in_use && s->hash == h && strcmp(s->key, key) == 0) {
            s->last_access = now;
            return s;
        }
    }
    return NULL;
}

const char *kwcc_mempool_get_str(const char *key, const char *default_value) {
    kwcc_mempool_slot_t *s = kwcc_mempool_get(key);
    if (!s || !s->data || !s->size) return default_value;
    if (s->data[s->size - 1] != '\0') {
        if (s->size < s->capacity) {
            s->data[s->size] = '\0';
            s->size++;
        } else {
            s->data[s->size - 1] = '\0';
        }
    }
    return (const char *)s->data;
}

/* ═══ Set data ═══ */

void kwcc_mempool_set(kwcc_mempool_slot_t *slot, const void *data, uint32_t size) {
    if (!slot || !data || !slot->data) return;

    /* Const lookup兜底 */
    if (size <= 8) {
        int ci = kwcc_mempool_const_lookup(data, size, slot->type);
        if (ci >= 0) {
            slot->data = g_kwcc_mempool_const_table[ci].value;
            slot->size = g_kwcc_mempool_const_table[ci].size;
            slot->type = KWCC_MEMPOOL_TYPE_CONST;
            slot->last_access = (uint32_t)time(NULL);
            return;
        }
    }

    if (size > slot->capacity) size = slot->capacity;
    memcpy(slot->data, data, size);
    slot->size = size;
    slot->last_access = (uint32_t)time(NULL);
}

/* ═══ Acquire / Release ═══ */

void kwcc_mempool_acquire(kwcc_mempool_slot_t *slot) {
    if (!slot) return;
    if (slot->ref_count == UINT16_MAX) {
        log_error("mempool: slot %s ref_count overflow", slot->key ? slot->key : "?");
        return;
    }
    slot->ref_count++;
}

void kwcc_mempool_release(kwcc_mempool_slot_t *slot) {
    if (!slot || slot->ref_count == 0) return;
    slot->ref_count--;
}

/* ═══ Invalidate ═══ */

void kwcc_mempool_invalidate(kwcc_mempool_slot_t *slot) {
    if (!slot || !slot->in_use) return;
    slot->ref_count = 0;
    slot->timeout_sec = 0;
}

/* ═══ Free slot ═══ */

static void kwcc_mempool_free_slot(kwcc_mempool_slot_t *slot) {
    if (!slot || !slot->in_use) return;
    kwcc_mempool_keymap_remove(slot->hash);

    if (slot->pool_type == KWCC_MEMPOOL_L7) {
        if (slot->data) g_kwcc_mempool_l7_used -= slot->capacity;
        free(slot->data);
        memset(slot, 0, sizeof(*slot));
        return;
    }

    kwcc_mempool_pool_t *p = g_kwcc_mempool_mgr.pools[slot->pool_type][slot->pool_idx];
    if (!p) { memset(slot, 0, sizeof(*slot)); return; }
    kwcc_mempool_slab_free(&p->slab, slot->slot_idx);
    memset(slot, 0, sizeof(*slot));
}

/* ═══ GC ═══ */

static void kwcc_mempool_gc_internal(void) {
    uint32_t now = (uint32_t)time(NULL);

    for (int t = 0; t < KWCC_MEMPOOL_L7; t++) {
        int count = g_kwcc_mempool_mgr.pool_count[t];
        for (int pi = 0; pi < count; pi++) {
            kwcc_mempool_pool_t *p = g_kwcc_mempool_mgr.pools[t][pi];
            if (!p || !p->in_use) continue;
            int n = kwcc_mempool_slots_per_pool[t];
            for (int si = 0; si < n; si++) {
                kwcc_mempool_slot_t *s = &p->slots[si];
                if (!s->in_use) continue;
                if (s->ref_count == 0) {
                    kwcc_mempool_free_slot(s);
                    continue;
                }
                if (s->timeout_sec > 0 && (now - s->alloc_time) >= s->timeout_sec) {
                    s->ref_count = 0;
                    kwcc_mempool_free_slot(s);
                }
            }
        }
    }

    int l7_count = g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L7];
    for (int pi = 0; pi < l7_count; pi++) {
        kwcc_mempool_l7_pool_t *p = g_kwcc_mempool_mgr.l7_pools[pi];
        if (!p || !p->in_use) continue;
        for (int si = 0; si < KWCC_MEMPOOL_SLOTS_L7; si++) {
            kwcc_mempool_slot_t *s = &p->slots[si];
            if (!s->in_use) continue;
            if (s->ref_count == 0) {
                kwcc_mempool_free_slot(s);
                continue;
            }
            if (s->timeout_sec > 0 && (now - s->alloc_time) >= s->timeout_sec) {
                s->ref_count = 0;
                kwcc_mempool_free_slot(s);
            }
        }
    }
}

static uint32_t g_kwcc_mempool_last_gc_time = 0;

void kwcc_mempool_gc(void) {
    uint32_t now = (uint32_t)time(NULL);
    if (now - g_kwcc_mempool_last_gc_time < 5) return;
    g_kwcc_mempool_last_gc_time = now;
    kwcc_mempool_gc_internal();
}

void kwcc_mempool_gc_force(void) {
    g_kwcc_mempool_last_gc_time = 0;
    kwcc_mempool_gc();
}

void kwcc_mempool_gc_auto(void) {
    int total_used = 0, total_slots = 0;
    for (int t = 0; t < KWCC_MEMPOOL_L7; t++) {
        int count = g_kwcc_mempool_mgr.pool_count[t];
        int n = kwcc_mempool_slots_per_pool[t];
        total_slots += count * n;
        for (int pi = 0; pi < count; pi++) {
            kwcc_mempool_pool_t *p = g_kwcc_mempool_mgr.pools[t][pi];
            if (!p) continue;
            for (int si = 0; si < n; si++)
                if (p->slots[si].in_use) total_used++;
        }
    }
    int l7_count = g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L7];
    for (int pi = 0; pi < l7_count; pi++) {
        kwcc_mempool_l7_pool_t *p = g_kwcc_mempool_mgr.l7_pools[pi];
        if (!p) continue;
        total_slots += KWCC_MEMPOOL_SLOTS_L7;
        for (int si = 0; si < KWCC_MEMPOOL_SLOTS_L7; si++)
            if (p->slots[si].in_use) total_used++;
    }
    float usage = total_slots > 0 ? (float)total_used / (float)total_slots : 0.0f;
    if (usage > 0.8f) { kwcc_mempool_gc_force(); return; }
    kwcc_mempool_gc();
}

/* ═══ Key enumeration (prefix scan) ═══ */

int kwcc_mempool_get_keys(const char *prefix, const char **out_keys, int max_keys) {
    if (!prefix || !out_keys) return 0;
    int count = 0;
    uint32_t now = (uint32_t)time(NULL);
    size_t plen = strlen(prefix);

    for (int t = 0; t < KWCC_MEMPOOL_L7; t++) {
        int pc = g_kwcc_mempool_mgr.pool_count[t];
        int n = kwcc_mempool_slots_per_pool[t];
        for (int pi = 0; pi < pc; pi++) {
            kwcc_mempool_pool_t *p = g_kwcc_mempool_mgr.pools[t][pi];
            if (!p) continue;
            for (int si = 0; si < n && count < max_keys; si++) {
                kwcc_mempool_slot_t *s = &p->slots[si];
                if (!s->in_use) continue;
                if (strncmp(s->key, prefix, plen) == 0) {
                    out_keys[count++] = s->key;
                    s->last_access = now;
                }
            }
        }
    }

    int l7_count = g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L7];
    for (int pi = 0; pi < l7_count && count < max_keys; pi++) {
        kwcc_mempool_l7_pool_t *p = g_kwcc_mempool_mgr.l7_pools[pi];
        if (!p) continue;
        for (int si = 0; si < KWCC_MEMPOOL_SLOTS_L7 && count < max_keys; si++) {
            kwcc_mempool_slot_t *s = &p->slots[si];
            if (!s->in_use) continue;
            if (strncmp(s->key, prefix, plen) == 0) {
                out_keys[count++] = s->key;
                s->last_access = now;
            }
        }
    }
    return count;
}

/* ═══ Pool management ═══ */

void kwcc_mempool_set_max_pools(int pool_type, int max) {
    if (pool_type < 0 || pool_type >= KWCC_MEMPOOL_MAX_TYPES) return;
    if (max < 4) {
        log_warn("mempool: set_max_pools(L%d, %d): value too small, clamped to 4", pool_type, max);
        max = 4;
    }
    g_kwcc_mempool_mgr.max_pools[pool_type] = max;
    log_info("mempool: L%d max_pools set to %d", pool_type, max);
}

/* ═══ TLV serialization (pure C, callback-driven) ═══ */

uint16_t kwcc_mempool_tlv_read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void kwcc_mempool_tlv_write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

#define KWCC_MEMPOOL_TLV_BUF_INIT 256

uint8_t *kwcc_mempool_tlv_build(kwcc_mempool_tlv_pack_cb cb, void *user_data,
                                 size_t *out_len) {
    size_t cap = KWCC_MEMPOOL_TLV_BUF_INIT;
    uint8_t *buf = malloc(cap);
    if (!buf) { *out_len = 0; return NULL; }
    size_t offset = 0;

    for (;;) {
        const char *name = NULL;
        const char *value = NULL;
        uint8_t etype = 0;
        size_t vlen = 0;
        int ret = cb(&name, &value, &etype, &vlen, user_data);
        if (ret <= 0 || !name) break;

        size_t nlen = strlen(name);
        size_t total = 1 + 2 + nlen + 1 + vlen;
        if (offset + total > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) { *out_len = 0; return NULL; }
        }
        buf[offset++] = etype;
        kwcc_mempool_tlv_write_le16(buf + offset, (uint16_t)total);
        offset += 2;
        memcpy(buf + offset, name, nlen);
        offset += nlen;
        buf[offset++] = '\0';
        if (vlen > 0 && value) {
            memcpy(buf + offset, value, vlen);
            offset += vlen;
        }
    }
    *out_len = offset;
    return buf;
}

int kwcc_mempool_tlv_iter(const uint8_t *tlv_data, size_t tlv_len,
                           kwcc_mempool_tlv_iter_cb cb, void *user_data) {
    if (!tlv_data || tlv_len == 0) return -1;
    if (tlv_len < 3) return -5;  /* truncated: need at least Type(1B) + TotalLen(2B) */
    const uint8_t *ptr = tlv_data;
    const uint8_t *end = tlv_data + tlv_len;
    int entries = 0;

    while (ptr + 3 <= end) {
        uint8_t type = ptr[0];
        uint16_t total_len = kwcc_mempool_tlv_read_le16(ptr + 1);
        if (total_len < 3) return -2;
        if (ptr + total_len > end) return -3;

        const char *name = (const char *)(ptr + 3);
        size_t nlen = 0;
        while (nlen < total_len - 3 && name[nlen] != '\0') nlen++;
        const uint8_t *value = ptr + 3 + nlen + 1;
        size_t value_len = total_len - 3 - nlen - 1;

        if (!cb(name, value, value_len, type, user_data)) return -4;
        entries++;
        ptr += total_len;
    }
    return entries;
}

const char *kwcc_mempool_tlv_get_path(const uint8_t *tlv_data, size_t tlv_len,
                                       const char *path, size_t *out_len,
                                       uint8_t *out_type) {
    if (!tlv_data || !path || !out_len) { if (out_len) *out_len = 0; if (out_type) *out_type = 0; return NULL; }
    if (out_type) *out_type = 0;
    const uint8_t *ptr = tlv_data;
    const uint8_t *end = tlv_data + tlv_len;
    const char *target = path;

    while (ptr + 3 <= end && *target) {
        uint8_t type = ptr[0];
        uint16_t total_len = kwcc_mempool_tlv_read_le16(ptr + 1);
        if (total_len < 3) { *out_len = 0; return NULL; }
        if (ptr + total_len > end) { *out_len = 0; return NULL; }

        const char *name = (const char *)(ptr + 3);
        size_t nlen = 0;
        while (nlen < total_len - 3 && name[nlen] != '\0') nlen++;
        const uint8_t *value = ptr + 3 + nlen + 1;
        size_t value_len = total_len - 3 - nlen - 1;

        const char *next_slash = strchr(target, '/');
        size_t seg_len = next_slash ? (size_t)(next_slash - target) : strlen(target);

        if (nlen == seg_len && memcmp(name, target, seg_len) == 0) {
            if (next_slash) {
                if (type == KWCC_MEMPOOL_TLV_OBJECT) {
                    ptr = value;
                    end = ptr + value_len;
                    target = next_slash + 1;
                    continue;
                }
                *out_len = 0;
                return NULL;
            }
            *out_len = value_len;
            if (out_type) *out_type = type;
            return (const char *)value;
        }
        ptr += total_len;
    }
    *out_len = 0;
    return NULL;
}

/* JSON string escape helper */
static size_t kwcc_mempool_json_escape_len(const char *s, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') n += 2;
        else if (c < 0x20) n += 6; /* \u00XX */
        else n++;
    }
    return n;
}

static void kwcc_mempool_json_write_escaped(char *dst, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') { *dst++ = '\\'; *dst++ = '"'; }
        else if (c == '\\') { *dst++ = '\\'; *dst++ = '\\'; }
        else if (c == '\n') { *dst++ = '\\'; *dst++ = 'n'; }
        else if (c == '\t') { *dst++ = '\\'; *dst++ = 't'; }
        else if (c == '\r') { *dst++ = '\\'; *dst++ = 'r'; }
        else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            *dst++ = '\\'; *dst++ = 'u'; *dst++ = '0'; *dst++ = '0';
            *dst++ = hex[(c >> 4) & 0xF]; *dst++ = hex[c & 0xF];
        } else {
            *dst++ = (char)c;
        }
    }
}

#define KWCC_MEMPOOL_TLV_JSON_INIT 256

char *kwcc_mempool_tlv_to_json(const uint8_t *tlv_data, size_t tlv_len, size_t *out_len) {
    if (!tlv_data || tlv_len == 0 || !out_len) { if (out_len) *out_len = 0; return NULL; }
    size_t cap = KWCC_MEMPOOL_TLV_JSON_INIT;
    char *json = malloc(cap);
    if (!json) { *out_len = 0; return NULL; }
    size_t pos = 0;

#define JSON_APPEND(s, l) do { \
    size_t _l = (l); \
    if (pos + _l >= cap) { cap *= 2; json = realloc(json, cap); } \
    memcpy(json + pos, (s), _l); pos += _l; \
} while (0)
#define JSON_APPEND_C(c) do { \
    if (pos + 1 >= cap) { cap *= 2; json = realloc(json, cap); } \
    json[pos++] = (c); \
} while (0)

    JSON_APPEND_C('{');
    const uint8_t *ptr = tlv_data;
    const uint8_t *end = tlv_data + tlv_len;
    int first = 1;

    while (ptr + 3 <= end) {
        uint8_t type = ptr[0];
        uint16_t total_len = kwcc_mempool_tlv_read_le16(ptr + 1);
        if (total_len < 3) break;
        if (ptr + total_len > end) break;

        const char *name = (const char *)(ptr + 3);
        size_t nlen = 0;
        while (nlen < total_len - 3 && name[nlen] != '\0') nlen++;
        const uint8_t *value = ptr + 3 + nlen + 1;
        size_t value_len = total_len - 3 - nlen - 1;

        if (!first) JSON_APPEND_C(',');
        first = 0;

        char quote_buf[1024];
        int qlen = snprintf(quote_buf, sizeof(quote_buf), "\"%.*s\":", (int)nlen, name);
        if (qlen > 0) JSON_APPEND(quote_buf, (size_t)qlen);

        if (type == KWCC_MEMPOOL_TLV_FIELD) {
            int is_num = value_len > 0;
            for (size_t i = 0; i < value_len && is_num; i++) {
                char c = ((const char *)value)[i];
                if (!((c >= '0' && c <= '9') || c == '-' || c == '.')) is_num = 0;
            }
            if (is_num && value_len > 0) {
                JSON_APPEND((const char *)value, value_len);
            } else {
                size_t elen = kwcc_mempool_json_escape_len((const char *)value, value_len);
                if (pos + elen + 2 >= cap) { cap *= 2; json = realloc(json, cap); }
                json[pos++] = '"';
                kwcc_mempool_json_write_escaped(json + pos, (const char *)value, value_len);
                pos += elen;
                json[pos++] = '"';
            }
        } else if (type == KWCC_MEMPOOL_TLV_OBJECT) {
            size_t sub_len;
            char *sub_json = kwcc_mempool_tlv_to_json(value, value_len, &sub_len);
            if (sub_json) { JSON_APPEND(sub_json, sub_len); free(sub_json); }
        } else {
            JSON_APPEND("\"\"", 2);
        }
        ptr += total_len;
    }
    JSON_APPEND_C('}');
    json[pos] = '\0';
    *out_len = pos;
    return json;

#undef JSON_APPEND
#undef JSON_APPEND_C
}

void kwcc_mempool_tlv_free_json(char *ptr) { free(ptr); }

/* ═══ Debug dumps (KWCC_DEBUG only) ═══ */

#ifdef KWCC_DEBUG

void kwcc_mempool_dump_stats(void) {
    int total_used = 0, total_slots = 0;
    for (int t = 0; t < KWCC_MEMPOOL_L7; t++) {
        int count = g_kwcc_mempool_mgr.pool_count[t];
        int n = kwcc_mempool_slots_per_pool[t];
        int used = 0;
        for (int pi = 0; pi < count; pi++) {
            kwcc_mempool_pool_t *p = g_kwcc_mempool_mgr.pools[t][pi];
            if (!p) continue;
            for (int si = 0; si < n; si++)
                if (p->slots[si].in_use) used++;
        }
        total_used += used;
        total_slots += count * n;
        log_info("L%d (%dB): %d pool%s [ %d/%d slots used ]",
                 t, kwcc_mempool_chunk_sizes[t], count, count > 1 ? "s" : "", used, count * n);
    }
    log_info("L7 (dynamic): %d pools [ %lu bytes used ]",
             g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L7],
             (unsigned long)g_kwcc_mempool_l7_used);
}

void kwcc_mempool_dump_all(const char *filepath, int show_content) {
    if (!filepath) return;
    FILE *f = fopen(filepath, "w");
    if (!f) { log_error("mempool_dump_all: cannot open %s", filepath); return; }
    uint32_t now = (uint32_t)time(NULL);
    int total = 0;

    for (int t = 0; t < KWCC_MEMPOOL_L7; t++) {
        int count = g_kwcc_mempool_mgr.pool_count[t];
        int n = kwcc_mempool_slots_per_pool[t];
        for (int pi = 0; pi < count; pi++) {
            kwcc_mempool_pool_t *p = g_kwcc_mempool_mgr.pools[t][pi];
            if (!p) continue;
            for (int si = 0; si < n; si++) {
                kwcc_mempool_slot_t *s = &p->slots[si];
                if (!s->in_use) continue;
                total++;
                uint32_t age = now - s->alloc_time;
                fprintf(f, "slot: key=\"%s\", type=%u, pool=L%d[%d], size=%u/%uB, ref=%u, timeout=%u, age=%us\n",
                        s->key_buf, s->type, s->pool_type, s->pool_idx,
                        s->size, s->capacity, s->ref_count, s->timeout_sec, age);
                if (show_content && s->size > 0 && s->data) {
                    int printable = 1;
                    for (uint32_t j = 0; j < s->size && j < 64; j++) {
                        uint8_t c = s->data[j];
                        if (c < 32 && c != '\n' && c != '\r' && c != '\t') { printable = 0; break; }
                    }
                    if (printable) {
                        fprintf(f, "  data: ");
                        uint32_t show = s->size < 128 ? s->size : 128;
                        for (uint32_t j = 0; j < show; j++) fputc(s->data[j], f);
                        if (s->size > 128) fprintf(f, "... [truncated]");
                        fputc('\n', f);
                    } else {
                        fprintf(f, "  (binary, %u bytes)\n", s->size);
                    }
                }
            }
        }
    }

    int l7_count = g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L7];
    for (int pi = 0; pi < l7_count; pi++) {
        kwcc_mempool_l7_pool_t *p = g_kwcc_mempool_mgr.l7_pools[pi];
        if (!p) continue;
        for (int si = 0; si < KWCC_MEMPOOL_SLOTS_L7; si++) {
            kwcc_mempool_slot_t *s = &p->slots[si];
            if (!s->in_use) continue;
            total++;
            uint32_t age = now - s->alloc_time;
            fprintf(f, "slot: key=\"%s\", type=%u, pool=L%d[%d], size=%u/%uB, ref=%u, timeout=%u, age=%us\n",
                    s->key_buf, s->type, s->pool_type, s->pool_idx,
                    s->size, s->capacity, s->ref_count, s->timeout_sec, age);
            if (show_content && s->size > 0 && s->data) {
                fprintf(f, "  (L7 dynamic, %u bytes)\n", s->size);
            }
        }
    }
    fprintf(f, "=== Total: %d slots ===\n", total);
    fclose(f);
    log_info("mempool: dump written to %s", filepath);
}

#endif /* KWCC_DEBUG */
