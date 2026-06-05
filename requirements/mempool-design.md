# 三段式 Slab 内存池设计文档（v7 — 按池类型分层 + TLV 序列化 + 多池管理）

## Context

内存池方案从业务分层（Core/App/User）重构为**按池类型分层**（L0-L7），值存储使用 **TLV 二进制格式**，支持层级数据快速路径查询。`$config` 是唯一 JS 公开 API。

**重要知识（来自记忆文件）**：
- mquickjs 是 ES5 语法，不支持 `let/const/箭头函数/...展开`，JS 侧必须用 `var` 和 `new Object()`
- mquickjs C API：JSValue 是 `uint64_t` 值类型，无 `JS_FreeValue`，GC 用 `JS_PushGCRef/JS_AddGCRef`
- `JS_ToCString` 可能返回 NULL，必须检查
- `{}` 在语句开头被解析为 block，JS 侧用 `new Object()` 或括号强制
- mquickjs 不支持 `...rest` 展开参数，JS 侧传固定参数
- `kwcc_register_ui` 中通过 `JS_Eval` 注入 JS wrapper 到 `kwcc_ui.c`
- C 函数注册通过 `CONFIG_KWCC` + `JS_CFUNC_DEF` 在 `mqjs_stdlib.c` 中
- mquickjs 内部 API 暴露于 `mquickjs_priv.h`（`js_object_keys` 等），外部可用
- macOS 10.14 编译：`.m` 扩展名、`-fobjc-arc`、`SOKOL_GLCORE`
- 日志用 `llog.h`，不是直接 `log.h`（syslog.h 宏冲突）

## 池类型（按 数据类型 + 长度 划分）

L0-L7 不是层级，而是**池类型**。一个池实例只有一种类型（固定 chunk 大小）。每种池独立管理槽数、独立扩缩容。

```
kwcc_mempool_type_t:
  KWCC_MEMPOOL_L0  — 8B      基础类型 (int/float/bool)
  KWCC_MEMPOOL_L1  — 32B     极短字符串 (keys, IDs, 开关)
  KWCC_MEMPOOL_L2  — 128B    短字符串 (配置值, URL, 标签)
  KWCC_MEMPOOL_L3  — 512B    中字符串 (错误消息, JSON片段)
  KWCC_MEMPOOL_L4  — 1KB     较长字符串 (短JSON响应, 配置对象)
  KWCC_MEMPOOL_L5  — 4KB     长字符串 (HTTP body, 日志)
  KWCC_MEMPOOL_L6  — 16KB    大字符串 (SVG, 大文本)
  KWCC_MEMPOOL_L7  — 动态    malloc 无上限 (非 slab, 固定 128 元信息槽)
```

alloc 时按数据类型 + 长度自动路由到对应池类型：
- `type != string` → L0
- `size <= 32` → L1
- `size <= 128` → L2
- `size <= 512` → L3
- `size <= 1KB` → L4
- `size <= 4KB` → L5
- `size <= 16KB` → L6
- `> 16KB` → L7（动态 malloc）

## 每池规格（单种池类型）

| 池类型 | chunk 大小 | 槽数/池 | 数据内存 | 元数据(80B/槽) | 合计/池 | 用途 |
|--------|------------|---------|----------|----------------|---------|------|
| L0 | 8B | 128 | 1KB | 10KB | ~11KB | int/float/bool |
| L1 | 32B | 128 | 4KB | 10KB | ~14KB | keys, IDs, 开关 |
| L2 | 128B | 128 | 16KB | 10KB | ~26KB | 配置值, URL |
| L3 | 512B | 128 | 64KB | 10KB | ~74KB | 错误消息, JSON片段 |
| L4 | 1KB | 128 | 128KB | 10KB | ~138KB | 短JSON响应, 配置对象 |
| L5 | 4KB | 32 | 128KB | 2.5KB | ~131KB | HTTP body, 日志 |
| L6 | 16KB | 8 | 128KB | 0.6KB | ~129KB | SVG, 大文本 |
| L7 | 动态 | 128 | — | 10KB | ~10KB | 非 slab |

每种池独立扩缩容，max_pools 可分别配置：
```c
max_pools[KWCC_MEMPOOL_MAX_TYPES] = { 16, 8, 8, 4, 4, 4, 2, 2 };
```

L0 最多 16 池（2048 个 key），L3 最多 4 池（512 个 key），L6/L7 最多 2 池。

**初始化（1 池/类型，8 种）**：~530KB
**满载（max_pools 全部展开）**：~2.1MB

## 值编码方式

mempool 层是纯 KV 存储（字节数组），不关心值的内容。编码由 `$config` 层决定：

**方式一：按类型直接存储（默认）**
```js
$config.appSetString("name", "myapp");
$config.appSetInt("count", 42);
$config.appSetBool("enabled", true);
```
→ 按类型存原生格式（int/bool 走常量表），零编码开销。

**方式二：JSON 字符串**
```js
$config.appSetJson("io", { max_fds: "16", port: "8080" });
```
→ JS 对象 → C 侧 JSON.stringify → 存 JSON 字符串，JS 侧 `JSON.parse()` 解。mquickjs 内置 JSON，零实现成本。

**方式三：TLV 二进制（可选，用于对象/层级数据）**
```js
$config.appSetTlv("io", { max_fds: "16", port: "8080" });
```
→ `$config` 内部调用 `kwcc_mempool_tlv_build()` 将 JSValue 对象编码为 TLV 二进制存 slot。
→ 读取时 C 侧 TLV 路径查询，返回字符串/JSON 字符串。
→ **比 JSON 省 30% 空间**，适合大量层级配置。

**总结：mempool 不管编码，只存字节。TLV 只是 `$config` 层的一个编码选项。**

### TLV 条目格式
```
Type(1B) + TotalLen(2B, 小端) + Name(str) + Value(data)
```

### 类型定义
```c
KWCC_MEMPOOL_TLV_FIELD  = 0x01   // 基础类型（string/int/bool）
KWCC_MEMPOOL_TLV_OBJECT = 0x02   // 子对象（Value 是子 TLV 块）
KWCC_MEMPOOL_TLV_ARRAY  = 0x03   // 数组（子元素用索引名 "0","1"...）
```

### TLV 示例
```
TLV 块：slot[42]->data = [io.timeout.user = "v1", io.timeout.enabled = true]

[OBJECT "io" len=40]
  [OBJECT "timeout" len=24]
    [FIELD  "user"    len=2]  "v1"
    [FIELD  "enabled" len=1]  true
  [FIELD "version"    len=3]  "2.0"
```

### TLV API（纯 C，无 JS 依赖）

**架构原则**：`kwcc_mempool` 是纯 C 基础设施，不依赖 `JSContext`/`JSValue`。
所有 JS 相关逻辑放在 `kwcc_js.c`，通过回调钩子与 mempool 交互。

```c
/* ═══ kwcc_mempool（纯 C，不依赖 JS）═══ */

/* 回调：构建 TLV 时，调用方提供 key/value 对 */
typedef int (*kwcc_mempool_tlv_pack_cb)(const char *key, const char *value,
                                         uint8_t type, void *user_data);

/* 回调：遍历 TLV 时，通知每个条目 */
typedef int (*kwcc_mempool_tlv_iter_cb)(const char *name, const uint8_t *value,
                                         size_t value_len, uint8_t type, void *user_data);

/* 构建 TLV：调用方通过回调驱动，mempool 负责序列化 */
uint8_t *kwcc_mempool_tlv_build(kwcc_mempool_tlv_pack_cb cb, void *user_data,
                                 size_t *out_len);

/* 遍历 TLV：解析字节，逐条回调 */
int kwcc_mempool_tlv_iter(const uint8_t *tlv_data, size_t tlv_len,
                           kwcc_mempool_tlv_iter_cb cb, void *user_data);

/* 路径查询：返回 TLV 内部字节指针（不 malloc） */
const char *kwcc_mempool_tlv_get_path(const uint8_t *tlv_data, size_t tlv_len,
                                       const char *path, size_t *out_len);

/* TLV → JSON 字符串（malloc） */
char *kwcc_mempool_tlv_to_json(const uint8_t *tlv_data, size_t tlv_len, size_t *out_len);
void  kwcc_mempool_tlv_free_json(char *ptr);
```

```c
/* ═══ kwcc_js.c（JS 层，依赖 mquickjs）═══ */

/* JSValue → TLV：遍历 JS keys，通过 tlv_build 回调逐个转换 */
/* TLV → JSValue：用 tlv_get_path 拿到 C 字符串 → JS_NewStringLen */
/* TLV → JSON：用 tlv_to_json 拿到 JSON 字符串 → JS_NewStringLen */
```

### TLV 读写转换流程
```
set: JS对象 → C侧 kwcc_mempool_tlv_build() → TLV字节 → 存slot
get: 读slot → TLV字节
  → 无path: kwcc_mempool_tlv_to_json() → JSON字符串给JS → JSON.parse()
  → 有path: kwcc_mempool_tlv_get_path(tlv, "timeout/user") → 直接返回值

$config.appSetTlv("io", { timeout: { user: "v1" } })
  → C 侧 kwcc_mempool_tlv_build() → 打包 TLV → 存slot

$config.appGetTlv("io")
  → C侧读slot → TLV块 → tlv_to_json() → JSON字符串给JS

$config.appGetTlv("io", "timeout/user")
  → C侧读slot → TLV块 → tlv_get_path("timeout/user") → "v1"
```

### TLV 安全边界（强制）

TLV 数据可能来自外部不可信源（HTTP 响应、文件读取等），解析时 **绝不信任数据内的 Length 声明**，所有 TLV 解析函数必须在每次指针移动前做边界检查：

```c
// 安全解析模板：每次偏移前必须校验
static int tlv_parse_entry(const uint8_t *ptr, const uint8_t *end, ...) {
    if (ptr + 3 > end) return TLV_ERROR_TRUNCATED;          // Type(1B) + Len(2B) 不完整
    uint16_t total_len = read_le16(ptr + 1);
    if (total_len < 3) return TLV_ERROR_INVALID_LEN;         // 长度异常
    if (ptr + total_len > end) return TLV_ERROR_OVERRUN;     // 越界保护
    ...
}
```

**必须加边界检查的函数**：
- `kwcc_mempool_tlv_get_path()` — 路径查询，每次跳子节点前校验
- `kwcc_mempool_tlv_to_json()` — JSON 转换，遍历所有条目时校验
- `kwcc_mempool_tlv_build()` — 输出缓冲区满时校验（防内存溢出）
- 任何直接读 TLV 字节的内部辅助函数

## 数据类型（slot->type，uint8_t）

```c
enum {
    KWCC_MEMPOOL_TYPE_STRING = 0,  // 纯字符串
    KWCC_MEMPOOL_TYPE_INT32  = 1,  // 32位整数
    KWCC_MEMPOOL_TYPE_INT64  = 2,  // 64位整数
    KWCC_MEMPOOL_TYPE_FLOAT  = 3,  // 浮点
    KWCC_MEMPOOL_TYPE_DOUBLE = 4,  // 双精度
    KWCC_MEMPOOL_TYPE_JSON   = 5,  // JSON字符串（读取时自动decode）
    KWCC_MEMPOOL_TYPE_TLV    = 6,  // TLV二进制块（读取时自动解码）
    KWCC_MEMPOOL_TYPE_CONST  = 7,  // 常量引用（指向 g_kwcc_mempool_const_table）
};
```

C 侧 `memcpy` 直接存原生格式，JS 侧走字符串（自动转）。

## 常量表（16 个高频值）

```c
// 字符串常量
KWCC_MEMPOOL_CONST_NULL     = 0,    // "null"
KWCC_MEMPOOL_CONST_EMPTY    = 1,    // ""
KWCC_MEMPOOL_CONST_ZERO     = 2,    // "0"
KWCC_MEMPOOL_CONST_ONE      = 3,    // "1"
KWCC_MEMPOOL_CONST_TRUE     = 4,    // "true"
KWCC_MEMPOOL_CONST_FALSE    = 5,    // "false"

// 布尔常量
KWCC_MEMPOOL_CONST_TRUE_BOOL  = 6,  // true (BOOL)
KWCC_MEMPOOL_CONST_FALSE_BOOL = 7,  // false (BOOL)

// 整数常量
KWCC_MEMPOOL_CONST_M1       = 8,    // -1 (INT32)
KWCC_MEMPOOL_CONST_0_INT    = 9,    // 0 (INT32)
KWCC_MEMPOOL_CONST_1_INT    = 10,   // 1 (INT32)

// 11-15 保留
```

```c
typedef struct {
    uint8_t  value[8];      // 常量数据
    uint8_t  real_type;     // 真实类型（BOOL/INT32/STRING）
    uint8_t  size;          // 数据大小
} kwcc_mempool_const_t;

kwcc_mempool_const_t g_kwcc_mempool_const_table[16];  // ~1.3KB
```

slot->type = CONST 时，data 指针指向常量区，不消耗 slab chunk。

```c
int kwcc_mempool_const_lookup(const void *data, size_t len, uint8_t type);
  // 通用常量查找：匹配 data+len+type → 返回常量表索引（-1=未匹配）
  // 替代 if/else 链，16 个常量遍历很快
```

## Slot 结构体

```c
typedef struct {
    uint32_t  hash;           /* FNV-1a hash */
    char      key_buf[32];    /* 短 key 内联 */
    const char *key;          /* → key_buf 或外部 malloc（长 key） */
    uint8_t  *data;           /* → slab chunk 或 g_kwcc_mempool_const_table */
    uint32_t  capacity;       /* chunk size */
    uint32_t  size;           /* actual bytes */
    uint32_t  alloc_time;     /* timestamp */
    uint32_t  last_access;    /* timestamp */
    uint32_t  timeout_sec;    /* 0=never */
    uint16_t  ref_count;
    uint8_t   pool_type;      /* L0-L7，L7=动态 */
    uint8_t   type;           /* STRING/INT32/JSON/TLV/CONST 等 */
    uint8_t   pool_idx;       /* 该类型第几个池 */
    uint8_t   slot_idx;       /* 该池第几个槽 */
    uint8_t   in_use;
} kwcc_mempool_slot_t;
/* 约 88B */
```

## 架构分层

```
C 层：kwcc_mempool（按 数据类型+长度 分池的 KV 存储）
  池类型：L0-L7 共 8 种，每池只有一种 chunk 大小
  多池管理：每种池独立扩缩容，独立 max_pools
  key_map：key → pool_id 映射（扁平 hash 表）
  池扩展：初始化开 1 池/类型，满即扩，直到该类型的 max_pools
  TLV 值存储（可选编码）：JS 对象 ↔ TLV 二进制转换

C 层 API（纯 KV，无默认值）：
  kwcc_mempool_alloc(data_type, key, size, timeout)      // 按 data_type + size 自动路由 L0-L6
  kwcc_mempool_alloc_dynamic(key, cap, timeout)         // 直走 L7（动态 malloc）
  kwcc_mempool_get(pool_id, key)                        // 返回 slot*
  kwcc_mempool_set(pool_id, slot, data, size)           // 写数据
  kwcc_mempool_release(pool_id, slot)                   // ref--
  kwcc_mempool_get_keys(pool_id, prefix)                // 前缀扫描
  kwcc_mempool_get_str(pool_id, key, default_value)     // 返回 null-terminated 字符串
  kwcc_mempool_const_lookup(data, len, type)            // 常量表查找

Key 前缀拼接（`$config` 层业务逻辑）：
  在 `kwcc_js.c` 中实现，负责给 key 拼 `a.` / `c.` 前缀。
  用途：JS wrapper 自动加业务前缀，防止 JS 注入恶意 key。
  例：`$config.appSetInt("io/port", 8080)` → C 侧 `"a.io/port"`

JS 层：$config（唯一公开的配置工具）
  分隔符约定：
    - . 是业务命名空间分隔符（C端内部加 a. / c. 前缀）
    - / 是前缀分组分隔符（appReleasePrefix、key_map 前缀扫描）
    - / 也是 TLV 路径查询分隔符（tlv_get_path 查子节点）
  key 语义：扁平字符串，/ 只是人为约定的前缀分隔符
  TLV 是唯一特例：值内部是树形结构，路径查询在值内容上操作

  App 域（读写，JS wrapper 自动加 a. 前缀）：
    appSetInt(key, val) / appSetString(key, val) / appSetBool(key, val)
    appSetJson(key, obj)     // JS对象 → JSON字符串
    appSetTlv(key, obj)      // JS对象 → TLV二进制
    appGet(key, default)     // 通用读取，C侧根据 slot->type 自动返回
    appGetTlv(key, path)     // TLV专用，支持路径查询（无path返回JSON字符串）
    appRelease(key)          // 释放单个 key
    appReleasePrefix(key)    // 释放 key/ 开头的所有 key

  Core 域（JS 可写、C 可读）：
    JS 写：只允许 coreSetTlv(key, obj) → 存 TLV 到 "c." 前缀
    C 读：直接调 `kwcc_mempool_get()` 拿 slot，根据 `slot->type` 调对应工具：
      - type=TLV → 调 `kwcc_mempool_tlv_get_path()` 解析
      - type=STRING → 直接读 `slot->data`
      - 无简化封装，C 模块直接用 mempool 提供的纯 C 工具

  池管理：
    setMaxPools(type, n)     // 调整某池类型最大池数
```

## $config 完整 API

```javascript
// App — 设置
$config.appSetInt("io/port", 8080);                // 存 int（走常量表）
$config.appSetString("name", "myapp");             // 存字符串
$config.appSetBool("enabled", true);               // 存 bool（走常量表）
$config.appSetJson("io", { max_fds: "16" });       // JS对象 → JSON字符串
$config.appSetTlv("io", { max_fds: "16" });        // JS对象 → TLV二进制

// Core — JS 设置（只允许 TLV）
$config.coreSetTlv("io", { max_fds: "16" });       // JS对象 → TLV → 存 "c.io"

// App — 读取
$config.appGet("name", "default");                 // 返回字符串
$config.appGet("io/port", 0);                      // 返回 int
$config.appGetTlv("io");                           // 无path → JSON字符串
$config.appGetTlv("io", "max_fds");                // 有path → "16"

// App — 释放
$config.appRelease("io/port");                     // 释放 a.io/port
$config.appReleasePrefix("io");                    // 释放 a.io/ 前缀所有

// Core — JS 端可写，C 端可读
// C 模块直接调 kwcc_mempool_get("c.io") 拿 slot，根据 type 调 tlv_get_path 解析

// 池管理
$config.setMaxPools("l5", 4);                      // L5 最大 4 池
$config.setMaxPools("*", 4);                       // 全部设为 4
```

**JS wrapper 内部**（在 `JS_Eval` 中注入）：
```javascript
// App 域 — 自动加 "a." 前缀
$config.appSetInt = function(k, v) { kwcc_config_set_int("a." + k, v); };
$config.appSetString = function(k, v) { kwcc_config_set_string("a." + k, v); };
$config.appSetBool = function(k, v) { kwcc_config_set_bool("a." + k, v); };
$config.appSetJson = function(k, v) { kwcc_config_set_json("a." + k, v); };
$config.appSetTlv = function(k, v) { kwcc_config_set_tlv("a." + k, v); };
$config.appGet = function(k, d) { return kwcc_config_get("a." + k, d); };
$config.appGetTlv = function(k, p, d) {
    if (p) { return kwcc_config_get_tlv_path("a." + k, p); }
    return kwcc_config_get_tlv_json("a." + k, d);
};
$config.appRelease = function(k) { kwcc_config_release("a." + k); };
$config.appReleasePrefix = function(k) { kwcc_config_release_prefix("a." + k + "/"); };

// Core 域 — 自动加 "c." 前缀
$config.coreSetTlv = function(k, v) { kwcc_config_set_tlv("c." + k, v); };

// 池管理
$config.setMaxPools = function(t, n) { kwcc_config_set_max_pools(t, n); };
```

## 多池管理

- **max_pools 默认 = { 16, 8, 8, 4, 4, 4, 2, 2 }**，每种池类型独立配置
- `$config.setMaxPools()` 可指定每种类型的上限，n < 4 自动修正为 4
- **池扩展策略**：每种类型初始化开 1 池，满即扩，直到该类型的 max_pools
- **L7 动态数据**：固定 128 元信息槽，不占 slab 空间，通过独立 malloc 分配
  - 不设硬上限，信任业务调用者（alloc_dynamic 本身就是要大内存的场景）
  - 但跟踪 L7 总物理内存用量（`g_kwcc_mempool_l7_used` 全局变量），dump 中可见
  - timeout 不强制，使用者自己传，不设兜底
- **key_map**：key → pool_id 映射（扁平 hash 表，32768 条目，O(1) 查找）
  - 前缀扫描：遍历匹配 "prefix/" ≈ 3μs（50 个实际条目）
  - 单 key 查找：hash 一次，O(1) ≈ 100ns
  - key_map 条目：{ hash, key指针, pool_type, pool_idx, slot_idx }

## key_map 设计

```c
#define KWCC_MEMPOOL_KEYMAP_SIZE 32768  /* 2^15，满载 14160 条目，负载率 43% */

typedef struct {
    uint32_t hash;          /* FNV-1a hash */
    const char *key;        /* 指向 slot 的 key_buf */
    uint8_t  pool_type;     /* L0-L7 池类型 */
    uint8_t  pool_idx;      /* 该类型第几个池 (0-based) */
    uint16_t slot_idx;      /* 该池第几个槽 */
} kwcc_mempool_keymap_entry_t;
```

- alloc 时写入 key_map
- free 时标记为无效
- get 时 hash 查表直接定位
- prefix 时遍历匹配

## 关键源文件

| 文件 | 作用 | 本次改动 |
|------|------|---------|
| `src/kwcc_mempool.h` | 重命名自 kwcc_pool.h | 类型定义 + API 声明 |
| `src/kwcc_mempool.c` | 重命名自 kwcc_pool.c | 核心实现（多池管理 + key_map + TLV + 常量表） |
| `src/kwcc.h` | umbrella header | 加 `#include "kwcc_mempool.h"` |
| `src/kwcc_js.c` | JS lifecycle | 添加 `$config` JS API + C handler + TLV pack/unpack/to_json |
| `src/kwcc_ui.c` | UI bridge | 在 `kwcc_register_ui` 中加 `$config` JS wrapper |
| `src/main.m` | Sokol lifecycle | `init()` 加 `kwcc_mempool_init()` |
| `src/kwcc_base.h` | base infra | 添加 `KWCC_DEBUG` 宏 + 编译默认宏 |
| `app/main.js` | JS entry | 加 `$config.setMaxPools` 调用 |
| `Makefile` | build | 加 `kwcc_mempool.c` 编译规则 |
| `deps/mquickjs/mqjs_stdlib.c` | stdlib | 加 C 函数注册（CONFIG_KWCC） |
| `src/kwcc_io.c` | I/O reactor | 迁移到 `kwcc_mempool_*` API |

## 清理步骤（旧 `__kwcc_config` 系统）

1. **`src/kwcc.c`** — 清空全部旧实现
2. **`src/kwcc_base.h`** — 删除旧 config 声明
3. **`src/kwcc_io.c`** — 迁移到 mempool API
4. **`src/kwcc_js.c`** — 删除 `kwcc_config_set_jsctx` + `js_kwcc_config_set`
5. **`src/kwcc_js.h`** — 删除旧声明
6. **`src/kwcc_ui.c`** — 删除 `kwcc_config` JS wrapper
7. **`deps/mquickjs/mqjs_stdlib.c`** — 删除 `kwcc_config_set` 注册

> **注意**：代码已回滚，所有清理步骤待重新执行。

## 当前状态

- 代码在 `kwcc_pool.h/c`（旧命名）
- Phase 1-7 均未开始，待从零执行

## 实施步骤（待执行，见下方 Phase 1-6）

## 关键设计决策

- **命名规范**：所有 C 符号统一 `kwcc_mempool_` 前缀；枚举/宏大写 `KWCC_MEMPOOL_*`；全局变量 `g_kwcc_mempool_*`
- **业务前缀**：JS wrapper 自动加 `a.`（App）/ `c.`（Core），在 `kwcc_js.c` 中实现；Core 可写可读
- **分隔符**：`.` 是业务命名空间分隔符，`/` 是前缀分组和 TLV 路径分隔符
- **key 语义**：扁平字符串，`/` 只是人为约定的前缀分隔符
- **JS API**：扁平方法名 `appSetInt/appSetString/appSetBool/appSetJson/appSetTlv/appGet/appGetTlv/appRelease/appReleasePrefix` + `coreSetTlv` + `setMaxPools`
- **常量查找**：`kwcc_mempool_const_lookup(data, len, type)` 通用函数，替代 if/else
- **ref_count**：`uint16_t`，`alloc()` 时 = 0，`acquire()` 时++，溢出打 error 忽略
- **GC 80% 阈值**：自动检测，超过立即强制 GC
- **$config 是唯一公开 API**，`$memory` 不暴露
- **类型标记**：uint8_t，0-7（STRING/INT32/INT64/FLOAT/DOUBLE/JSON/TLV/CONST）
- **L0-L7 按池类型分层**：每种池独立 chunk 大小 + 独立槽数 + 独立扩缩容
- **单池规格**：L0 ~11KB/128槽 到 L6 ~129KB/8槽，L7 仅 128 元信息槽
- **L7 动态 malloc**：不设硬上限，跟踪总用量 `g_kwcc_mempool_l7_used`（dump 可见），timeout 使用者自理
- **总内存**：初始化 ~530KB（1 池/类型），满载 ~2.1MB
- **池扩展**：满即扩，直到 max_pools，不预留
- **TLV build**：返回 `uint8_t*` 字节缓冲区，供 mempool 直接 `memcpy`
- **TLV to_json**：无 path 时返回 JSON 字符串给 JS，JS 侧 JSON.parse()
- **常量表**：16 个高频值（null/空串/true/false/0/1/-1），引用不占 slot chunk
- **key_map**：32768 条目 hash 表，单 key O(1)，前缀扫描 ~3μs
- **mquickjs ES5 限制**：JS wrapper 用 `var`，无展开参数
- **JS_ToCString NULL 保护**
- **TLV 实现位置**：纯 C 工具在 `kwcc_mempool.c`，JS 转换在 `kwcc_js.c`，include mquickjs_priv.h，使用 js_object_keys 内部 API
- **生命周期**：L0-L7 统一初始化（~550KB），App/Core 只是业务前缀

## 生命周期流程

```
init() — main.m
  │
  ├─ 1. kwcc_mempool_init()        → L0-L7 各开 1 池（~530KB）
  │                                    max_pools = 编译时默认值
  │
  ├─ 2. kwcc_create_js()           → JSContext 就绪，注入 $config
  │
  └─ 3. kwcc_register_ui(js_ctx)   → 注入 $config 全局 API
       │
       └─ 执行 main.js
            $config.setMaxPools("l5", 4);        // 运行时调整 max_pools
            $config.setMaxPools("*", 4);         // 全局调整
            $config.appSetTlv("io", { port: "8080" });  // 业务使用
            loadJs 业务代码...

frame() — 每帧
  ├─ kwcc_process_js(js_ctx, "onFrame();")  → 渲染
  └─ kwcc_mempool_gc_auto()                 → GC 自动节流
```

## 调试 Dump 功能

通过 `#ifdef KWCC_DEBUG` 编译宏控制，发布包剔除。

### JS API

```javascript
$config.dump();                              // 各池概要 → 控制台
$config.dumpAll("path");                     // 槽位元信息 → 写到文件
$config.dumpAll("path", true);               // 元信息 + 数据内容明细 → 写到文件
```

### C 层对应函数

```c
/* 概要 dump（打印到 log） */
void kwcc_mempool_dump_stats(void);

/* 详情 dump + 可选数据内容（写入文件） */
void kwcc_mempool_dump_all(const char *filepath, int show_content);
```

### `dump()` — 各池概要（控制台输出）

```
=== Memory Pool Dump ===
L0 (8B):  2 pools  [ 100/256 slots used ]
L1 (32B): 1 pool   [ 64/128 slots used ]
L2 (128B): 1 pool  [ 32/128 slots used ]
L3 (512B): 1 pool  [ 10/128 slots used ]
L4 (1KB): 1 pool   [ 5/128 slots used ]
L5 (4KB): 1 pool   [ 2/32 slots used ]
L6 (16KB): 1 pool  [ 1/8 slots used ]
L7 (dynamic): 0 pools [ 0/128 slots used, total 0MB ]
```

用途：快速判断哪个池满了、使用率如何、是否需要调整 max_pools。

### `dumpAll(filepath, show_content)` — 逐槽位详情（写入文件）

```javascript
$config.dumpAll("pool_dump.txt")         // 元信息（不含数据内容）
$config.dumpAll("pool_dump.txt", true)   // 元信息 + 数据内容明细
```

**无第 2 参时**：只输出槽位元信息（key/size/ref/timeout/age/pool_type）。
**第 2 参 = true 时**：输出数据内容明细（按文本/二进制分段规则）。

```
=== L0 Pool 0 (8B chunks, 128/128 slots) ===
slot 0: key="a.io/port", size=4/8B, ref=1, timeout=0, age=120s, type=CONST
  data: CONST[0] (KWCC_MEMPOOL_CONST_ONE)

slot 1: key="a.enabled", size=4/8B, ref=1, timeout=0, age=120s, type=INT32
  data: 42

=== L2 Pool 0 (128B chunks, 32/256 slots used) ===
slot 5: key="a.name", size=5/128B, ref=1, timeout=0, age=45s, type=STRING
  data: "myapp"

=== L4 Pool 0 (1KB chunks, 5/128 slots used) ===
slot 0: key="a.io/config", size=512/1024B, ref=1, timeout=0, age=30s, type=TLV
  content (text, 512 bytes):
    {"io":{"timeout":{"user":"v1"},"max_fds":"16"}}
```

**数据内容输出规则**：

| 类型 | 判定 | 输出方式 |
|------|------|---------|
| **纯文本** | 全部是可打印 ASCII/UTF8 | 前 128 字节原样输出 + `... [truncated, total X bytes]` |
| **TLV 二进制** | slot->type=TLV | 先尝试 `tlv_to_json()` 转 JSON 输出，失败则 hex dump |
| **CONST** | slot->type=CONST | 输出常量表索引和值 |
| **二进制** | 含不可打印字节 | hex dump，每行 16 字节，最多 4 行（64 字节）+ `... [X more bytes, not shown]` |

**设计约束**：
- 单个 slot 的 dump 内容最多占 ~10 行，不会刷屏
- 开发者调试时 `cat pool_dump.txt | grep "a.io"` 即可定位
- 两个 dump 方法可被 `#ifdef KWCC_DEBUG` 编译宏去掉，不影响发布包体积
- 遍历所有池类型 × 所有池实例 × 所有槽位，过滤 in_use 的

---

## 验证

- 编译通过（`make clean && make`）
- 窗口正常显示（`make run`）
- `$config.appSetTlv("io", { port: "8080" })` → TLV 存
- `$config.appGetTlv("io", "port")` → 返回 `"8080"`
- `$config.appGetTlv("io")` → 返回 JSON 字符串，JS `JSON.parse()` 得到对象
- `$config.appRelease("io/port")` → 释放 a.io/port
- `$config.appReleasePrefix("io")` → 释放 a.io/ 前缀所有
- 检查 `kwcc.log` 无错误日志

## 执行计划

### Phase 1: 基础骨架（重命名 + 新头文件 + 常量表 + 编译通过）

**目标**：文件重命名 + 新头文件定义 + 常量表 + Makefile 更新 + 编译通过

**改动文件**：
| 文件 | 操作 |
|------|------|
| `src/kwcc_pool.h` | 重命名为 `src/kwcc_mempool.h` |
| `src/kwcc_pool.c` | 重命名为 `src/kwcc_mempool.c` |
| `src/kwcc_mempool.h` | 全面重写：L0-L7 池类型 + slot 结构体 + key_map + 常量表 + API 声明 |
| `src/kwcc_mempool.c` | 全面重写：多池管理 + alloc/get/set/release + key_map + GC + 常量表 + L7 动态分配 |
| `Makefile` | `kwcc_pool.c` → `kwcc_mempool.c`，头文件依赖更新 |
| `src/kwcc.h` | `#include "kwcc_mempool.h"` |
| `src/kwcc_js.c` | 更新所有 `#include` 和符号引用 |
| `src/kwcc_io.c` | 更新所有符号引用（`kwcc_pool_*` → `kwcc_mempool_*`） |
| `src/kwcc_base.h` | 删除旧 config 声明 |
| `src/kwcc.c` | 清空全部旧实现 |
| `src/kwcc_ui.c` | 删除 `kwcc_config` JS wrapper |
| `deps/mquickjs/mqjs_stdlib.c` | 删除 `kwcc_config_set` 注册 |

**编译验证**：`make clean && make` 必须通过

**常量表（Phase 1 内做）**：
- `g_kwcc_mempool_const_table[16]` 初始化（null/空串/0/1/true/false/-1 等高频值）
- `kwcc_mempool_const_lookup(data, len, type)` — alloc 存 int/bool 时自动匹配常量表
- 匹配到常量时 slot->type = CONST，data 指向常量区，不占 slab chunk
- 这是 alloc 的必经路径，必须在 Phase 1 就做

---

### Phase 2: TLV 序列化

**目标**：TLV 打包/解包/路径查询 + to_json 转换 + JS ↔ TLV 集成

**改动文件**：
| 文件 | 内容 |
|------|------|
| `src/kwcc_mempool.c` | `kwcc_mempool_tlv_build()` / `tlv_iter()` / `tlv_get_path()` / `tlv_to_json()` — 纯 C，无 JS 依赖 |
| `src/kwcc_mempool.h` | TLV 类型 + API 声明 |
| `src/kwcc_js.c` | JSValue → TLV 打包（遍历 JS keys + 回调驱动） + TLV → JSValue 解包（调用 tlv_get_path / tlv_to_json） |

**关键约束**：
- `kwcc_mempool` 层：纯 C 回调钩子，不碰 `JSContext`/`JSValue`
- `kwcc_js.c` 层：JS 转换逻辑，通过回调调用 mempool 的纯 C 函数
- 使用 `mquickjs_priv.h` 内部 API（`js_object_keys`）
- `tlv_get_path()` 按 `/` 分隔符逐层查找
- TLV 安全边界：所有解析函数必须有边界检查

---

### Phase 3: $config JS API 重写

**目标**：新扁平 API + 业务前缀自动添加 + TLV + 常量表集成

**改动文件**：
| 文件 | 内容 |
|------|------|
| `deps/mquickjs/mqjs_stdlib.c` | 更新 `#ifdef CONFIG_KWCC` 函数声明 |
| `src/kwcc_js.c` | 重写所有 C handler + JS wrapper |
| `src/kwcc_ui.c` | 更新 `$config` JS wrapper 字符串（`JS_Eval` 注入） |

**C handler 新增/替换**：
```
js_config_set_int / js_config_set_string / js_config_set_bool / js_config_set_json / js_config_set_tlv
js_config_get
js_config_get_tlv_path / js_config_get_tlv_json
js_config_release / js_config_release_prefix
js_config_set_max_pools
js_config_core_set_tlv
```

**JS wrapper**（通过 `JS_Eval` 注入，mquickjs ES5 语法）：
- 扁平方法名：`appSetInt / appSetString / appSetBool / appSetJson / appSetTlv / appGet / appGetTlv / appRelease / appReleasePrefix / coreSetTlv / setMaxPools`
- App 域内部自动加 `"a."` 前缀，Core 域自动加 `"c."` 前缀
- 使用 `var` 声明，无箭头函数/展开参数

**删除**：旧的 `setApp / setUser / setCore / getApp / getUser / getCore / releaseApp / releaseUser` 相关 C handler 和 JS wrapper

---

### Phase 4: 运行时验证（基础 API + 窗口正常显示）

**目标**：确保重命名 + TLV + 新 API 后程序行为正常，窗口正常显示，无 crash

**测试点**：
- `make run` 窗口正常显示
- `kwcc.log` 无错误日志

---

### Phase 5: 集成验证

**目标**：完整功能端到端测试

**测试点**：
- `$config.appSetInt("io/port", 8080)` → 常量表匹配或正常存储
- `$config.appSetString("name", "myapp")` → 正常存储
- `$config.appSetBool("enabled", true)` → 常量表引用
- `$config.appSetTlv("io", { timeout: { user: "v1" } })` → TLV 存储
- `$config.appGetTlv("io", "timeout/user")` → 返回 `"v1"`
- `$config.appGetTlv("io")` → 返回 `{"timeout":{"user":"v1"}}` JSON 字符串
- `$config.appGet("name", "default")` → 返回 `"myapp"`
- `$config.appRelease("io/port")` → 释放 `a.io/port`
- `$config.appReleasePrefix("io")` → 释放 `a.io/` 前缀所有
- `$config.coreSetTlv("io", { max_fds: "16" })` → 存 TLV 到 `c.io`
- C 模块 `kwcc_mempool_get("c.io")` → 拿 slot → 根据 type 调 `kwcc_mempool_tlv_get_path()` 解析 max_fds
- `$config.setMaxPools("l5", 4)` → 运行时调整
- 编译通过 + 窗口正常显示 + 无日志错误

---

### Phase 6: 清理 + 提交

- 删除所有旧 `kwcc_config_set_app` 等残留声明
- 确认 `kwcc.log` 无错误
- 提交 git
