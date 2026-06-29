# kwcc_js 架构调整方案：Facade + Plugin 模式

> 创建于 2026-06-28
> 前置依赖：无（纯架构重构，不依赖 HTTP 实施进度）
> 被依赖：`requirements/js-http-implementation-plan.md`（HTTP 模块落地需要本方案完成）

---

## Context

当前 `kwcc_js.c/h` 直接暴露 mquickjs 类型（`JSContext`、`JSValue`、`JSCStringBuf` 等）给扩展模块。每个扩展模块（如 `kwcc_js_http`）都必须 `#include "mquickjs/mquickjs.h"` 并直接调用 mquickjs API，导致：

1. 扩展模块与 mquickjs 强耦合，换引擎需要改所有模块
2. 每个模块重复处理 mquickjs 的坑（include 顺序、`JSCStringBuf`、`JS_StackCheck` + `JS_PushArg` + `JS_Call` 三步调用）
3. 注册机制散乱，每个模块暴露自己的 `kwcc_register_xxx_js()` 让外部调

参考 Linux 内核 core + module 的关系：core 封装底层引擎细节，module 遵循规范注册进 core，通过 core 提供的接口操作 JS，不直接碰底层引擎。

---

## 架构设计

### 层次关系

```
mquickjs.h           ← 第三方 JS 引擎
    ↑ (只有 kwcc_js.c 知道)
kwcc_js.h            ← Facade：封装 mquickjs，定义类型/接口/注册规范
    ↑ (扩展模块通过这一层操作 JS，可 include 其他所需库)
kwcc_js_http.c/h     ← Plugin：实现 kwcc_js_module_t 接口，依赖 kwcc_js.h + kwcc_http.h
kwcc_js_ws.c/h       ← Plugin：未来，依赖 kwcc_js.h + kwcc_ws.h
```

### 隔离原则

子模块**只隔离 mquickjs**，不隔离其他第三方库或项目内部库。具体规则：

- **禁止 include**：`mquickjs.h`、`mquickjs_priv.h` — JS 引擎是唯一需要隔离的依赖，子模块通过 `kwcc_js_ops_t` 操作 JS
- **可 include**：项目内部库（`llog.h`、`kwcc_http.h` 等）和第三方库（`picohttpparser.h` 等）— 子模块可以 include 它需要的任何非 mquickjs 依赖

### 核心类型

#### 1. kwcc_js_val_t — 不透明 JS 值句柄

```c
typedef uint64_t kwcc_js_val_t;
```

子模块不直接操作 `JSValue`，只通过 `kwcc_js_ops_t` 的函数指针操作。

常量由 ops 属性提供，子模块不碰 `JS_NULL`/`JS_UNDEFINED` 等宏。

#### 2. kwcc_js_cstr_buf_t — C 字符串转换缓冲区

```c
typedef struct { char buf[5]; } kwcc_js_cstr_buf_t;
```

子模块不直接操作 `JSCStringBuf`，使用 `kwcc_js_cstr_buf_t` 替代。调用方在栈上声明，传给 `to_cstring`，生命周期由调用方控制——和原始 `JS_ToCString` 的使用模式一致。`buf[5]` 对应 mquickjs 的短字符串阈值：单个 Unicode code point 最多 4 字节 UTF-8 + 1 字节 `\0`。

**不需要 free**：`to_cstring` 返回的指针要么指向调用方的 buf（短字符串），要么指向 JS 内部内存（长字符串），两种情况都不需要手动释放，用完即止。但长字符串指针由 GC 管理，不能跨作用域长期保存——用完即止，不要缓存。

#### 3. kwcc_js_ops_t — JS 操作接口（属性 + 函数指针）

```c
typedef struct kwcc_js_ops {
    /* ── 属性 ── */
    void           *ctx;          /* 不透明 JSContext，子模块不碰 */
    kwcc_js_val_t  undefined;
    kwcc_js_val_t  null;
    kwcc_js_val_t  exception;
    kwcc_js_val_t  global_obj;

    /* ── 值创建 ── */
    kwcc_js_val_t  (*new_object)(kwcc_js_ops_t *ops);
    kwcc_js_val_t  (*new_int32)(kwcc_js_ops_t *ops, int32_t val);
    kwcc_js_val_t  (*new_string)(kwcc_js_ops_t *ops, const char *buf);
    kwcc_js_val_t  (*new_string_len)(kwcc_js_ops_t *ops, const char *buf, size_t len);

    /* ── 属性操作 ── */
    void           (*set_str_prop)(kwcc_js_ops_t *ops, kwcc_js_val_t obj,
                                   const char *key, kwcc_js_val_t val);
    kwcc_js_val_t  (*get_str_prop)(kwcc_js_ops_t *ops, kwcc_js_val_t obj,
                                   const char *key);

    /* ── 函数调用 ── */
    int            (*is_function)(kwcc_js_ops_t *ops, kwcc_js_val_t val);
    void           (*call_cb)(kwcc_js_ops_t *ops, kwcc_js_val_t cb,
                              int argc, kwcc_js_val_t *argv);

    /* ── C 字符串转换（两步调用，和原始 JS_ToCString 模式一致）── */
    const char *   (*to_cstring)(kwcc_js_ops_t *ops, kwcc_js_val_t val,
                                  kwcc_js_cstr_buf_t *buf);

    /* ── 类型判断 ── */
    int            (*is_undefined)(kwcc_js_val_t val);
    int            (*is_null)(kwcc_js_val_t val);
    int            (*is_exception)(kwcc_js_val_t val);

    /* ── 代码执行 ── */
    kwcc_js_val_t  (*eval)(kwcc_js_ops_t *ops, const char *code, size_t len,
                           const char *filename, int flags);

    /* ── 数组操作 ── */
    int            (*get_class_id)(kwcc_js_ops_t *ops, kwcc_js_val_t val);
    int            (*array_length)(kwcc_js_ops_t *ops, kwcc_js_val_t arr);
    kwcc_js_val_t  (*array_get)(kwcc_js_ops_t *ops, kwcc_js_val_t arr, uint32_t idx);

    /* ── 数字转换 ── */
    int            (*to_int32)(kwcc_js_ops_t *ops, kwcc_js_val_t val);

    /* ── C→JS 通知（$notify 通道）── */
    void           (*notify_js)(kwcc_js_ops_t *ops,
                                const char *type, const char *event,
                                const char *id, kwcc_js_val_t data,
                                void (*ack_cleanup)(const char *id));
} kwcc_js_ops_t;
```

**设计要点**：
- 函数指针第一个参数都是 `ops` 自身，子模块不需要知道 `ctx`
- `to_cstring` 保持两步调用模式（调用方提供 buf），和原始 `JS_ToCString` 一致，不跨作用域
- `is_function` 需要 `ops` 参数（mquickjs 的 `JS_IsFunction` 需要 ctx 检查对象类型），而 `is_undefined`/`is_null`/`is_exception` 不需要 `ops`（纯值比较宏，直接检查 uint64_t 位模式）
- `call_cb` 内部处理 `JS_StackCheck` + `JS_PushArg` + `JS_Call` 三步，子模块只传参数数组。`JS_Call` 返回后检查 `JS_IsException(ret)`：如果是异常，log_warn 记录并清除（`JS_GetException`），防止异常状态累积影响后续 JS 执行。子模块不需要处理异常
- 代理只做映射，不承担引擎内部的内存管理职责（GC、引用计数等由引擎自己管理）
- `notify_js` 的 `ack_cleanup` 参数：投递前自动调 `ack_cleanup(id)` 释放 C 侧资源（buffer、句柄等）。传入 NULL 表示无需清理（如 progress/Timer 事件）。调用方不需要记着调 release，传了就自动处理。`ack_cleanup` 内部失败时（如 req_id 已被清理）应 log_warn 并安全返回，不影响后续 `call_cb` 执行
- `s_notify_emit_fn`（`$notify.emit` 的引用）缓存为 static 变量。`$notify.emit` 通过 `global.$notify.emit` 属性链永远可达，GC 不会回收，不需要 `JS_AddGCRef` 保护

#### 4. kwcc_js_module_t — 模块描述符（规范）

```c
typedef struct kwcc_js_module {
    const char *name;       /* 模块名，用于日志和代理表注册 */
    void (*load)(kwcc_js_ops_t *ops);         /* 初始化：注入 JS 对象壳 + $notify.on(type, handler) */
    void (*register_cfun)(kwcc_js_ops_t *ops); /* 注册代理表 C handler */
    void (*on_bus_event)(const char *topic, const void *data,
                         size_t len, kwcc_js_ops_t *ops); /* bus 事件路由 */
    void (*unload)(kwcc_js_ops_t *ops);        /* 退出清理：释放长连接/定时器等资源，可为 NULL */
} kwcc_js_module_t;
```

**模块生命周期**：core 统一按 `load → register_cfun → on_bus_event（运行时）→ unload（退出时）` 顺序调用。`unload` 可为 NULL，WS/Timer 等管理长连接/定时器的模块必须实现。

### C→JS 通知模式：`$notify`

子模块收到 bus 事件后，不直接调 resolve/reject 等 JS 回调，而是通过 `$notify` 通用通知通道把事件信息传给 JS 端。JS 端通过 `$notify.on(type, handler)` 注册处理器，自己做 id → callback 的映射和路由。

**`$notify` vs `$bus` 职责划分**：

| 通道 | 方向 | 场景 |
|------|------|------|
| `$notify` | C → JS | 原生事件（HTTP 响应、WS 消息、Timer 触发） |
| `$bus` | JS → JS | 业务事件（按钮点击、state 变更） |

**C 端职责**：publish bus 事件 + 调 `ops->notify_js` 通知 JS 端
**JS 端职责**：通过 `$notify.on(type, handler)` 注册处理器，维护 callback 映射 + 调 resolve/reject

这样做的好处：
- C 端不存 JSValue 回调，不需要维护回调注册表
- C 端只管"发生了什么"，不管"谁在等"
- 回调映射是纯 JS 逻辑，写在模块注册的 handler 里，不跨语言边界
- 新模块只需加一行 `$notify.on(type, handler)` 注册，不改 core 代码

#### JS 端

```javascript
// $notify — C→JS 通知分发器（由 core 通过 C API 创建对象 + eval 注入方法）
$notify.registry = {};

$notify.on = function(type, handler) {
    $notify.registry[type] = handler;
};

// C 端唯一入口
$notify.emit = function(type, event, id, data) {
    var handler = $notify.registry[type];
    if (handler) handler(event, id, data);
};
```

#### C 端 API

`kwcc_js_ops_t` 提供 `notify_js` 函数指针，模块只调这个：

```c
void (*notify_js)(kwcc_js_ops_t *ops,
                  const char *type,      /* "http" | "ws" | "timer" */
                  const char *event,     /* "end" | "error" | "progress" | ... */
                  const char *id,        /* 通用标识符（reqId / connId / timerId） */
                  kwcc_js_val_t data,    /* 模块专属数据对象 */
                  void (*ack_cleanup)(const char *id));  /* 投递前释放 C 侧资源，可为 NULL */
```

core 实现内部持有 `$notify.emit` 的引用，`ack_cleanup` 在 `call_cb` 之前自动调用。`call_cb` 内部已做异常捕获（`JS_IsException` 检查 + log_warn + 清除），JS handler 抛异常不会中断 C 端执行或累积异常状态。模块不需要知道 `$notify` 的存在，也不需要记着调 release — 传了 `ack_cleanup` 就自动处理。

#### 数据流

```
C 模块 (http/ws/timer)
    │ ops->notify_js(ops, "http", "end", reqId, dataObj, ack_cleanup_fn)
    ▼
kwcc_js_ops.notify_js (core 实现)
    │ ① ack_cleanup(id) — 释放 C 侧资源（数据已在 GC 堆）
    │ ② call_cb($notify.emit, [type, event, id, data])
    ▼
$notify.emit(type, event, id, data)
    │ 查 registry[type]
    │ 调 handler(event, id, data)
    ▼
模块 handler（匿名函数，如 $notify.on('http', function(...)）
    │ 查 callbacks[id]
    │ 调 resolve/reject
    ▼
MiniPromise resolved/rejected
```

#### payload 设计

| 参数 | 说明 | 示例 |
|------|------|------|
| type | 模块类型 | "http", "ws", "timer" |
| event | 事件类型 | "end", "error", "progress" |
| id | 通用标识符 | HTTP reqId / WS connId / Timer timerId |
| data | 模块专属数据（JSValue 对象） | HTTP 响应对象 / WS 消息对象 |

**不设 source 字段**：`$notify` 本身就是 C→JS 通道，来源恒为 C。JS→JS 事件走 `$bus`。

**`$notify` 不经过 `bus/js_whitelist`**：`$notify` 是 C→JS 的直达通道，由 C 端 `ops->notify_js` 触发，信任边界是模块代码本身。如果未来有不可信的数据源需要过滤，应在模块 handler 内部做，而不是在 `$notify` 层加白名单——这里的设计意图是原生事件的零开销直达，不应该增加过滤层。

详见 `requirements/js-http-implementation-plan.md` 中 HTTP 模块使用 `$notify` 的设计。

---

## kwcc_js.c 核心改动

### 1. ops 实例初始化

```c
static kwcc_js_ops_t g_kwcc_js_ops;

static void kwcc_js_ops_init(JSContext *ctx) {
    g_kwcc_js_ops.ctx = ctx;
    g_kwcc_js_ops.undefined = JS_UNDEFINED;
    g_kwcc_js_ops.null = JS_NULL;
    g_kwcc_js_ops.exception = JS_EXCEPTION;
    g_kwcc_js_ops.global_obj = JS_GetGlobalObject(ctx);

    g_kwcc_js_ops.new_object     = js_new_object_impl;
    g_kwcc_js_ops.new_int32      = js_new_int32_impl;
    g_kwcc_js_ops.new_string     = js_new_string_impl;
    g_kwcc_js_ops.new_string_len = js_new_string_len_impl;
    g_kwcc_js_ops.set_str_prop   = js_set_str_prop_impl;
    g_kwcc_js_ops.get_str_prop   = js_get_str_prop_impl;
    g_kwcc_js_ops.is_function    = js_is_function_impl;
    g_kwcc_js_ops.call_cb        = js_call_cb_impl;
    g_kwcc_js_ops.to_cstring     = js_to_cstring_impl;
    g_kwcc_js_ops.is_undefined   = js_is_undefined_impl;
    g_kwcc_js_ops.is_null        = js_is_null_impl;
    g_kwcc_js_ops.is_exception   = js_is_exception_impl;
    g_kwcc_js_ops.eval           = js_eval_impl;
    g_kwcc_js_ops.get_class_id   = js_get_class_id_impl;
    g_kwcc_js_ops.array_length   = js_array_length_impl;
    g_kwcc_js_ops.array_get      = js_array_get_impl;
    g_kwcc_js_ops.to_int32       = js_to_int32_impl;
    g_kwcc_js_ops.notify_js     = js_notify_js_impl;
}
```

### 2. 注入 `$notify` + 统一模块注册

```c
static kwcc_js_val_t s_notify_emit_fn;  /* $notify.emit 的引用（global 可达，GC 安全） */

/* 注入 $notify 通用通知分发器（在模块注册之前） */
static void kwcc_js_inject_notify(kwcc_js_ops_t *ops) {
    /* 创建 $notify 对象（C API，和 http_load 中 $http 创建方式一致） */
    kwcc_js_val_t notify_obj = ops->new_object(ops);
    ops->set_str_prop(ops, ops->global_obj, "$notify", notify_obj);

    const char *code =
        "$notify.registry = {};\n"
        "$notify.on = function(type, handler) {\n"
        "    $notify.registry[type] = handler;\n"
        "};\n"
        "$notify.emit = function(type, event, id, data) {\n"
        "    var handler = $notify.registry[type];\n"
        "    if (handler) handler(event, id, data);\n"
        "};\n";
    ops->eval(ops, code, strlen(code), "<$notify>", JS_EVAL_REPL);

    /* 缓存 $notify.emit 的引用，供 notify_js 实现调用 */
    s_notify_emit_fn = ops->get_str_prop(ops, notify_obj, "emit");
}

/* notify_js 实现 — 模块通过 ops->notify_js 调用 */
static void js_notify_js_impl(kwcc_js_ops_t *ops,
                               const char *type, const char *event,
                               const char *id, kwcc_js_val_t data,
                               void (*ack_cleanup)(const char *id)) {
    /* ① 先释放 C 侧资源（数据已通过 JSValue 拷贝进 GC 堆） */
    if (ack_cleanup) ack_cleanup(id);

    /* ② 投递到 JS 端 */
    kwcc_js_val_t args[4];
    args[0] = ops->new_string(ops, type);
    args[1] = ops->new_string(ops, event);
    args[2] = ops->new_string(ops, id);
    args[3] = data;
    ops->call_cb(ops, s_notify_emit_fn, 4, args);
}

void kwcc_js_register_modules(kwcc_js_ops_t *ops) {
    /* 先注入 $notify，模块 load 时才能 $notify.on(type, handler) */
    kwcc_js_inject_notify(ops);

    extern kwcc_js_module_t kwcc_js_http_module;
    kwcc_js_register_module(ops, &kwcc_js_http_module);
    /* 未来加模块只加一行 */
}

void kwcc_js_register_module(kwcc_js_ops_t *ops, kwcc_js_module_t *mod) {
    log_info("js: loading module '%s'", mod->name);
    if (mod->load) mod->load(ops);
    if (mod->register_cfun) mod->register_cfun(ops);
    g_kwcc_js_modules[g_kwcc_js_module_count++] = mod;   /* 维护模块列表，供 bus 事件分发 */
    log_info("js: module '%s' registered", mod->name);
}
```

### 3. bus 事件按前缀分发

```c
#define KWCC_JS_MAX_MODULES 8
static kwcc_js_module_t *g_kwcc_js_modules[KWCC_JS_MAX_MODULES];
static int g_kwcc_js_module_count = 0;

static void kwcc_js_on_bus_event(const char *topic, const void *data,
                                  size_t len, void *user_data) {
    /* 按 topic 前缀分发给对应模块的 on_bus_event */
    for (int i = 0; i < g_kwcc_js_module_count; i++) {
        if (g_kwcc_js_modules[i]->on_bus_event) {
            g_kwcc_js_modules[i]->on_bus_event(topic, data, len, &g_kwcc_js_ops);
        }
    }
    /* 无模块认领的 topic，走默认白名单 + $bus.emit */
    /* ... */
}
```

**注意**：模块的 `on_bus_event` 内部自行判断 topic 前缀（如 `http/`），core 不做前缀匹配分发——这样更灵活，模块可以订阅任意 topic。当前实现是每个事件遍历所有模块（O(n)），模块数量少时开销可忽略。如果未来模块数增长到十几个，可考虑让模块在描述符中声明 `topic_prefix` 数组，core 按前缀做 O(1) 路由表分发——当前阶段不提前优化。

---

## 实施步骤

| Step | 内容 | 新增/修改文件 | 依赖 |
|------|------|--------------|------|
| 1 | kwcc_js.h 新增类型和接口定义 | `src/kwcc_js.h` | 无 |
| 2 | kwcc_js.c 实现 ops 绑定 + 模块注册 | `src/kwcc_js.c` | Step 1 |
| 3 | ops 接口测试 | `tests/test_js_ops.c` | Step 2 |
| 4 | 重构现有 HTTP 代码为模块描述符 | `src/kwcc_js.c`（删除内联 HTTP 代码） | Step 3 |

### Step 1: kwcc_js.h 类型定义

在 `kwcc_js.h` 中新增：
- `kwcc_js_val_t` 类型
- `kwcc_js_cstr_buf_t` 类型
- `kwcc_js_ops_t` 结构体
- `kwcc_js_module_t` 结构体
- `kwcc_js_register_module()` / `kwcc_js_register_modules()` 声明

保留现有声明（`kwcc_create_js`、`kwcc_destroy_js`、config handler 等）不变。

**验证**：`make` 编译通过

### Step 2: kwcc_js.c ops 实现

1. 实现所有 `js_xxx_impl` 函数（包括 `js_notify_js_impl`）
2. `kwcc_create_js()` 中调用 `kwcc_js_ops_init(ctx)` + `kwcc_js_register_modules(&g_kwcc_js_ops)`
3. `kwcc_js_register_modules` 先注入 `$notify`，再逐个注册模块（模块 `load` 中可 `$notify.on(type, handler)`）
4. `kwcc_js_on_bus_event` 改为遍历模块列表分发

**验证**：`make` 编译通过

### Step 3: ops 接口测试

**目的**：把 `kwcc_js_ops_t` 的每个函数指针当成内部 ABI 契约做单元测试，在重构 HTTP 代码之前确保 core 本身正确。测试跑通后再做模块迁移，避免问题交叉。

### `tests/test_js_ops.c`

| # | 测试 | 验证 |
|---|------|------|
| 1 | `new_object` + `set_str_prop` + `get_str_prop` | 创建对象、设属性、读属性闭环 |
| 2 | `new_int32` + `to_int32` | 数字往返正确 |
| 3 | `new_string` + `to_cstring` | 短字符串内联（≤5字节）和长字符串分支都正确，不需要 free |
| 4 | `call_cb` | 调 JS 函数、传参数、取返回值 |
| 5 | `is_function` / `is_undefined` / `is_null` / `is_exception` | 类型判断正确 |
| 6 | `eval` + `JS_EVAL_REPL` | 执行 JS 代码，隐式全局变量生效 |
| 7 | `notify_js` + `ack_cleanup` | C→JS `$notify.emit` 通道工作，`$notify.on` handler 被调用，`ack_cleanup` 在投递前被自动调用 |
| 8 | `array_length` + `array_get` | 数组创建、长度、索引访问 |

测试方法参考 `testing_methodology.md`：独立 mquickjs 环境，不依赖 Sokol/microui/NanoVG。

---

### Step 4: 重构 HTTP 为模块

1. 删除 `kwcc_js.c` 中的 HTTP 相关代码（`kwcc_js_http_request`、`kwcc_js_http_cancel`、`kwcc_register_http_js`、回调注册表、bus 事件路由）——这些由 `kwcc_js_http.c` 模块提供
2. 在 `kwcc_js.c` 中只保留 `extern kwcc_js_module_t kwcc_js_http_module;` 和注册调用
3. `match_whitelist` 改名 `kwcc_js_match_whitelist`
4. 删除 `kwcc_js.c` 中的 `#include "kwcc_http.h"` 和 `#include "picohttpparser/picohttpparser.h"` — core 不依赖具体模块的服务层，编译隔离

**注意**：此步不创建 `kwcc_js_http.c/h`，那是 `js-http-implementation-plan.md` 的内容。此步只确保 `kwcc_js.c` 中的 HTTP 代码通过 ops 接口调用，不再直接用 mquickjs API。

**验证**：`make` 编译通过，`make run` 正常，ops 测试仍通过

---

## 风险点

| 风险 | 影响 | 缓解 |
|------|------|------|
| `call_cb` argv 数组传递 | 子模块需要构造 kwcc_js_val_t 数组 | 栈上小数组即可，最多 7 个参数 |
| `kwcc_js_val_t` 和 `JSValue` 实际相同 | 类型安全是编译期的 | `typedef` 提供文档性隔离 |
| bus 事件分发顺序 | 多模块可能对同一 topic 感兴趣 | 模块内部自行判断前缀，互不干扰 |
| 代理表注册时机 | `register_cfun` 需在 `kwcc_create_js` 期间完成 | `kwcc_js_register_modules` 在 ctx 创建后立即调用 |
