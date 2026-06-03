/* kwcc_pool.h — three-tier Slab memory pool */
#ifndef KWCC_POOL_H
#define KWCC_POOL_H

#include <stdint.h>
#include <stddef.h>

#define KWCC_NUM_SIZE_CLASSES 3

/* ── Slab (single size class) ── */
typedef struct {
    uint32_t  chunk_size;    /* chunk size (power of 2, naturally aligned) */
    uint32_t  chunk_count;   /* number of chunks */
    uint8_t  *memory;        /* contiguous memory block */
    uint16_t  free_head;     /* free list head (index, 0xFFFF = empty) */
} kwcc_slab_t;

/* ── Slot (per-key metadata) ── */
typedef struct {
    uint32_t  hash;          /* FNV-1a hash of key */
    char      key_buf[32];   /* inline key buffer */
    const char *key;         /* → key_buf or raw_memory */
    uint8_t  *data;          /* → slab chunk */
    uint32_t  capacity;      /* chunk quota size */
    uint32_t  size;          /* actual loaded bytes */
    uint8_t   in_use;        /* slot occupied */
    uint8_t   slab_idx;      /* slab index (0=Small, 1=Medium, 2=Large) */
    uint16_t  ref_count;     /* ref count (max 65535) */
    uint32_t  alloc_time;    /* allocation timestamp (time_t) */
    uint32_t  last_access;   /* last access timestamp */
    uint32_t  timeout_sec;   /* timeout seconds (0=never) */
} kwcc_slot_t;

/* ── Memory pool ── */
typedef struct {
    uint8_t      *raw_memory;       /* one big malloc */
    size_t        total_size;       /* pool capacity */
    kwcc_slot_t  *slots;            /* slot metadata array (separate malloc) */
    uint32_t      total_slot_count; /* total slots across all slabs */
    kwcc_slab_t   slabs[KWCC_NUM_SIZE_CLASSES]; /* 3 size classes */
    uint32_t      last_gc_time;     /* last GC time(NULL) for throttling */
    int           is_user_land;     /* 0=Core, 1=User (debug label) */
    const char   *name;             /* "Core" / "App" / "User" */
} kwcc_mem_pool_t;

/* ── Runtime spec (optional override) ── */
typedef struct {
    size_t core_size;   /* 0 = use default */
    size_t app_size;    /* 0 = use default */
    size_t user_size;   /* 0 = skip User pool */
} kwcc_runtime_spec_t;

/* ── Global pools (three independent mallocs) ── */
extern kwcc_mem_pool_t g_core_pool;
extern kwcc_mem_pool_t g_app_pool;
extern kwcc_mem_pool_t g_user_pool;

/* ── Initialization ── */
void kwcc_mem_init_defaults(void);
void kwcc_mem_init(const kwcc_runtime_spec_t *spec);
void kwcc_mem_shutdown(void);
void kwcc_pool_configure(kwcc_mem_pool_t *pool, size_t size);

/* ── Slot operations ── */
kwcc_slot_t *kwcc_pool_alloc(kwcc_mem_pool_t *pool, const char *key,
                              uint32_t size, uint32_t timeout_sec);
kwcc_slot_t *kwcc_pool_get(kwcc_mem_pool_t *pool, const char *key);
void         kwcc_pool_set(kwcc_mem_pool_t *pool, kwcc_slot_t *slot,
                           const void *data, uint32_t size);
void         kwcc_pool_acquire(kwcc_mem_pool_t *pool, kwcc_slot_t *slot);
void         kwcc_pool_release(kwcc_mem_pool_t *pool, kwcc_slot_t *slot);
void         kwcc_pool_invalidate(kwcc_mem_pool_t *pool, kwcc_slot_t *slot);

/* ── GC ── */
void kwcc_pool_gc(kwcc_mem_pool_t *pool);
void kwcc_pool_gc_force(kwcc_mem_pool_t *pool);
void kwcc_pool_gc_auto(kwcc_mem_pool_t *pool);

/* ── Debug (only when KWCC_DEBUG) ── */
#ifdef KWCC_DEBUG
void kwcc_pool_dump_stats(kwcc_mem_pool_t *pool);
void kwcc_pool_dump_all(kwcc_mem_pool_t *pool, const char *filepath, int show_content);
#endif

/* ── Internal (not public) ── */
uint32_t kwcc_pool_fnv1a(const char *s);

#endif
