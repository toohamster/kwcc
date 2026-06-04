/* kwcc_pool.c — three-tier Slab memory pool implementation */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "kwcc_pool.h"
#include "kwcc_base.h"
#include "llog.h"

/* ── Global pool instances ── */
kwcc_mem_pool_t g_core_pool;
kwcc_mem_pool_t g_app_pool;
kwcc_mem_pool_t g_user_pool;

/* ═══════════════════════════════════════════════════════════
 * Cross-platform aligned malloc
 * ═══════════════════════════════════════════════════════════ */

static void *kwcc_pool_aligned_malloc(size_t size, size_t align) {
#ifdef __APPLE__
    /* macOS malloc is 16-byte aligned by default */
    return malloc(size);
#elif defined(__linux__)
    return aligned_alloc(align, size);
#elif defined(_WIN32)
    return _aligned_malloc(size, align);
#else
    /* Fallback: manual alignment */
    void *ptr = malloc(size + align + sizeof(void *));
    if (!ptr) return NULL;
    void *aligned = (void *)(((uintptr_t)ptr + align + sizeof(void *)) & ~(align - 1));
    ((void **)aligned)[-1] = ptr;
    return aligned;
#endif
}

static void kwcc_pool_aligned_free(void *ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

/* ═══════════════════════════════════════════════════════════
 * FNV-1a hash
 * ═══════════════════════════════════════════════════════════ */

uint32_t kwcc_pool_fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

/* ═══════════════════════════════════════════════════════════
 * Slab implicit linked list
 * ═══════════════════════════════════════════════════════════ */

static uint16_t kwcc_pool_slab_alloc(kwcc_slab_t *slab) {
    if (slab->free_head == 0xFFFF) return 0xFFFF;
    uint16_t idx = slab->free_head;
    uint16_t *next = (uint16_t *)(slab->memory + (size_t)idx * slab->chunk_size);
    slab->free_head = *next;
    return idx;
}

static void kwcc_pool_slab_free(kwcc_slab_t *slab, uint16_t idx) {
    uint16_t *next = (uint16_t *)(slab->memory + (size_t)idx * slab->chunk_size);
    *next = slab->free_head;
    slab->free_head = idx;
}

static void kwcc_pool_slab_init(kwcc_slab_t *slab) {
    assert(slab->chunk_size > 0 && (slab->chunk_size & (slab->chunk_size - 1)) == 0);
    uint32_t n = slab->chunk_count;
    for (uint32_t i = 0; i < n - 1; i++) {
        uint16_t *next = (uint16_t *)(slab->memory + (size_t)i * slab->chunk_size);
        *next = (uint16_t)(i + 1);
    }
    *(uint16_t *)(slab->memory + (size_t)(n - 1) * slab->chunk_size) = 0xFFFF;
    slab->free_head = 0;
}

/* ═══════════════════════════════════════════════════════════
 * Single pool initialization
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t chunk_size;
    uint32_t chunk_count;
} slab_spec_t;

static void kwcc_pool_init_internal(kwcc_mem_pool_t *pool, size_t size,
                                     uint32_t slot_count,
                                     const slab_spec_t *specs,
                                     const char *name) {
    memset(pool, 0, sizeof(*pool));
    pool->name = name;
    pool->total_size = size;
    pool->total_slot_count = slot_count;

    /* Allocate raw memory (16-byte aligned) */
    pool->raw_memory = (uint8_t *)kwcc_pool_aligned_malloc(size, 16);
    assert(pool->raw_memory && "pool raw_memory malloc failed");

    /* Allocate slot metadata array */
    pool->slots = (kwcc_slot_t *)calloc(slot_count, sizeof(kwcc_slot_t));
    assert(pool->slots && "pool slots calloc failed");

    /* Partition raw_memory into 3 slabs */
    size_t offset = 0;
    for (int i = 0; i < KWCC_NUM_SIZE_CLASSES; i++) {
        kwcc_slab_t *slab = &pool->slabs[i];
        slab->chunk_size = specs[i].chunk_size;
        slab->chunk_count = specs[i].chunk_count;
        slab->memory = pool->raw_memory + offset;
        offset += (size_t)specs[i].chunk_size * specs[i].chunk_count;
        kwcc_pool_slab_init(slab);
    }

    pool->last_gc_time = 0;
}

/* ═══════════════════════════════════════════════════════════
 * Default slab specs for each pool
 * ═══════════════════════════════════════════════════════════ */

/* Core: 32KB — Small 64×64B=4K, Medium 64×256B=16K, Large 12×1KB=12K */
static const slab_spec_t core_specs[] = {
    { 64,    64  },
    { 256,   64  },
    { 1024,  12  },
};
#define CORE_SLOT_COUNT (64 + 64 + 12)  /* 140 */

/* App: 256KB — Small 256×256B=64K, Medium 128×1KB=128K, Large 4×16KB=64K */
static const slab_spec_t app_specs[] = {
    { 256,    256 },
    { 1024,   128 },
    { 16384,  4   },
};
#define APP_SLOT_COUNT (256 + 128 + 4)  /* 388 */

/* User: 1MB — Small 256×256B=64K, Medium 512×1KB=512K, Large 64×4KB=256K */
static const slab_spec_t user_specs[] = {
    { 256,   256  },
    { 1024,  512  },
    { 4096,  64   },
};
#define USER_SLOT_COUNT (256 + 512 + 64)  /* 832 */

/* ═══════════════════════════════════════════════════════════
 * Public initialization
 * ═══════════════════════════════════════════════════════════ */

void kwcc_mem_init_defaults(void) {
    kwcc_runtime_spec_t spec = {0};
    kwcc_mem_init(&spec);
}

void kwcc_mem_init(const kwcc_runtime_spec_t *spec) {
    size_t core_sz = (spec && spec->core_size > 0) ? spec->core_size : KWCC_CORE_SIZE;
    size_t app_sz  = (spec && spec->app_size  > 0) ? spec->app_size  : KWCC_APP_SIZE;
    size_t user_sz = (spec && spec->user_size > 0) ? spec->user_size : KWCC_USER_SIZE;

    kwcc_pool_init_internal(&g_core_pool, core_sz, CORE_SLOT_COUNT, core_specs, "Core");
    g_core_pool.is_user_land = 0;

    kwcc_pool_init_internal(&g_app_pool, app_sz, APP_SLOT_COUNT, app_specs, "App");
    g_app_pool.is_user_land = 1;

    if (user_sz > 0) {
        kwcc_pool_init_internal(&g_user_pool, user_sz, USER_SLOT_COUNT, user_specs, "User");
        g_user_pool.is_user_land = 1;
    }

    log_info("pool: Core=%zuKB App=%zuKB User=%zuKB initialized",
             core_sz / 1024, app_sz / 1024, user_sz / 1024);
}

void kwcc_pool_configure(kwcc_mem_pool_t *pool, size_t size) {
    if (!pool || size == 0) return;
    if (pool->raw_memory != NULL) {
        if (pool->total_size == size) return;  /* already same size, silent skip */
        log_warn("pool: pool %s already configured (%zu bytes), ignoring resize to %zu",
                 pool->name, pool->total_size, size);
        return;
    }

    if (pool == &g_app_pool) {
        kwcc_pool_init_internal(pool, size, APP_SLOT_COUNT, app_specs, "App");
        pool->is_user_land = 1;
    } else if (pool == &g_user_pool) {
        kwcc_pool_init_internal(pool, size, USER_SLOT_COUNT, user_specs, "User");
        pool->is_user_land = 1;
    }

    log_info("pool: %s configured with %zu bytes", pool->name, size);
}

void kwcc_mem_shutdown(void) {
    kwcc_mem_pool_t *pools[] = { &g_core_pool, &g_app_pool, &g_user_pool };
    for (int i = 0; i < 3; i++) {
        kwcc_mem_pool_t *p = pools[i];
        if (p->raw_memory) {
            kwcc_pool_aligned_free(p->raw_memory);
            p->raw_memory = NULL;
        }
        if (p->slots) {
            free(p->slots);
            p->slots = NULL;
        }
        memset(p, 0, sizeof(*p));
    }
    log_info("pool: shutdown complete");
}

/* ═══════════════════════════════════════════════════════════
 * Slot allocation
 * ═══════════════════════════════════════════════════════════ */

kwcc_slot_t *kwcc_pool_alloc(kwcc_mem_pool_t *pool, const char *key,
                              uint32_t size, uint32_t timeout_sec) {
    if (!pool || !pool->raw_memory || !key) return NULL;

    uint32_t now = (uint32_t)time(NULL);
    uint32_t h = kwcc_pool_fnv1a(key);

    /* Try each slab from smallest to largest (best fit) */
    for (int i = 0; i < KWCC_NUM_SIZE_CLASSES; i++) {
        kwcc_slab_t *slab = &pool->slabs[i];
        if (slab->chunk_size < size) continue;

        uint16_t idx = kwcc_pool_slab_alloc(slab);
        if (idx == 0xFFFF) continue;  /* full, try next larger slab */

        kwcc_slot_t *s = &pool->slots[idx];
        memset(s, 0, sizeof(*s));

        /* Key storage: inline if short, else fallback (for now just inline + truncate) */
        size_t klen = strlen(key);
        if (klen < sizeof(s->key_buf)) {
            memcpy(s->key_buf, key, klen);
            s->key_buf[klen] = '\0';
            s->key = s->key_buf;
        } else {
            /* Truncate to fit (config keys are always short) */
            memcpy(s->key_buf, key, sizeof(s->key_buf) - 1);
            s->key_buf[sizeof(s->key_buf) - 1] = '\0';
            s->key = s->key_buf;
        }

        s->hash = h;
        s->data = slab->memory + (size_t)idx * slab->chunk_size;
        s->capacity = slab->chunk_size;
        s->size = 0;
        s->in_use = 1;
        s->slab_idx = (uint8_t)i;
        s->ref_count = 0;
        s->alloc_time = now;
        s->last_access = now;
        s->timeout_sec = timeout_sec;

        return s;
    }

    log_warn("pool: %s full, alloc failed key=%s size=%u", pool->name, key, size);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 * Lookup
 * ═══════════════════════════════════════════════════════════ */

kwcc_slot_t *kwcc_pool_get(kwcc_mem_pool_t *pool, const char *key) {
    if (!pool || !pool->raw_memory || !key) return NULL;

    uint32_t h = kwcc_pool_fnv1a(key);
    uint32_t now = (uint32_t)time(NULL);

    for (uint32_t i = 0; i < pool->total_slot_count; i++) {
        kwcc_slot_t *s = &pool->slots[i];
        if (!s->in_use) continue;
        if (s->hash != h) continue;
        if (strcmp(s->key, key) != 0) continue;
        s->last_access = now;
        return s;
    }
    return NULL;
}

const char *kwcc_pool_get_str(kwcc_mem_pool_t *pool, const char *key,
                               const char *default_value) {
    kwcc_slot_t *s = kwcc_pool_get(pool, key);
    if (!s || !s->data || !s->size) return default_value;
    /* Ensure null-terminated */
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

/* ═══════════════════════════════════════════════════════════
 * Set data
 * ═══════════════════════════════════════════════════════════ */

void kwcc_pool_set(kwcc_mem_pool_t *pool, kwcc_slot_t *slot,
                   const void *data, uint32_t size) {
    if (!pool || !slot || !data) return;
    if (size > slot->capacity) size = slot->capacity;
    memcpy(slot->data, data, size);
    slot->size = size;
    slot->last_access = (uint32_t)time(NULL);
}

/* ═══════════════════════════════════════════════════════════
 * Acquire / Release (ref_count)
 * ═══════════════════════════════════════════════════════════ */

void kwcc_pool_acquire(kwcc_mem_pool_t *pool, kwcc_slot_t *slot) {
    (void)pool;
    if (!slot) return;
    if (slot->ref_count == UINT16_MAX) {
        log_error("pool: slot %s ref_count overflow (max %u)", slot->key, UINT16_MAX);
        return;
    }
    slot->ref_count++;
}

void kwcc_pool_release(kwcc_mem_pool_t *pool, kwcc_slot_t *slot) {
    (void)pool;
    if (!slot || slot->ref_count == 0) return;
    slot->ref_count--;
}

/* ═══════════════════════════════════════════════════════════
 * Invalidate
 * ═══════════════════════════════════════════════════════════ */

void kwcc_pool_invalidate(kwcc_mem_pool_t *pool, kwcc_slot_t *slot) {
    if (!pool || !slot || !slot->in_use) return;
    slot->ref_count = 0;
    slot->timeout_sec = 0;
}

/* ═══════════════════════════════════════════════════════════
 * Internal: free a slot back to its slab
 * ═══════════════════════════════════════════════════════════ */

static void kwcc_pool_free_slot(kwcc_mem_pool_t *pool, kwcc_slot_t *slot) {
    if (!slot || !slot->in_use) return;
    uint8_t idx = slot->slab_idx;
    /* Compute chunk index from data pointer */
    kwcc_slab_t *slab = &pool->slabs[idx];
    uintptr_t data_addr = (uintptr_t)slot->data;
    uintptr_t slab_addr = (uintptr_t)slab->memory;
    uint16_t chunk_idx = (uint16_t)((data_addr - slab_addr) / slab->chunk_size);

    memset(slot, 0, sizeof(*slot));
    kwcc_pool_slab_free(slab, chunk_idx);
}

static void kwcc_pool_force_invalidate(kwcc_mem_pool_t *pool, kwcc_slot_t *slot) {
    if (!slot || !slot->in_use) return;
    log_warn("pool: forcing invalidate slot key=%s", slot->key_buf);
    slot->ref_count = 0;
    kwcc_pool_free_slot(pool, slot);
}

/* ═══════════════════════════════════════════════════════════
 * GC
 * ═══════════════════════════════════════════════════════════ */

static void kwcc_pool_gc_internal(kwcc_mem_pool_t *pool, uint32_t now) {
    for (uint32_t i = 0; i < pool->total_slot_count; i++) {
        kwcc_slot_t *s = &pool->slots[i];
        if (!s->in_use) continue;

        /* Layer 1: ref=0 → immediate reclaim */
        if (s->ref_count == 0) {
            kwcc_pool_free_slot(pool, s);
            continue;
        }

        /* Layer 2: timeout → forced reclaim */
        if (s->timeout_sec > 0 && (now - s->alloc_time) >= s->timeout_sec) {
            kwcc_pool_force_invalidate(pool, s);
        }
    }
}

void kwcc_pool_gc(kwcc_mem_pool_t *pool) {
    if (!pool || !pool->raw_memory) return;
    uint32_t now = (uint32_t)time(NULL);
    if (now - pool->last_gc_time < 5) return;  /* throttle */
    pool->last_gc_time = now;
    kwcc_pool_gc_internal(pool, now);
}

void kwcc_pool_gc_force(kwcc_mem_pool_t *pool) {
    if (!pool || !pool->raw_memory) return;
    pool->last_gc_time = 0;  /* reset throttle */
    kwcc_pool_gc(pool);
}

void kwcc_pool_gc_auto(kwcc_mem_pool_t *pool) {
    if (!pool || !pool->raw_memory) return;

    /* Check usage */
    int used = 0;
    for (uint32_t i = 0; i < pool->total_slot_count; i++) {
        if (pool->slots[i].in_use) used++;
    }
    float usage = pool->total_slot_count > 0
        ? (float)used / (float)pool->total_slot_count : 0.0f;

    if (usage > 0.8f) {
        kwcc_pool_gc_force(pool);
        return;
    }
    kwcc_pool_gc(pool);
}

/* ═══════════════════════════════════════════════════════════
 * Debug dumps (only when KWCC_DEBUG)
 * ═══════════════════════════════════════════════════════════ */

#ifdef KWCC_DEBUG

void kwcc_pool_dump_stats(kwcc_mem_pool_t *pool) {
    if (!pool || !pool->raw_memory) return;

    int used = 0;
    size_t used_bytes = 0;
    for (uint32_t i = 0; i < pool->total_slot_count; i++) {
        if (pool->slots[i].in_use) {
            used++;
            used_bytes += pool->slots[i].size;
        }
    }

    log_info("=== Pool %s: %zu/%zu KB (%d used, %u free) ===",
             pool->name,
             used_bytes / 1024,
             pool->total_size / 1024,
             used,
             pool->total_slot_count - (uint32_t)used);
}

void kwcc_pool_dump_all(kwcc_mem_pool_t *pool, const char *filepath, int show_content) {
    if (!pool || !pool->raw_memory || !filepath) return;

    FILE *f = fopen(filepath, "w");
    if (!f) { log_error("pool_dump_all: cannot open %s", filepath); return; }

    int used = 0;
    for (uint32_t i = 0; i < pool->total_slot_count; i++) {
        kwcc_slot_t *s = &pool->slots[i];
        if (!s->in_use) continue;
        used++;

        uint32_t age = (uint32_t)time(NULL) - s->alloc_time;
        fprintf(f, "slot %u: key=\"%s\", size=%u/%uB, ref=%u, timeout=%u, age=%us\n",
                i, s->key_buf, s->size, s->capacity, s->ref_count, s->timeout_sec, age);

        if (show_content && s->size > 0) {
            /* Check if printable */
            int printable = 1;
            for (uint32_t j = 0; j < s->size && j < 64; j++) {
                uint8_t c = s->data[j];
                if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
                    printable = 0;
                    break;
                }
            }

            if (printable) {
                fprintf(f, "  data: ");
                uint32_t show = s->size < 128 ? s->size : 128;
                for (uint32_t j = 0; j < show; j++) {
                    fputc(s->data[j], f);
                }
                if (s->size > 128) {
                    fprintf(f, "... [truncated, total %u bytes]", s->size);
                }
                fputc('\n', f);
            } else {
                /* Hex dump, max 4 rows (64 bytes) */
                uint32_t max_show = s->size < 64 ? s->size : 64;
                fprintf(f, "  content (binary, %u bytes):\n", s->size);
                for (uint32_t j = 0; j < max_show; j += 16) {
                    fprintf(f, "  %04x:", j);
                    for (uint32_t k = 0; k < 16 && j + k < max_show; k++) {
                        fprintf(f, " %02x", s->data[j + k]);
                    }
                    fputc('\n', f);
                }
                if (s->size > 64) {
                    fprintf(f, "  ... [%u more bytes, not shown]\n", s->size - 64);
                }
            }
        }
        fputc('\n', f);
    }

    fprintf(f, "=== %s Pool: %d used, %u free ===\n",
            pool->name, used, pool->total_slot_count - (uint32_t)used);

    fclose(f);
    log_info("pool: dump written to %s", filepath);
}

#endif  /* KWCC_DEBUG */
