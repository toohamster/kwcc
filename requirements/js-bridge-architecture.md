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
    ↑ (扩展模块只依赖这一层)
kwcc_js_http.c/h     ← Plugin：实现 kwcc_js_module_t 接口，依赖 kwcc_js.h + kwcc_http.h
kwcc_js_ws.c/h       ← Plugin：未来，依赖 kwcc_js.h + kwcc_ws.h
```

### 核心类型

#### 1. kwcc_js_val_t — 不透明 JS 值句柄

```c
typedef uint64_t kwcc_js_val_t;
```

子模块不直接操作 `JSValue`，只通过 `kwcc_js_ops_t` 的函数指针操作。

常量由 ops 属性提供，子模块不碰 `JS_NULL`/`JS_UNDEFINED` 等宏。

#### 2. kwcc_js_ops_t — JS 操作接口（属性 + 函数指针）

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

    /* ── C 字符串转换（配对使用，子模块不碰 JSCStringBuf）── */
    const char *   (*to_cstring)(kwcc_js_ops_t *ops, kwcc_js_val_t val);
    void           (*free_cstring)(kwcc_js_ops_t *ops, const char *s);

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
} kwcc_js_ops_t;
```

**设计要点**：
- 函数指针第一个参数都是 `ops` 自身，子模块不需要知道 `ctx`
- `to_cstring` / `free_cstring` 配对，隐藏 `JSCStringBuf` 细节
- `call_cb` 内部处理 `JS_StackCheck` + `JS_PushArg` + `JS_Call` 三步，子模块只传参数数组

#### 3. kwcc_js_module_t — 模块描述符（规范）

```c
typedef struct kwcc_js_module {
    const char *name;       /* 模块名，用于日志和代理表注册 */
    void (*load)(kwcc_js_ops_t *ops);         /* 初始化：注入 JS 对象壳 */
    void (*register_cfun)(kwcc_js_ops_t *ops); /* 注册代理表 C handler */
    void (*on_bus_event)(const char *topic, const void *data,
                         size_t len, kwcc_js_ops_t *ops); /* bus 事件路由 */
} kwcc_js_module_t;
```

**模块生命周期**：core 统一按 `load → register_cfun → on_bus_event` 顺序调用。

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
    g_kwcc_js_ops.free_cstring   = js_free_cstring_impl;
    g_kwcc_js_ops.is_undefined   = js_is_undefined_impl;
    g_kwcc_js_ops.is_null        = js_is_null_impl;
    g_kwcc_js_ops.is_exception   = js_is_exception_impl;
    g_kwcc_js_ops.eval           = js_eval_impl;
    g_kwcc_js_ops.get_class_id   = js_get_class_id_impl;
    g_kwcc_js_ops.array_length   = js_array_length_impl;
    g_kwcc_js_ops.array_get      = js_array_get_impl;
    g_kwcc_js_ops.to_int32       = js_to_int32_impl;
}
```

### 2. 统一模块注册

```c
void kwcc_js_register_modules(kwcc_js_ops_t *ops) {
    extern kwcc_js_module_t kwcc_js_http_module;
    kwcc_js_register_module(ops, &kwcc_js_http_module);
    /* 未来加模块只加一行 */
}

void kwcc_js_register_module(kwcc_js_ops_t *ops, kwcc_js_module_t *mod) {
    log_info("js: loading module '%s'", mod->name);
    if (mod->load) mod->load(ops);
    if (mod->register_cfun) mod->register_cfun(ops);
    log_info("js: module '%s' registered", mod->name);
}
```

### 3. bus 事件按前缀分发

```c
static void kwcc_js_on_bus_event(const char *topic, const void *data,
                                  size_t len, void *user_data) {
    /* 按 topic 前缀分发给对应模块的 on_bus_event */
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i]->on_bus_event) {
            g_modules[i]->on_bus_event(topic, data, len, &g_kwcc_js_ops);
        }
    }
    /* 无模块认领的 topic，走默认白名单 + $bus.emit */
    /* ... */
}
```

**注意**：模块的 `on_bus_event` 内部自行判断 topic 前缀（如 `http/`），core 不做前缀匹配分发——这样更灵活，模块可以订阅任意 topic。

---

## to_cstring / free_cstring 实现策略

`JSCStringBuf` 是栈上 5 字节缓冲区，mquickjs 内联短字符串。子模块不能持有 `JSCStringBuf`（生命周期不可控），所以：

```c
/* 实现：内部维护线程局部 JSCStringBuf 池，或直接 strdup */
static const char *js_to_cstring_impl(kwcc_js_ops_t *ops, kwcc_js_val_t val) {
    JSContext *ctx = (JSContext *)ops->ctx;
    JSCStringBuf buf;
    const char *s = JS_ToCString(ctx, val, &buf);
    /* 如果 s 指向 buf（短字符串内联），需要 strdup 保证生命周期 */
    return s ? strdup(s) : NULL;
}

static void js_free_cstring_impl(kwcc_js_ops_t *ops, const char *s) {
    free((void *)s);
}
```

**权衡**：每次 `to_cstring` 都 `strdup` 有性能开销，但子模块的 C 字符串使用场景有限（提取 method/url/body），不是热路径。如果后续有性能问题，可以用 TLS 缓冲池优化。

---

## 实施步骤

| Step | 内容 | 新增/修改文件 | 依赖 |
|------|------|--------------|------|
| 1 | kwcc_js.h 新增类型和接口定义 | `src/kwcc_js.h` | 无 |
| 2 | kwcc_js.c 实现 ops 绑定 + 模块注册 | `src/kwcc_js.c` | Step 1 |
| 3 | 重构现有 HTTP 代码为模块描述符 | `src/kwcc_js.c`（删除内联 HTTP 代码） | Step 2 |
| 4 | 编译验证 | — | Step 3 |

### Step 1: kwcc_js.h 类型定义

在 `kwcc_js.h` 中新增：
- `kwcc_js_val_t` 类型
- `kwcc_js_ops_t` 结构体
- `kwcc_js_module_t` 结构体
- `kwcc_js_register_module()` / `kwcc_js_register_modules()` 声明

保留现有声明（`kwcc_create_js`、`kwcc_destroy_js`、config handler 等）不变。

**验证**：`make` 编译通过

### Step 2: kwcc_js.c ops 实现

1. 实现所有 `js_xxx_impl` 函数
2. `kwcc_create_js()` 中调用 `kwcc_js_ops_init(ctx)` + `kwcc_js_register_modules(&g_kwcc_js_ops)`
3. `kwcc_js_on_bus_event` 改为遍历模块列表分发

**验证**：`make` 编译通过

### Step 3: 重构 HTTP 为模块

1. 将 `kwcc_js.c` 中的 HTTP 相关代码（`kwcc_js_http_request`、`kwcc_js_http_cancel`、`kwcc_register_http_js`、回调注册表、bus 事件路由）移出
2. 在 `kwcc_js.c` 中只保留 `extern kwcc_js_module_t kwcc_js_http_module;` 和注册调用
3. `match_whitelist` 改名 `kwcc_js_match_whitelist`

**注意**：此步不创建 `kwcc_js_http.c/h`，那是 `js-http-implementation-plan.md` 的内容。此步只确保 `kwcc_js.c` 中的 HTTP 代码通过 ops 接口调用，不再直接用 mquickjs API。

**验证**：`make` 编译通过，`make run` 正常

### Step 4: 编译验证

```bash
make clean && make
make run
```

验证项：
- [ ] 无编译错误
- [ ] 无链接错误
- [ ] `make run` 原有功能不受影响
- [ ] `kwcc_js_ops_t` 接口可用（ops 函数指针都能正确调用）

---

## 风险点

| 风险 | 影响 | 缓解 |
|------|------|------|
| `to_cstring` strdup 性能 | 每次提取 C 字符串都 malloc | 非 hot path，可接受 |
| `call_cb` argv 数组传递 | 子模块需要构造 kwcc_js_val_t 数组 | 栈上小数组即可，最多 7 个参数 |
| `kwcc_js_val_t` 和 `JSValue` 实际相同 | 类型安全是编译期的 | `typedef` 提供文档性隔离 |
| bus 事件分发顺序 | 多模块可能对同一 topic 感兴趣 | 模块内部自行判断前缀，互不干扰 |
| 代理表注册时机 | `register_cfun` 需在 `kwcc_create_js` 期间完成 | `kwcc_js_register_modules` 在 ctx 创建后立即调用 |
