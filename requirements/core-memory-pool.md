# KWCC 核心层重构与三段式 Slab 内存池实施方案

## Context

当前 `kwcc` 已完成物理模块拆分（`kwcc_base.h` / `kwcc.c` / `kwcc_ui.c` / `kwcc_bus.c`），但底层内存管理仍是零散方案：
- config 系统用 JS `__kwcc_config` 全局对象，走 JS 堆
- SVG cache 用独立 128 槽固定数组
- 未来 IO/HTTP 模块需要大块接收缓冲，无处分配

**问题**：C↔JS 高频交互数据（字符串、缓冲区）走 JS 堆 → 每帧触发 mquickjs GC → 破坏 60fps。未来引入异步 HTTP + 新 UI 引擎时问题会放大。

**目标**：引入**通用三段式 Slab 内存池**，为 kwcc 筑起核心级性能与安全底座，彻底消除跨语言交互的内存分配压力。

**设计原则**：
- 通用模块，不绑定帧率/UI
- 池自身维护生命周期跟踪，开发者只需 alloc/get/release
- 零配置可用，有配置可调

---

## 技术约束

1. 使用 `mquickjs`（非标准 QuickJS），JS 代码严格遵守 ES5 语法
2. 100% 源码依赖，零外部库安装需求
3. 轻量定位：零配置可用，有配置可调

---

## 启动时序

内存池采用**两阶段初始化**：Core 池由 C 层提前建好，App/User 池由 JS 在 `main.js` 中指定大小后动态创建。

```
init() — main.m
  │
  ├─ 1. kwcc_mem_init_defaults()     → Core 池建好（C 层内部可用）
  │                                    App/User 池暂为 NULL
  │
  ├─ 2. kwcc_create_js()             → JSContext 就绪
  │
  ├─ 3. kwcc_register_ui(js_ctx)     → 注入 $config 全局 API
  │
  └─ 4. kwcc_process_js(js_text)     → 执行 main.js
       │
       └─ main.js 第 1 行:
            $config.setAppSize(256 * 1024);       → g_app_pool 建好
            $config.setUserSize(4 * 1024 * 1024); → g_user_pool 建好
            // App/User 池在 main.js 阶段就就绪了
       │
       └─ 后面 loadJs 业务代码...


frame() — 每帧
  ├─ kwcc_process_js(js_ctx, "onFrame();")  → 渲染
  └─ kwcc_pool_gc(&g_app_pool);              → GC 自动节流
```

**为什么 App/User 池不在 C 层建**：
- JS 业务层最知道自己需要多大内存
- 不改 C 代码、不重新编译，改 `main.js` 即可调参
- Core 池作为 bootstrap（32 KB），足够支撑到 JS 完成配置

**防御性设计**：
- App/User 池未配置前（NULL），如果有人调 `$config.setApp()` → 自动 fallback 到 Core 池 + 打 warn
- $config.setAppSize() 只能在池未配置时调用一次，重复调用打 warn 并忽略

### 新增 JS API

```javascript
// 配置池大小（只能在池未配置时调用一次，通常放 main.js 开头）
$config.setAppSize(bytes)      // 指定 App 池大小
$config.setUserSize(bytes)     // 指定 User 池大小
```

### C 层对应函数

```c
/* 仅初始化 Core 池（main.m 调用） */
void kwcc_mem_init_defaults(void);

/* 配置并创建 App/User 池（JS Bridge 调用） */
void kwcc_pool_configure(kwcc_mem_pool_t *pool, size_t size);
```

### 内存布局

```
三次独立 malloc → [ Core 块 ] [ App 块 ] [ User 块 ]
                    32 KB     256 KB    1~16 MB (可配)
                   (引擎内部) (模块缓冲) (JS 业务层)
每块内部按 3 个 size class 分 slab，分配时选最小适配桶
```

### 各段规格与 Slab 划分

#### Core 池 (32 KB) — 引擎内部状态

| Slab | 块数 × 块大小 | 合计 | 用途 |
|------|-------------|------|------|
| Small | 64 × 64B | 4 KB | 引擎内部短值 |
| Medium | 64 × 256B | 16 KB | 引擎内部中值 |
| Large | 12 × 1KB | 12 KB | 引擎内部大值 |

**注意**：`g_topic_map`（kwcc_bus.c）、`g_sync_table`（kwcc_ui.c）每帧清零的 C 数组**不迁移**到此池。

#### App 池 (256 KB) — 模块级缓冲

| Slab | 块数 × 块大小 | 合计 | 用途 |
|------|-------------|------|------|
| Small | 256 × 256B | 64 KB | 小缓冲 |
| Medium | 128 × 1KB | 128 KB | SVG path 数据、UI 文本 |
| Large | 4 × 16KB | 64 KB | IO/HTTP 接收缓冲 |

#### User 池 (1 MB 默认，运行时可配) — JS 业务层

| Slab | 块数 × 块大小 | 合计 | 用途 |
|------|-------------|------|------|
| Small | 256 × 256B | 64 KB | JS 短数据 |
| Medium | 512 × 1KB | 512 KB | JS 正常数据 |
| Large | 64 × 4KB | 256 KB | JS 大缓冲 |

#### 各池槽位总数汇总

| 池 | 槽位总数 | 最大 chunk | 说明 |
|---|---------|-----------|------|
| Core (32 KB) | 64 + 64 + 12 = **140** | 1 KB | 引擎内部 |
| App (256 KB) | 256 + 128 + 4 = **388** | 16 KB | 模块缓冲 / IO / HTTP |
| User (1 MB) | 256 + 512 + 64 = **832** | 4 KB | JS 业务层 |

**最大池 832 槽，线性扫描 ≈ 1~3 微秒**，hash 桶表不需要。

### 配置方式

```c
/* 编译时默认（kwcc_base.h） */
#ifndef KWCC_CORE_SIZE
#define KWCC_CORE_SIZE  (32 * 1024)
#endif
#ifndef KWCC_APP_SIZE
#define KWCC_APP_SIZE   (256 * 1024)
#endif
#ifndef KWCC_USER_SIZE
#define KWCC_USER_SIZE  (1 * 1024 * 1024)
#endif

/* 运行时配置结构体 */
typedef struct {
    size_t core_size;   // 0 = 用默认值
    size_t app_size;    // 0 = 用默认值
    size_t user_size;   // 0 = 不分配 User 池
} kwcc_runtime_spec_t;
```

调用示例（main.m）：

```c
kwcc_runtime_spec_t spec = {0};
spec.core_size = 64 * 1024;       // 覆盖默认
spec.app_size  = 512 * 1024;      // 覆盖默认
spec.user_size = 4 * 1024 * 1024; // 4 MB User
kwcc_mem_init(&spec);
```

### 为什么不用 INI 配置文件

- 开发者调一次就定，改 `main.m` 重编译只要 5 秒
- 项目已有 JS 层 `kwcc_config` 基础设施
- C 传统就是 struct 传参（SDL / GLFW / Sokol 都如此）
- pool 初始化是引擎启动关键路径，不能因配置文件问题导致启动失败

---

## Slab 分配器设计

### 为什么不用单槽位固定大小

单槽位方案下，10 字节字符串占 512B 槽位 → 浪费 98%。采用 Redis 风格的 **slab 分配器**：每池内部分 3 个 size class，分配时选"刚好装得下"的桶，**浪费从 98% 降到 <50%**。

### 防御性规则（6 条）

1. **Fallback 仅限同池内部**：Small 满了 → 试 Medium → 试 Large。最大桶也满了 → 返回 NULL，**绝不跨池越界**抢其他池的内存。
2. **Free List 必须是隐式链表**：每个空闲 chunk 的前 2 字节存下一个空闲块的 idx。分配从链表头拿，释放插回链表头，**任意顺序释放都是真 O(1)**。不能简单用 `free_list[--free_head]`（那要求严格 LIFO，实际释放顺序是随机的）。
3. **Pool 函数原子化（不含 yield 点）**：所有 slab 操作（alloc/get/release/GC）是纯 C 函数，不包含阻塞或 yield，天然兼容 protothread。不需要加锁，调用方只需确保不在两次 pool 操作之间 yield 后跨 protothread 释放同一个 slot。
4. **内存对齐**：封装 `kwcc_aligned_malloc(size, align)` 跨平台函数，macOS/Linux 用 `aligned_alloc`，Windows 用 `_aligned_malloc`，其他平台 fallback 到 malloc + 手动对齐。`raw_memory` 统一用此函数分配，确保 16 字节对齐。`chunk_size` 均为 2 的幂次方，天然对齐。
5. **ref_count 语义（reentrancy 防护）**：`alloc()` 时 `ref_count = 0`（初始无人引用）。`acquire()` 加 1，`release()` 减 1。只有 `ref_count` 从 1 变为 0 时才真正回收槽位。无论 C 或 JS 侧调用，语义一致。**这防止单线程重入 bug**：C 侧持有数据时 JS_Eval 回调中提前 release，ref_count 不会立即归零（因为 C 侧也 acquire 了），slot 不会被立即释放。
6. **内存泄漏防护（三层兜底）**：`alloc()` 后不调 `acquire()` 的槽位（ref=0）由 GC 回收。具体规则见「Reentrancy 与泄漏防护」专节。

### Pool 内部函数命名规范

所有函数严格遵循 `kwcc_{模块}_{动作}` 三段式命名：

| 层级 | 前缀 | 可见性 | 示例 |
|------|------|--------|------|
| 公开 API | `kwcc_pool_*` | 外部可见 | `kwcc_pool_alloc`, `kwcc_pool_release` |
| 内部辅助 | `kwcc_pool_*` 或 `pool_*` | `static`（仅 kwcc_pool.c） | `kwcc_pool_slab_alloc`, `kwcc_pool_gc_internal` |

```
模块前缀: kwcc_pool_
            ↓
  slab 底层操作:
    kwcc_pool_slab_alloc(slab)      → 从隐式链表分配一个 chunk
    kwcc_pool_slab_free(slab, idx)  → 归还 chunk 到链表
    kwcc_pool_slab_init(slab)       → 初始化空闲链表

  内存对齐辅助:
    kwcc_pool_aligned_malloc(size, align)
    kwcc_pool_aligned_free(ptr)

  GC 内部:
    kwcc_pool_gc_internal(pool, now)    → 核心扫描逻辑
    kwcc_pool_free_slot(pool, slot)     → 释放单个槽位
    kwcc_pool_force_invalidate(pool, s) → 强制回收超时槽位
```

### kwcc_slab_t（单个 slab）

```c
typedef struct {
    uint32_t chunk_size;     // 该 slab 的块大小（2 的幂次方，天然对齐）
    uint32_t chunk_count;    // 块数
    uint8_t *memory;         // 连续内存块（从 pool->raw_memory 切分，16 字节对齐）
    uint16_t  free_head;     // 空闲链表头（存的是 idx，-1 表示空）
} kwcc_slab_t;
```

**空闲块内嵌链表**（每个空闲 chunk 的前 2 字节存 next idx）：

```
空闲时：chunk 内部 [next_idx(2B) | 实际可用数据区(chunk_size - 2B)]
分配时：从 free_head 取出 idx，该 chunk 被占用
释放时：将 next_idx 写入 chunk 前 2 字节，插回 free_head 链表头
```

分配 O(1)：
```c
uint16_t kwcc_pool_slab_alloc(kwcc_slab_t *slab) {
    if (slab->free_head == 0xFFFF) return 0xFFFF;  // 空链表
    uint16_t idx = slab->free_head;
    uint16_t *next = (uint16_t *)(slab->memory + idx * slab->chunk_size);
    slab->free_head = *next;
    return idx;
}
```

释放 O(1)（任意顺序安全）：
```c
void kwcc_pool_slab_free(kwcc_slab_t *slab, uint16_t idx) {
    uint16_t *next = (uint16_t *)(slab->memory + idx * slab->chunk_size);
    *next = slab->free_head;
    slab->free_head = idx;
}
```

**初始化时构建链表**：
```c
void kwcc_pool_slab_init(kwcc_slab_t *slab) {
    for (uint32_t i = 0; i < slab->chunk_count - 1; i++) {
        uint16_t *next = (uint16_t *)(slab->memory + i * slab->chunk_size);
        *next = i + 1;
    }
    *(uint16_t *)(slab->memory + (slab->chunk_count - 1) * slab->chunk_size) = 0xFFFF;
    slab->free_head = 0;
}
```

### 跨平台内存对齐分配

```c
static void *kwcc_pool_aligned_malloc(size_t size, size_t align) {
#ifdef __APPLE__
    return malloc(size);
#elif defined(__linux__)
    return aligned_alloc(align, size);
#elif defined(_WIN32)
    return _aligned_malloc(size, align);
#else
    void *ptr = malloc(size + align + sizeof(void *));
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
```

### kwcc_slot_t（单个槽位元数据）

```c
typedef struct {
    uint32_t hash;              // key 的 FNV-1a 哈希（快速匹配）
    char key_buf[32];           // 内联 key buffer
    const char *key;            // 短 key → 指向 key_buf; 长 key → 指向 raw_memory key 区
    uint8_t *data;              // 指向 slab chunk 的数据区
    uint32_t capacity;          // 该 chunk 固定配额大小
    uint32_t size;              // 当前实际装载的字节数
    uint8_t in_use;             // 槽位占用标志
    uint16_t ref_count;         // 引用计数（max 65535，0 = 无引用，等待 GC 回收；acquire 时检查溢出）
    uint32_t alloc_time;        // 分配时的 time(NULL) 秒级时间戳
    uint32_t last_access;       // 最后一次 get/set 的 time(NULL)
    uint32_t timeout_sec;       // 超时秒数（0 = 永不超时，用于 EventSource 等持久连接）
} kwcc_slot_t;
```

### kwcc_mem_pool_t（一个内存池）

```c
#define KWCC_NUM_SIZE_CLASSES 3

typedef struct {
    uint8_t *raw_memory;        // 一次性 malloc 的连续大内存首地址
    size_t total_size;          // 池总容量
    kwcc_slot_t *slots;         // 槽位索引管理数组（独立小 malloc）
    uint32_t total_slot_count;  // 总槽位数（各 slab 之和）
    kwcc_slab_t slabs[KWCC_NUM_SIZE_CLASSES];  // 3 个 size class
    uint32_t last_gc_time;      // 上次 GC 的 time(NULL)，用于节流
    int is_user_land;           // 0=Core, 1=User（调试用）
} kwcc_mem_pool_t;
```

### key 存储策略

- **key ≤ 31 字节**：直接存 `key_buf`，`key` 指针指向 `key_buf`（零额外 malloc）
- **key > 31 字节**：从 raw_memory 的 key 区分配，`key` 指向那
- **config key 如 `"theme"`、`"width"` 最多 10 字节，99% 走内联路径**

### 查找算法

```c
kwcc_slot_t* kwcc_pool_get(kwcc_mem_pool_t *pool, const char *key) {
    uint32_t h = fnv1a(key);
    uint32_t now = time(NULL);
    for (uint32_t i = 0; i < pool->total_slot_count; i++) {
        if (!pool->slots[i].in_use) continue;
        if (pool->slots[i].hash != h) continue;  // uint32 比较，纳秒级
        if (strcmp(pool->slots[i].key, key) != 0) continue;  // hash 相同时才 strcmp
        pool->slots[i].last_access = now;  // 自动更新访问时间
        return &pool->slots[i];
    }
    return NULL;
}
```

**为什么不用 hash 桶表**：
- FNV-1a 是 32 位 hash，几千 key 量级下碰撞概率趋近于零
- 即使碰撞，`strcmp` 回退也能正确区分，数据不会丢
- Core 140 槽、App 388 槽、User 832 槽，线性扫描 ≈ 100 纳秒~3 微秒
- hash 桶表需要额外维护链表，多 ~200 行 C 代码，收益不值得

### 分配算法

```c
kwcc_slot_t* kwcc_pool_alloc(kwcc_mem_pool_t *pool, const char *key,
                              uint32_t size, uint32_t timeout_sec) {
    uint32_t now = time(NULL);

    // 1. 遍历 slabs，找第一个 chunk_size >= size 的（从小到大，选最小适配）
    for (int i = 0; i < KWCC_NUM_SIZE_CLASSES; i++) {
        kwcc_slab_t *slab = &pool->slabs[i];
        if (slab->chunk_size < size) continue;

        // 2. 从隐式链表分配（O(1)，任意释放顺序安全）
        uint16_t idx = slab_alloc(slab);
        if (idx == 0xFFFF) {
            continue;  // 当前桶满了，试下一个更大的桶（同池内冒泡）
        }

        kwcc_slot_t *s = &pool->slots[idx];
        // 3. 初始化槽位（同前文）...
        s->data = slab->memory + idx * slab->chunk_size;
        // ...
        return s;
    }
    return NULL;  // 池满，绝不跨池越界
}
```

### GC 策略

```c
/* 普通 GC（自动节流：5 秒内跳过） */
void kwcc_pool_gc(kwcc_mem_pool_t *pool) {
    uint32_t now = time(NULL);
    if (now - pool->last_gc_time < 5) return;  // 节流跳过
    pool->last_gc_time = now;
    kwcc_pool_gc_internal(pool, now);
}

/* 强制 GC（重置节流，立即执行） */
void kwcc_pool_gc_force(kwcc_mem_pool_t *pool) {
    pool->last_gc_time = 0;  // 重置节流
    kwcc_pool_gc(pool);
}

/* 内部 GC 逻辑 — 三层兜底 */
static void kwcc_pool_gc_internal(kwcc_mem_pool_t *pool, uint32_t now) {
    for (uint32_t i = 0; i < pool->total_slot_count; i++) {
        kwcc_slot_t *s = &pool->slots[i];
        if (!s->in_use) continue;

        // 第一层：ref=0 的孤儿槽位立即回收（无人认领的内存）
        if (s->ref_count == 0) {
            kwcc_pool_free_slot(pool, s);
            continue;
        }

        // 第二层：超时强制回收（timeout_sec > 0 且已超时）
        if (s->timeout_sec > 0 && (now - s->alloc_time) >= s->timeout_sec) {
            log_warn("pool: slot %d (key=%s) expired after %ds, forcing reclaim",
                     i, s->key_buf, now - s->alloc_time);
            kwcc_pool_force_invalidate(pool, s);
        }
    }
}

/* 自动节流 + 80% 使用率强制 GC */
void kwcc_pool_gc_auto(kwcc_mem_pool_t *pool) {
    // 先算使用率
    int used = 0;
    for (uint32_t i = 0; i < pool->total_slot_count; i++) {
        if (pool->slots[i].in_use) used++;
    }
    float usage = (float)used / pool->total_slot_count;
    if (usage > 0.8f) {
        // 池快满了，不管节流，强制 GC
        kwcc_pool_gc_force(pool);
        return;
    }
    kwcc_pool_gc(pool);
}
```

### 生命周期跟踪（池自身维护，开发者无需管）

| 操作 | 自动处理 | 开发者需要 |
|------|---------|-----------|
| 分配时 | `alloc_time`, `last_access` 自动设，`ref_count = 0` | 需要持久持有则调 `acquire()` |
| 读取时 | `get()` 内自动更新 `last_access` | 无 |
| 引用计数 | `acquire/release` 自动维护，ref=0 时 GC 回收 | 调 `acquire`/`release` |
| GC 清理 | 每 5 秒自动扫描：ref=0 立即回收 + timeout 兜底；使用率 >80% 强制 GC | 可选调 `gc_force` |

### Reentrancy 与内存泄漏防护

#### 问题背景

kwcc 是单线程架构，C→JS 的 Bridge 调用是**同步嵌套**：`JS_Eval()` 阻塞执行 JS 回调，完成后才返回 C 层。这导致经典的**单线程重入 bug**：

```
C: http_on_read()
  ├─ slot = kwcc_pool_alloc()    → 分配槽位
  ├─ recv(fd, slot->data)         → 接收数据中
  ├─ kwcc_dispatch_event()        ← 同步调 JS_Eval("$bus.emit(...)")
  │   └─ JS 回调:
  │       $config.releaseUser()   ← JS 侧释放槽位
  │   ← JS_Eval 返回
  ├─ slot->data 继续使用...       ← 💥 slot 已被 JS 侧提前释放
  └─ kwcc_pool_release()          ← 💥 二次释放 / 踩踏
```

从 `c_architecture.md` 确认的调用链：`frame() → kwcc_io_poll_once() → slot->callback() → kwcc_dispatch_event() → JS_Eval()`。

#### 防护机制：ref_count + acquire/release 语义

**核心规则**：

| 操作 | ref_count 变化 | 说明 |
|------|---------------|------|
| `alloc()` | `ref_count = 0` | 初始无人引用 |
| `acquire()` | `ref_count++` | 声明"我需要持有这个槽位"。溢出时打 error 日志并忽略 |
| `release()` | `ref_count--` | 释放引用 |
| `release()` 后 ref=0 | **不立即回收** | 等待 GC 回收（安全，不在嵌套栈上释放） |

**C 侧标准用法**：

```c
http_on_read(int fd, void *user_data) {
    kwcc_slot_t *s = kwcc_pool_alloc(pool, "http", 4096, 0);
    if (!s) return;

    kwcc_pool_acquire(pool, s);   // ← 声明 C 侧持有
    recv(fd, s->data, s->capacity, 0);

    // dispatch 给 JS（JS 侧 setUser 也会 acquire）
    kwcc_dispatch_event(ctx, "http/data", s->key);

    kwcc_pool_release(pool, s);   // ← C 侧放手，ref_count--
    // 如果 JS 也 release 了，ref_count=0，下一轮 GC 回收
    // 如果 JS 没 release，ref_count=1，由 timeout 兜底
}
```

**即使 C 侧忘记 release**：

```
alloc → ref=0
JS setUser → ref=1
JS release → ref=0
GC 扫到 ref=0 → 回收 ✅（不会泄漏）
```

#### 三层泄漏防护

| 层级 | 条件 | 动作 | 处理场景 |
|------|------|------|---------|
| **第一层** | `ref_count == 0` | GC 立即回收 | C 忘记 release、early return 等 |
| **第二层** | `timeout_sec > 0` 且超时 | 强制回收 + warn 日志 | 长期持有者忘记释放 |
| **第三层** | `timeout_sec == 0` | 永不超时 | EventSource 等长连接，开发者需自行管理生命周期 |

**关键设计**：

- `alloc()` 后不调 `acquire()` 的槽位（ref=0），**下一轮 GC 自动回收**（最多 5 秒延迟）
- `release()` 只减 ref_count，**不在调用栈上直接回收**，避免 reentrancy 踩踏
- GC 是独立函数，不在 `http_on_read` 的调用栈上回收——即使 ref 归零，槽位也等到下一轮 GC 才标记空闲
- EventSource 长连接：`timeout_sec=0` + C 侧主动 `acquire` 持有，不会被误杀

#### 典型场景 ref_count 变化

```
场景：HTTP 响应
  alloc()              → ref=0
  C acquire()          → ref=1   （C 持有）
  JS setUser()         → ref=2   （JS 持有）
  C release()          → ref=1
  JS release()         → ref=0
  GC 扫到 ref=0        → 回收 ✅

场景：C 忘记 release
  alloc()              → ref=0
  C acquire()          → ref=1
  JS setUser()         → ref=2
  JS release()         → ref=1
  （C 忘记 release）   → ref=1
  GC 扫到 ref=1        → 不回收
  timeout 到期         → 强制回收 + warn ✅

场景：不持有任何引用（一次性数据）
  alloc()              → ref=0
  JS setUser()         → ref=1
  JS release()         → ref=0
  GC 扫到 ref=0        → 回收 ✅
```

### 全局声明

```c
extern kwcc_mem_pool_t g_core_pool;
extern kwcc_mem_pool_t g_app_pool;
extern kwcc_mem_pool_t g_user_pool;   // user_size=0 时 raw_memory=NULL
```

三块独立 `malloc`。User=0 时 Core+App 只占 ~300KB。

---

## 核心 API 设计

### 底层 C API（kwcc_pool.c/h）

```c
/* 初始化与清理 */
void kwcc_mem_init(const kwcc_runtime_spec_t *spec);
void kwcc_mem_shutdown(void);

/* 槽位操作 */
kwcc_slot_t* kwcc_pool_alloc(kwcc_mem_pool_t *pool, const char *key,
                             uint32_t size, uint32_t timeout_sec);
kwcc_slot_t* kwcc_pool_get(kwcc_mem_pool_t *pool, const char *key);
void         kwcc_pool_set(kwcc_mem_pool_t *pool, kwcc_slot_t *slot,
                           const void *data, uint32_t size);
void         kwcc_pool_release(kwcc_mem_pool_t *pool, kwcc_slot_t *slot);
void         kwcc_pool_acquire(kwcc_mem_pool_t *pool, kwcc_slot_t *slot);

/* 主动作废 */
void         kwcc_pool_invalidate(kwcc_mem_pool_t *pool, kwcc_slot_t *slot);

/* GC（自带节流，5 秒一次） */
void         kwcc_pool_gc(kwcc_mem_pool_t *pool);
void         kwcc_pool_gc_force(kwcc_mem_pool_t *pool);

/* 调试 */
void         kwcc_pool_dump_stats(kwcc_mem_pool_t *pool);
```

### JS Bridge API（mquickjs 层）

```javascript
// App 池：模块配置（永久，timeout=0）
$config.setApp(key, val)      // 写入 App 池
$config.setApp(key, null)     // 释放 App 池（等同于 releaseApp，与 setUser 语义一致）
$config.getApp(key)            // 从 App 池读取
$config.releaseApp(key)        // 手动释放 App 池

// User 池：临时数据（可指定超时秒数）
$config.setUser(key, val)      // 写入 User 池（默认不超时）
$config.setUser(key, val, 30)  // 写入 User 池，30 秒超时
$config.setUser(key, null)     // 释放 User 池（等同于 releaseUser，与 setApp 语义一致）
$config.getUser(key)           // 从 User 池读取
$config.releaseUser(key)       // 手动释放 User 池

// 调试
$config.dump()                              // 各池概要 → 控制台
$config.dumpAll("path")                     // 槽位元信息 → 写到文件
$config.dumpAll("path", true)               // 元信息 + 数据内容明细 → 写到文件
```

### 调试 Dump 规范

#### `dump()` — 各池概要（控制台输出）

```
=== Memory Pool Dump ===
Core: 18/32 KB (42 used, 66 free)
App:  64/256 KB (28 used, 113 free)
User: 128/1024 KB (5 used, 811 free)
```

用途：快速判断哪个池满了、使用率如何。

#### `dumpAll(filepath, show_content)` — 逐槽位详情（写入文件）

```javascript
$config.dumpAll("pool_dump.txt")         // 元信息（不含数据内容）
$config.dumpAll("pool_dump.txt", true)   // 元信息 + 数据内容明细
```

**无第 2 参时**：只输出槽位元信息（key/size/ref/timeout/age），快速查看各槽位分布状态。
**第 2 参 = true 时**：输出数据内容明细（按文本/二进制分段规则）。

```
=== Core Pool (18/32 KB, 42 used, 66 free) ===
slot 0: key="theme", size=4/64B, ref=1, timeout=0, age=120s
  data: "dark"

slot 1: key="font", size=6/64B, ref=1, timeout=0, age=120s
  data: "Roboto"

=== App Pool (64/256 KB, 28 used, 113 free) ===
slot 0: key="module/test/config", size=12/256B, ref=1, timeout=0, age=45s
  data: "theme=dark\n"

slot 1: key="http_resp", size=1024/16384B, ref=2, timeout=10, age=3s
  data: {"status":200,"data":"..."}

=== User Pool (128/1024 KB, 5 used, 811 free) ===
slot 0: key="form_data", size=256/1024B, ref=1, timeout=0, age=30s
  data: "name=test&age=25&..."

slot 5: key="http_body", size=8192/16384B, ref=1, timeout=10, age=2s
  content (text, 8192 bytes):
    {"id":1,"name":"test"}... [truncated, total 8192 bytes]

slot 8: key="img_buffer", size=4096/16384B, ref=1, timeout=30, age=5s
  content (binary, 4096 bytes):
    0000: 89 50 4E 47 0D 0A 1A 0A 00 00 00 0D 49 48 44 52 | .PNG..........IHDR
    0016: 00 00 00 20 00 00 00 20 08 02 00 00 00 FC 18 ED | ... ... ........
    ... [3840 more bytes, not shown]
```

**数据内容输出规则**：

| 类型 | 判定 | 输出方式 |
|------|------|---------|
| **纯文本** | 全部是可打印 ASCII/UTF8 | 前 128 字节原样输出 + `... [truncated, total X bytes]` |
| **二进制** | 含不可打印字节 | hex dump，每行 16 字节，最多 4 行（64 字节）+ `... [X more bytes, not shown]` |

**设计约束**：
- 单个 slot 的 dump 内容最多占 ~10 行，不会刷屏
- 开发者调试时 `cat kwcc_pool_dump.txt \| grep http_resp` 即可定位
- 两个 dump 方法可被 `#ifdef KWCC_DEBUG` 编译宏去掉，不影响发布包体积

#### C 层对应函数

```c
/* 概要 dump（打印到 log） */
void kwcc_pool_dump_stats(kwcc_mem_pool_t *pool);

/* 详情 dump + 可选数据内容（写入文件） */
void kwcc_pool_dump_all(kwcc_mem_pool_t *pool, const char *filepath, int show_content);
```

**使用原则**：
- JS 正常 `var` 变量继续用 mquickjs 堆
- `$config` 只给需要跨帧持久化 / 大缓冲的场景（**可选使用**）
- 方法名直接体现池子，一目了然

### 旧 `__kwcc_config` 迁移与清理步骤

内存池上线后，旧的 JS 全局对象方案（`kwcc.c` 中的 `__kwcc_config`）需要逐步淘汰：

**迁移步骤**：

1. **C 层**：`kwcc.c` 保留现有 `__kwcc_config` 实现不变，确保现有代码不崩溃
2. **JS 层**：在 `main.js` 中将 `__kwcc_config` 的读写全部迁移到 `$config.setApp/getApp`
3. **验证**：确认所有 config 读写都走 App 池后，进行清理

**清理步骤**（迁移完成后执行）：

```
1. kwcc.c — 删除内容：
   - g_js_ctx 全局变量
   - g_configs[] 数组 + JSGCRef 管理
   - kwcc_config_set_jsctx()
   - kwcc_config_set_object()
   - kwcc_config_set() / kwcc_config_get() / kwcc_config_get_int32()
   - __kwcc_config JS 全局对象创建代码

2. kwcc_base.h — 删除内容：
   - kwcc_config_get() 声明
   - kwcc_config_get_int32() 声明

3. kwcc_js.c — 删除内容：
   - js_kwcc_config_set stub 函数
   - CONFIG_KWCC 中的 "kwcc_config_set" 注册

4. mqjs_stdlib.c — 删除内容：
   - #ifdef CONFIG_KWCC 中的 JS_CFUNC_DEF("kwcc_config_set", ...) 注册行

5. main.m — 删除内容：
   - kwcc_config_set_jsctx() 调用

6. app/main.js — 更新内容：
   - 替换所有 kwcc_config(module, options) 为 $config.setApp(key, val)
   - 替换所有 __kwcc_config 直接访问为 $config.getApp(key)
```

**注意**：清理步骤在迁移验证通过后执行，不急于一次性完成。清理期间可以先保留旧代码作为 fallback。

---

## 后续待推演事项

以下问题尚未讨论确定，需在实施前逐一推演：

1. ~~槽位查找算法~~ ✅ 线性扫描 + hash 预过滤
2. ~~key 存储位置~~ ✅ 内联 32B + raw_memory fallback
3. ~~超时/生命周期管理~~ ✅ 引用计数 + timeout_sec + 三层兜底（ref=0 回收 + timeout 强制）
4. ~~GC 与帧率解耦~~ ✅ 真实时间戳 + 独立函数 + 自动节流
5. ~~分配时 slab 满 fallback~~ ✅ 试下一个更大的桶
6. ~~分配时自动更新时间~~ ✅ alloc/last_access 在 alloc 时自动设
7. ~~跨槽链式拼接~~ 暂不做（>最大 chunk_size 返回 NULL，由调用方拆分或换池）
8. **与现有代码集成**：
   - ~~`kwcc_bus.c` 的 `g_topic_map`~~ ❌ 不迁移，每帧清零的 C 结构体数组不需要池管理
   - ~~SVG cache（`g_svg_cache[128]`）~~ ❌ 不迁移，已有独立成熟的帧安全淘汰机制
   - `__kwcc_config` → ✅ 迁移到 **App 池**，替代旧的 JS `__kwcc_config` 全局对象方案
9. ~~错误处理~~ ✅ 分配失败返回 NULL + warn 日志，不返回错误码；JS 层返回 undefined 静默失败
10. ~~文件拆分~~ ✅ 独立 `kwcc_pool.c/h`，不塞入 `kwcc_base.h`
11. ~~单线程 Reentrancy 防护~~ ✅ ref_count + acquire/release 语义，release 不直接回收
12. ~~内存泄漏防护~~ ✅ 三层兜底：ref=0 GC 回收 + timeout 强制 + 开发者自行管理（timeout=0）
13. ~~ref_count 类型~~ ✅ uint16_t + acquire 溢出检查
14. ~~跨平台内存对齐~~ ✅ kwcc_aligned_malloc 封装函数
15. ~~JS API 一致性~~ ✅ setApp/setUser(key, null) 语义统一
16. ~~GC 80% 阈值~~ ✅ 使用率 >80% 自动强制 GC
17. ~~旧 __kwcc_config 迁移清理~~ ✅ 6 步清理清单（C/JS/Makefile）

---

## 决策记录

| 决策 | 结论 |
|------|------|
| 8MB 固定池 | ❌ 太重 → 改为三段独立 malloc，默认 ~1.3MB |
| FNV-1a 无碰撞防护 | ❌ 不安全 → 槽位加 `key` 指针，碰撞时 `strcmp` 回退 |
| 固定槽位大小不可变 | ❌ 死板 → 每池 3 个 size class（slab 分配器） |
| 强制 JS 用 userMem | ❌ 反直觉 → 可选使用，`var` 变量继续用 |
| INI 配置文件 | ❌ 不需要 → struct 传参（C 传统），未来需要再加 |
| 三段全部可运行时配置 | ✅ |
| Core/App 编译时默认 + 运行时覆盖 | ✅ |
| User 可为 0（不分配） | ✅ |
| 单独 hash 桶索引表 | ❌ 不需要，线性扫描 + hash 预过滤 = 伪 O(1) |
| key 单独 malloc | ❌ 碎片 → 内联 32B key_buf + raw_memory fallback |
| 三块连续 malloc | ❌ 独立 malloc，User=0 时只占 ~300KB |
| 生命周期跟踪谁维护 | ✅ 池自身维护，开发者只管 alloc/get/release |
| 超时用帧号还是时间 | ✅ 真实时间戳 `time(NULL)`，脱离帧率 |
| GC 每帧调用 | ❌ 独立函数 + 自动节流 5 秒，任何地方可调用 |
| 超时请求变黑洞 | ✅ 双超时：ref=0 淘汰 + timeout_sec 强制回收 |
| slab 满时分配失败 | ✅ fallback 试下一个更大的桶 |
| 分配时不更新 last_access | ✅ 必须更新，防刚分配就被 GC 误杀 |
| SVG cache 迁移到 App 池 | ❌ 不迁移，存 NSVGimage* 指针，已有独立帧安全淘汰 |
| g_topic_map / g_sync_table 迁移 | ❌ 不迁移，每帧清零的 C 数组不需要池管理 |
| __kwcc_config 放在哪 | ✅ App 池，替代旧的 JS 全局对象方案 |
| g_topic_map 放在哪 | 留在 kwcc_bus.c |
| JS API 统一入口 | ✅ `$config` 一个全局变量，`setApp/setUser/getApp/getUser/releaseApp/releaseUser` |
| 文件拆分方式 | ✅ 独立 `kwcc_pool.c/h`，不塞入 `kwcc_base.h` |
| 错误处理方式 | ✅ 分配失败返回 NULL + warn 日志，不返回错误码；JS 层返回 undefined 静默失败 |
| Fallback 跨池越界 | ❌ 禁止，仅限同池内 Small→Medium→Large 冒泡，最大桶满 → NULL |
| Free List 栈结构 | ❌ 随机释放会踩踏 → 改为 chunk 内嵌隐式链表（任意顺序真 O(1)） |
| Pool 函数原子化 | ✅ 纯 C 函数不含 yield，天然兼容 protothread，不加锁 |
| 内存对齐（旧版） | ✅ macOS malloc 天然 16 字节对齐 → 改为封装 kwcc_aligned_malloc 跨平台函数（见下方） |
| alloc 时 ref_count 初始值 | ✅ `ref_count = 0`（不是 1），需要持久持有则显式 `acquire()` |
| release 语义 | ✅ 只减 ref_count，不直接回收槽位（防 reentrancy 踩踏） |
| ref=0 何时回收 | ✅ 由 GC 统一回收，不在 release 调用点回收 |
| 内存泄漏防护 | ✅ 三层：ref=0 GC 回收 + timeout 强制 + 开发者管理（timeout=0） |
| C 忘记 release | ✅ 不会泄漏，JS release 后 ref=0 由 GC 回收，或 timeout 兜底 |
| early return 泄漏 | ✅ timeout_sec > 0 时不会永久泄漏，GC 强制回收 |
| ref_count 类型 | ✅ uint16_t（max 65535），acquire 时检查溢出 |
| 内存对齐 | ✅ 封装 kwcc_pool_aligned_malloc 跨平台函数 |
| 函数命名规范 | ✅ 三段式 `kwcc_{模块}_{动作}`，内部函数用 `static` + 统一前缀 |
| JS API 一致性 | ✅ setApp/setUser(key, null) 语义统一为释放 |
| GC 80% 阈值 | ✅ 池使用率 >80% 自动触发强制 GC，不等节流 |
| 旧 __kwcc_config 清理 | ✅ 迁移验证后执行 6 步清理（C/JS/Makefile 全面移除） |
| hash 桶表扩展 | ❌ 不需要，最大池 832 槽线性扫描 ≈ 1~3 微秒 |
| 超大数据自动拆分 | ❌ 不需要，最大 chunk 16KB 足够，超限由调用方处理 |
| 毫秒级超时 | ❌ 不需要，秒级够用，文档说明即可 |
