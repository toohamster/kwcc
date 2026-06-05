# 三段式 Slab 内存池设计文档（v6 — 按池类型分层 + TLV 序列化 + 多池管理）

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
kwcc_pool_type_t:
  KWCC_POOL_L0  — 8B      基础类型 (int/float/bool)
  KWCC_POOL_L1  — 32B     极短字符串 (keys, IDs, 开关)
  KWCC_POOL_L2  — 128B    短字符串 (配置值, URL, 标签)
  KWCC_POOL_L3  — 512B    中字符串 (错误消息, JSON片段)
  KWCC_POOL_L4  — 1KB     较长字符串 (短JSON响应, 配置对象)
  KWCC_POOL_L5  — 4KB     长字符串 (HTTP body, 日志)
  KWCC_POOL_L6  — 16KB    大字符串 (SVG, 大文本)
  KWCC_POOL_L7  — 动态    malloc 无上限 (非 slab, 固定 128 元信息槽)
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
| L0 | 8B | 512 | 4KB | 41KB | ~45KB | int/float/bool |
| L1 | 32B | 256 | 8KB | 20KB | ~28KB | keys, IDs, 开关 |
| L2 | 128B | 256 | 32KB | 20KB | ~52KB | 配置值, URL |
| L3 | 512B | 256 | 128KB | 20KB | ~148KB | 错误消息, JSON片段 |
| L4 | 1KB | 128 | 128KB | 10KB | ~138KB | 短JSON响应, 配置对象 |
| L5 | 4KB | 16 | 64KB | 1.3KB | ~65KB | HTTP body, 日志 |
| L6 | 16KB | 8 | 128KB | 0.6KB | ~129KB | SVG, 大文本 |
| L7 | 动态 | 128 | — | 10KB | ~10KB | 非 slab |

每种池独立扩缩容，max_pools 可分别配置：
```c
max_pools[KWCC_POOL_MAX_TYPES] = { 16, 8, 8, 4, 4, 4, 2, 2 };
```

L0 最多 16 池（8192 个 key），L3 最多 4 池，L6/L7 最多 2 池。

**初始化（1 池/类型，8 种）**：~550KB
**满载（max_pools 全部展开）**：~3.0MB

## 值编码方式

mempool 层是纯 KV 存储（字节数组），不关心值的内容。编码由 `$config` 层决定：

**方式一：直接字符串（默认）**
```js
$config.setApp("name", "myapp");
$config.setApp("count", "42");
```
→ 直接存字符串，零编码开销。

**方式二：JSON 字符串**
```js
$config.setApp("io", JSON.stringify({ max_fds: "16", port: "8080" }));
```
→ 存 JSON 字符串，JS 侧 `JSON.parse()` 解。mquickjs 内置 JSON，零实现成本。

**方式三：TLV 二进制（可选，用于对象/层级数据）**
```js
$config.setApp("io", { max_fds: "16", port: "8080" });
```
→ `$config` 内部调用 `js_tlv_pack()` 将 JSValue 对象编码为 TLV 二进制存 slot。
→ 读取时 C 侧 TLV 路径查询，返回字符串/JSValue。
→ **比 JSON 省 30% 空间**，适合大量层级配置。

**总结：mempool 不管编码，只存字节。TLV 只是 `$config` 层的一个编码选项。**

### TLV 条目格式
```
Type(1B) + TotalLen(2B) + Name(str) + Value(data)
```

### 类型定义
```c
KWCC_TLV_FIELD  = 0x01   // 基础类型（string/int/bool）
KWCC_TLV_OBJECT = 0x02   // 子对象（Value 是子 TLV 块）
KWCC_TLV_ARRAY  = 0x03   // 数组（子元素用索引名 "0","1"...）
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

### TLV API（kwcc_js.c，使用 mquickjs_priv.h 内部 API）
```c
JSValue js_tlv_pack(JSContext *ctx, JSValue obj);
  // JSValue 对象 → TLV 字符串
  // 使用 js_object_keys + JS_GetPropertyStr 递归遍历

JSValue js_tlv_unpack(JSContext *ctx, JSValue tlv_str);
  // TLV 字符串 → JSValue 对象

JSValue js_tlv_get_path(JSContext *ctx, const char *tlv_data,
                         size_t tlv_len, const char *path);
  // 路径查询语法糖：tlv_get_path(tlv, "io/timeout/user") → "v1"
  // 拆路径 → 逐层查 TLV → 返回值，只查 1 次 hashmap
```

### JSValue ↔ TLV 转换流程
```
set: JS对象 → C侧 js_tlv_pack() → TLV二进制 → 存slot
get: 读slot → TLV二进制 → C侧直接返回字符串/JSValue

C 侧读 TLV：
  tlv_get_path(slot_data, "timeout/user") → 直接拿值
  返回字符串给 JS → JS JSON.parse()（对象模式）

$config.setApp("io", { timeout: { user: "v1" } })
  → C 侧 js_tlv_pack() → 打包 TLV → 存 slot

$config.getApp("io")
  → C 侧读 slot → TLV 块 → 返回字符串给 JS
  → JS JSON.parse() 得到对象
```

## 数据类型（slot->type，uint8_t）

```c
enum {
    KWCC_TYPE_STRING = 0,  // 纯字符串
    KWCC_TYPE_INT32  = 1,  // 32位整数
    KWCC_TYPE_INT64  = 2,  // 64位整数
    KWCC_TYPE_FLOAT  = 3,  // 浮点
    KWCC_TYPE_DOUBLE = 4,  // 双精度
    KWCC_TYPE_JSON   = 5,  // JSON字符串（读取时自动decode）
    KWCC_TYPE_TLV    = 6,  // TLV二进制块（读取时自动解码）
    KWCC_TYPE_CONST  = 7,  // 常量引用（指向 g_const_table）
};
```

C 侧 `memcpy` 直接存原生格式，JS 侧走字符串（自动转）。

## 常量表（16 个高频值）

```c
// 字符串常量
KWCC_CONST_NULL     = 0,    // "null"
KWCC_CONST_EMPTY    = 1,    // ""
KWCC_CONST_ZERO     = 2,    // "0"
KWCC_CONST_ONE      = 3,    // "1"
KWCC_CONST_TRUE     = 4,    // "true"
KWCC_CONST_FALSE    = 5,    // "false"

// 布尔常量
KWCC_CONST_TRUE_BOOL  = 6,  // true (BOOL)
KWCC_CONST_FALSE_BOOL = 7,  // false (BOOL)

// 整数常量
KWCC_CONST_M1       = 8,    // -1 (INT32)
KWCC_CONST_0_INT    = 9,    // 0 (INT32)
KWCC_CONST_1_INT    = 10,   // 1 (INT32)

// 11-15 保留
```

```c
typedef struct {
    uint8_t  value[8];      // 常量数据
    uint8_t  real_type;     // 真实类型（BOOL/INT32/STRING）
    uint8_t  size;          // 数据大小
} kwcc_const_t;

kwcc_const_t g_const_table[16];  // ~1.3KB
```

slot->type = CONST 时，data 指针指向常量区，不消耗 slab chunk。

## Slot 结构体

```c
typedef struct {
    uint32_t  hash;           /* FNV-1a hash */
    char      key_buf[32];    /* 短 key 内联 */
    const char *key;          /* → key_buf 或外部 malloc（长 key） */
    uint8_t  *data;           /* → slab chunk 或 g_const_table */
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
} kwcc_slot_t;
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
  kwcc_mempool_alloc(type, key, size, timeout)         // 按类型+长度路由 L0-L6
  kwcc_mempool_alloc_dynamic(key, cap, timeout)         // 直走 L7（动态 malloc）
  kwcc_mempool_get(pool_id, key)                        // 返回 slot*
  kwcc_mempool_set(pool_id, slot, data, size)           // 写数据
  kwcc_mempool_release(pool_id, slot)                   // ref--
  kwcc_mempool_get_keys(pool_id, prefix)                // 前缀扫描
  kwcc_mempool_get_str(pool_id, key, default_value)     // 返回 null-terminated 字符串

JS 层：$config（唯一公开的配置工具）
  setApp/setUser/setCore(key, val, timeout, capacity)
    - capacity=0 → 按 value 长度自动路由 L0-L6
    - capacity > 16KB → 直走 L7
    - 对象参数 → TLV 编码（$config.setApp("io", obj)）或 JSON 字符串
    - null → 释放（单 key + 前缀）
  getApp/getUser/getCore(key, default)
    - 单 key 返回字符串
    - 前缀返回 {subkey: value}
    - JS JSON.parse() 解 JSON 字符串（对象模式）
    - 或 $config 自动 TLV 解码返回 JSValue
  setMaxPools(spec)
    - 字符串模式：setMaxPools("l5", 4)
    - 全局模式：setMaxPools("*", 4)
```

## $config 完整 API

```javascript
// App
$config.setApp("io", { max_fds: "16" });             // 对象 → TLV 编码
$config.setApp("io/max_fds", "16");                  // 单 key → 字符串存
$config.setApp("io", JSON.stringify(obj));            // 手动 JSON → 字符串存
$config.setApp("io", null);                          // 释放 "io/*" 前缀
$config.getApp("io/max_fds", "16");                  // 读 + 默认值
$config.getApp("io");                                // 前缀 → { max_fds: "16" }

// User（额外支持 timeout + capacity）
$config.setUser("http/body", body, 30);              // 存 + 30s 超时
$config.setUser("http/body", body, 30, 102400);      // 预估 100KB，直走 L7
$config.setUser("module", { data: "..." }, 60);      // 对象展开 + 超时
$config.setUser("module", null);                     // 释放前缀
$config.getUser("module/x", "default");              // 读 + 默认值
$config.getUser("module");                           // 前缀模式

// Core（同 App，无 timeout）
$config.setCore(key, val) / $config.setCore(module, {}) / $config.setCore(key, null)
$config.getCore(key, default) / $config.getCore(prefix)

// 池管理
$config.setMaxPools("l5", 4);                        // L5 最大 4 池
$config.setMaxPools("*", 4);                         // 全部设为 4
```

## 多池管理

- **max_pools 默认 = { 16, 8, 8, 4, 4, 4, 2, 2 }**，每种池类型独立配置
- `$config.setMaxPools()` 可指定每种类型的上限，n < 4 自动修正为 4
- **池扩展策略**：每种类型初始化开 1 池，满即扩，直到该类型的 max_pools
- **L7 动态数据**：固定 128 元信息槽，不占 slab 空间，通过独立 malloc 分配
- **key_map**：key → pool_id 映射（扁平 hash 表，32768 条目，O(1) 查找）
  - 前缀扫描：遍历匹配 "prefix/" ≈ 3μs（50 个实际条目）
  - 单 key 查找：hash 一次，O(1) ≈ 100ns
  - key_map 条目：{ hash, key指针, pool_type, pool_idx, slot_idx }

## key_map 设计

```c
#define KWCC_KEY_MAP_SIZE 32768  /* 2^15，满载 14160 条目，负载率 43% */

typedef struct {
    uint32_t hash;          /* FNV-1a hash */
    const char *key;        /* 指向 slot 的 key_buf */
    uint8_t  pool_type;     /* L0-L7 池类型 */
    uint8_t  pool_idx;      /* 该类型第几个池 (0-based) */
    uint16_t slot_idx;      /* 该池第几个槽 */
} kwcc_key_map_entry_t;
```

- alloc 时写入 key_map
- free 时标记为无效
- get 时 hash 查表直接定位
- prefix 时遍历匹配

## 关键源文件

| 文件 | 作用 | 本次改动 |
|------|------|---------|
| `src/kwcc_mempool.h` | 新建 | 类型定义 + API 声明 |
| `src/kwcc_mempool.c` | 新建 | 核心实现（多池管理 + key_map + TLV） |
| `src/kwcc.h` | umbrella header | 加 `#include "kwcc_mempool.h"` |
| `src/kwcc_js.c` | JS lifecycle | 添加 `$config` JS API + C handler + TLV pack/unpack |
| `src/kwcc_ui.c` | UI bridge | 在 `kwcc_register_ui` 中加 `$config` JS wrapper |
| `src/main.m` | Sokol lifecycle | `init()` 加 `kwcc_mempool_init()` |
| `src/kwcc_base.h` | base infra | 添加 `KWCC_DEBUG` 宏 + 编译默认宏 |
| `app/main.js` | JS entry | 加 `$config.setAppSize` 调用 |
| `Makefile` | build | 加 `kwcc_mempool.c` 编译规则 |
| `deps/mquickjs/mqjs_stdlib.c` | stdlib | 加 C 函数注册（CONFIG_KWCC） |
| `src/kwcc_io.c` | I/O reactor | 迁移到 `kwcc_mempool_get_str` |

## 清理步骤（旧 `__kwcc_config` 系统）

1. **`src/kwcc.c`** — 清空全部旧实现（已完成）
2. **`src/kwcc_base.h`** — 删除旧 config 声明（已完成）
3. **`src/kwcc_io.c`** — 用 `kwcc_mempool_get_str`（已完成）
4. **`src/kwcc_js.c`** — 删除 `kwcc_config_set_jsctx` + `js_kwcc_config_set`（已完成）
5. **`src/kwcc_js.h`** — 删除旧声明（已完成）
6. **`src/kwcc_ui.c`** — 删除 `kwcc_config` JS wrapper（已完成）
7. **`deps/mquickjs/mqjs_stdlib.c`** — 删除 `kwcc_config_set` 注册（已完成）

## 实施步骤（待执行）

### Step 1: 重命名现有文件
- `kwcc_pool.h` → `kwcc_mempool.h`
- `kwcc_pool.c` → `kwcc_mempool.c`
- 所有函数 `kwcc_pool_*` → `kwcc_mempool_*`
- 全局变量 `g_*_pool` → `g_*_mempool`

### Step 2: 重构内存池分层结构
- L0（8B）、L1（32B）、L2（128B）、L3（512B）、L4（1KB）、L5（4KB）、L6（16KB）、L7（动态 malloc）
- 新增数据类型标记（uint8_t，4bit 用：string/int32/int64/float/double/bool，CONST=6，7 保留）
- 新增常量表（g_const_table[16]），slot->type=CONST 时引用常量区
- 每种池类型单一 chunk 大小，无内部子 slab
- 新增 `kwcc_mempool_alloc_dynamic` 快速通道（L7 动态 malloc）

### Step 3: 多池管理
- 新增 `kwcc_mempool_manager`：每种池类型的池数组 + key_map（32768 条目）
- 初始化开 1 池/类型 + 满即扩策略
- 满即扩，最多 max_pools[type]
- `max_pools` 编译时默认 `{16, 8, 8, 4, 4, 4, 2, 2}`，运行时可配

### Step 4: 新增 TLV 序列化
- `kwcc_js.c` 实现 `js_tlv_pack()` / `js_tlv_unpack()`（使用 mquickjs_priv.h 内部 API）
- TLV 格式：Type(1B) + TotalLen(2B) + Name + Value
- 路径查询：`tlv_get_path(tlv, "io/timeout/user")` 语法糖
- JS ↔ TLV 转换：JS 对象 → TLV 存 slot，读 slot → 返回字符串/JSValue

### Step 5: 新增 `kwcc_mempool_get_keys` 和 `kwcc_mempool_get_str`
- 前缀扫描 key_map，返回 JS 数组
- get_str 返回 null-terminated 字符串 + 默认值

### Step 6: 重写 `$config` JS API
- C handler：`js_config_set_app`、`js_config_get_app`（App/User/Core 共 6 个）
- C handler：`js_config_set_app_size`、`js_config_set_max_pools`
- JS wrapper 在 `JS_Eval` 中注入 `$config`
- 处理：对象 → TLV 打包、null 释放、默认值、前缀读取

### Step 7-11: 更新相关文件 + 编译验证

## 关键设计决策

- **命名规范**：所有函数 `kwcc_mempool_{action}` 三段式
- **ref_count**：`uint16_t`，`alloc()` 时 = 0，`acquire()` 时++，溢出打 error 忽略
- **GC 80% 阈值**：自动检测，超过立即强制 GC
- **$config 是唯一公开 API**，`$memory` 不暴露
- **setApp/setUser(key, null)** 统一为释放
- **capacity 参数**：0=自动路由 L0-L6，>16KB 直走 L7
- **类型标记**：uint8_t，0-7（STRING/INT32/INT64/FLOAT/DOUBLE/JSON/TLV/CONST）
- **L0-L7 按池类型分层**：每种池独立 chunk 大小 + 独立槽数 + 独立扩缩容
- **单池规格**：L0 ~45KB/512槽 到 L6 ~129KB/8槽，L7 仅 128 元信息槽
- **总内存**：初始化 ~550KB（1 池/类型），满载 ~3.0MB
- **池扩展**：满即扩，直到 max_pools，不预留
- **TLV 值存储**：比 JSON 省 30% 空间，C 侧路径查询，JS 侧无感知
- **常量表**：16 个高频值（null/空串/true/false/0/1/-1），引用不占 slot chunk
- **key_map**：32768 条目 hash 表，单 key O(1)，前缀扫描 ~3μs
- **mquickjs ES5 限制**：JS wrapper 用 `var`，无展开参数
- **JS_ToCString NULL 保护**
- **TLV 实现位置**：kwcc_js.c，include mquickjs_priv.h，使用 js_object_keys 内部 API
- **生命周期**：L0-L7 统一初始化（~550KB），App/User/Core 只是业务命名空间

## 生命周期流程

```
init() — main.m
  │
  ├─ 1. kwcc_mempool_init()        → L0-L7 各开 1 池（~550KB）
  │                                    max_pools = 编译时默认值
  │
  ├─ 2. kwcc_create_js()           → JSContext 就绪，注入 $config
  │
  └─ 3. kwcc_register_ui(js_ctx)   → 注入 $config 全局 API
       │
       └─ 执行 main.js
            $config.setMaxPools("l5", 4);        // 运行时调整 max_pools
            $config.setMaxPools("*", 4);         // 全局调整
            loadJs 业务代码...

frame() — 每帧
  ├─ kwcc_process_js(js_ctx, "onFrame();")  → 渲染
  └─ kwcc_mempool_gc_auto()                 → GC 自动节流
```

## 验证

- 编译通过（`make clean && make`）
- 窗口正常显示（`make run`）
- `$config.setApp("io", { max_fds: "16" })` → TLV 存
- `$config.getApp("io/max_fds")` → 返回 `"16"`
- `$config.getApp("io")` → 返回 `{ max_fds: "16" }`
- `$config.setApp("io", null)` → 释放 `"io/"` 前缀所有
- 检查 `kwcc.log` 无错误日志
