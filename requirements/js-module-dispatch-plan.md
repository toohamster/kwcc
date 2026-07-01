# ModuleMan + dispatch_js_call 实施计划

> 创建于 2026-06-30
> 前置依赖：`requirements/js-bridge-architecture.md`（Steps 1-3 已完成：Facade 类型 + ops 绑定 + 75 测试点通过）
> 被依赖：`requirements/js-http-implementation-plan.md`（HTTP 模块落地需要本计划完成）

---

## Context

当前 JS→C 调用机制是 `kwcc_js_mquickjs_call`，它通过扁平的代理表 `g_kwcc_js_cfun_handlers` 按全名字符串匹配分发。存在以下问题：

1. **代理表是 core 内部数据结构，模块不应该知道它**，但 `register_cfun` 的描述是"注册代理表 C handler"，泄露了实现细节
2. **模块 handler 签名是 ops 风格，代理表要求 mquickjs 签名**，两者不匹配，需要适配层
3. **core 无法维护每个模块的 handler 列表**，代理表是静态数组，模块无法动态添加
4. **`kwcc_js_module_t` 的 `register_cfun` 回调机制不清晰**：模块声明了能力，但没有标准方式把 handler 信息传递给 core

本计划引入 **moduleMan（模块管理器）** 和 **`dispatch_js_call`（二级路由分发）**，替代原有的扁平代理表 + 全名匹配机制。

---

## 设计方案

### 1. JS→C 调用链变更

**旧**：
```
JS: kwcc_js_mquickjs_call('kwcc_js_http_request', method, url, headers, body)
    │ argv[0] = handler 全名，argv[1..] = 参数
    ▼
g_kwcc_js_cfun_handlers[] — 扁平全名匹配
    │ 找到 → func(ctx, this_val, argc-1, argv+1) — mquickjs 签名
    ▼
kwcc_js_http_request(ctx, this_val, argc, argv)
```

**新**：
```
JS: kwcc_js_call_c("http", "request", method, url, headers, body)
    │ argv[0] = module, argv[1] = func, argv[2..] = 参数
    ▼
kwcc_js_call_c (C 入口)
    │ 提取 module/func，argv+2 传给 dispatch
    ▼
dispatch_js_call(module, func, argc, argv)
    │ 查分发表: g_kwcc_js_dispatch[module+func] → handler
    ▼
handler(&g_kwcc_js_ops, argc, argv)  — ops 签名
```

### 2. 核心类型

#### kwcc_js_handler_t — 模块 handler 签名（ops 风格）

```c
typedef kwcc_js_val_t (*kwcc_js_handler_t)(kwcc_js_ops_t *ops, int argc, kwcc_js_val_t *argv);
```

子模块的 C handler 统一使用此签名，不需要知道 `JSContext*`/`JSValue*`/`this_val`。

#### kwcc_js_api_t — 模块 API 声明条目

```c
typedef struct {
    const char *name;              /* handler 名，如 "request"、"cancel" */
    kwcc_js_handler_t func;        /* ops 签名的 handler */
} kwcc_js_api_t;
```

命名用 `api_t` 而非 `handler_entry_t`，强调它描述的是模块对外暴露的 API，不是内部实现细节。

#### kwcc_js_dispatch_t — 分发表条目

```c
typedef struct {
    const char *module;        /* 模块名，如 "http"、"core" */
    const char *func;          /* API 名，如 "request"、"cancel" */
    kwcc_js_handler_t handler; /* ops 签名的 handler */
} kwcc_js_dispatch_t;
```

命名用 `dispatch_t` 而非 `dispatch_entry_t`，和 `kwcc_js_api_t` 区分开——前者是运行时分发条目，后者是模块声明条目。

#### kwcc_js_module_t 变更

```c
/* 旧 */
typedef struct kwcc_js_module {
    const char *name;
    void (*load)(kwcc_js_ops_t *ops);
    void (*register_cfun)(kwcc_js_ops_t *ops);  /* 注册代理表 C handler — 泄露实现细节 */
    void (*on_bus_event)(const char *topic, const void *data,
                         size_t len, kwcc_js_ops_t *ops);
    void (*unload)(kwcc_js_ops_t *ops);
} kwcc_js_module_t;

/* 新 */
typedef struct kwcc_js_module {
    const char *name;
    void (*load)(kwcc_js_ops_t *ops);
    const kwcc_js_api_t *apis;  /* 模块声明的 API 列表（静态数组，NULL 终止） */
    void (*on_bus_event)(const char *topic, const void *data,
                         size_t len, kwcc_js_ops_t *ops);
    void (*unload)(kwcc_js_ops_t *ops);
} kwcc_js_module_t;
```

**关键变更**：`register_cfun` → `apis`

- `register_cfun` 是个回调，模块被调用时需要"做点什么"来注册，机制不明确
- `apis` 是个静态数据声明，模块只产出"我有哪些能力"，core 读取后自行绑定
- 模块不需要调任何 core API，只需要在描述符里列出 API 数组
- core 控制：读取 `mod->apis`，遍历注册进分发表

**模块生命周期变更**：

- 旧：`load → register_cfun → on_bus_event（运行时）→ unload（退出时）`
- 新：`load → (apis 自动注册) → on_bus_event（运行时）→ unload（退出时）`
- `register_cfun` 这一步不再需要，core 在 `kwcc_js_register_module` 中直接读取 `mod->apis` 注册进分发表

### 3. moduleMan（模块管理器）

moduleMan 是 core 内部的概念，不是一个独立的结构体或文件。它在 `kwcc_js.c` 中实现，由以下数据结构和函数组成：

#### 数据结构

模块注册表使用动态增长数组，不设数量上限。API 分发表使用 **module 分组**的二级结构：先找 module，再在 module 内查 handler，避免线性遍历全部 handler。

```c
/* 模块注册表 — 动态增长 */
static kwcc_js_module_t **g_kwcc_js_modules = NULL;
static int g_kwcc_js_module_count = 0;
static int g_kwcc_js_module_cap = 0;

/* API 分发表：module 分组，二级查找 */
typedef struct {
    kwcc_js_dispatch_t *handlers;   /* 该 module 下的 handler 列表 */
    int handler_count;
    int handler_cap;
} kwcc_js_module_dispatch_group_t;

/* module name → dispatch group 映射 — 动态增长 */
static const char **g_kwcc_js_dispatch_modules = NULL;   /* module 名数组 */
static kwcc_js_module_dispatch_group_t *g_kwcc_js_dispatch_groups = NULL;
static int g_kwcc_js_dispatch_group_count = 0;
static int g_kwcc_js_dispatch_group_cap = 0;
```

#### 函数

```c
/* 注册模块 — 读取 mod->apis，遍历添加到分发表 */
static void kwcc_js_register_module(kwcc_js_ops_t *ops, kwcc_js_module_t *mod) {
    log_info("js: loading module '%s'", mod->name);
    if (mod->load) mod->load(ops);

    /* 读取模块声明的 API 列表，注册进分发表 */
    if (mod->apis) {
        for (int i = 0; mod->apis[i].name; i++) {
            kwcc_js_dispatch_add(mod->name, mod->apis[i].name, mod->apis[i].func);
        }
    }

    /* 动态增长 */
    if (g_kwcc_js_module_count >= g_kwcc_js_module_cap) {
        int new_cap = g_kwcc_js_module_cap ? g_kwcc_js_module_cap * 2 : 16;
        g_kwcc_js_modules = realloc(g_kwcc_js_modules, new_cap * sizeof(kwcc_js_module_t *));
        if (!g_kwcc_js_modules) { log_error("js: module registry realloc failed"); return; }
        g_kwcc_js_module_cap = new_cap;
    }
    g_kwcc_js_modules[g_kwcc_js_module_count++] = mod;
    log_info("js: module '%s' registered", mod->name);
}

/* 添加 API 到分发表 — 动态增长 */
static void kwcc_js_dispatch_add(const char *module, const char *func, kwcc_js_handler_t handler) {
    if (g_kwcc_js_dispatch_count >= g_kwcc_js_dispatch_cap) {
        int new_cap = g_kwcc_js_dispatch_cap ? g_kwcc_js_dispatch_cap * 2 : 16;
        g_kwcc_js_dispatch = realloc(g_kwcc_js_dispatch, new_cap * sizeof(kwcc_js_dispatch_t));
        if (!g_kwcc_js_dispatch) { log_error("js: dispatch table realloc failed"); return; }
        g_kwcc_js_dispatch_cap = new_cap;
    }
    g_kwcc_js_dispatch[g_kwcc_js_dispatch_count].module = module;
    g_kwcc_js_dispatch[g_kwcc_js_dispatch_count].func = func;
    g_kwcc_js_dispatch[g_kwcc_js_dispatch_count].handler = handler;
    g_kwcc_js_dispatch_count++;
}

/* JS→C 分发 — 从分发表查找 handler 并调用 */
static kwcc_js_val_t kwcc_js_dispatch_call(const char *module, const char *func,
                                            int argc, kwcc_js_val_t *argv) {
    for (int i = 0; i < g_kwcc_js_dispatch_count; i++) {
        if (strcmp(g_kwcc_js_dispatch[i].module, module) == 0 &&
            strcmp(g_kwcc_js_dispatch[i].func, func) == 0) {
            return g_kwcc_js_dispatch[i].handler(&g_kwcc_js_ops, argc, argv);
        }
    }
    log_error("js: dispatch: unknown handler '%s/%s'", module, func);
    return g_kwcc_js_ops.undefined;
}
```

### 4. kwcc_js_call_c — 新的 JS 全局函数

替代 `kwcc_js_mquickjs_call`，注册为 JS 全局函数 `kwcc_js_call_c`。

```c
/* 新全局函数：kwcc_js_call_c(module, func, ...args) */
JSValue kwcc_js_call_c(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;

    /* 提取 module 和 func（使用 ops 风格，和其余代码统一） */
    kwcc_js_cstr_buf_t mbuf, fbuf;
    const char *module = g_kwcc_js_ops.to_cstring(&g_kwcc_js_ops, argv[0], &mbuf);
    const char *func   = g_kwcc_js_ops.to_cstring(&g_kwcc_js_ops, argv[1], &fbuf);
    if (!module || !func) return JS_UNDEFINED;

    /* 参数从 argv[2] 开始 */
    return kwcc_js_dispatch_call(module, func, argc - 2, argv + 2);
}
```

在 `mqjs_stdlib.c` 中将 `kwcc_js_mquickjs_call` 替换为 `kwcc_js_call_c`：

```c
// 旧
JS_CFUNC_DEF("kwcc_js_mquickjs_call", -1, kwcc_js_mquickjs_call),
// 新
JS_CFUNC_DEF("kwcc_js_call_c", -1, kwcc_js_call_c),
```

### 5. core handler 注册

core 自身的 mempool handler 注册进分发表，使用虚拟模块名 `"core"`。

**`$config` 的 12 个 handler 不处理**：它们通过 `mqjs_stdlib.c` 的 `JS_CFUNC_DEF` 直接注册为 JS 全局函数，不走分发表也不走 `kwcc_js_call_c`，保持现状不动。

**core handler 直接使用 ops 签名**，注册进分发表，虚拟模块名 `"core"`。core 是特殊模块，不需要完整 `kwcc_js_module_t` 生命周期，只需把 handler 注册进分发表即可。

```c
/* core handler：ops 签名，直接调底层 C，不经过 mquickjs 签名中转 */
static kwcc_js_val_t js_core_mempool_dump_stats(kwcc_js_ops_t *ops, int argc, kwcc_js_val_t *argv) {
    (void)ops; (void)argc; (void)argv;
#ifdef KWCC_DEBUG
    kwcc_mempool_dump_stats();
#else
    log_info("dump: not available (build without KWCC_DEBUG)");
#endif
    return ops->undefined;
}

static kwcc_js_val_t js_core_mempool_dump_all(kwcc_js_ops_t *ops, int argc, kwcc_js_val_t *argv) {
    if (argc < 1) return ops->undefined;
#ifdef KWCC_DEBUG
    kwcc_js_cstr_buf_t pbuf;
    const char *path = ops->to_cstring(ops, argv[0], &pbuf);
    if (!path) return ops->undefined;
    int show_content = 0;
    if (argc >= 2) show_content = ops->to_int32(ops, argv[1]);
    kwcc_mempool_dump_all(path, show_content);
#else
    log_info("dumpAll: not available (build without KWCC_DEBUG)");
#endif
    return ops->undefined;
}

/* core API 列表（只有 mempool，config 不走分发表） */
static const kwcc_js_api_t g_kwcc_js_core_apis[] = {
    { "mempool_dump_stats", js_core_mempool_dump_stats },
    { "mempool_dump_all",   js_core_mempool_dump_all },
    { NULL, NULL }
};

/* 在 kwcc_js_register_modules 中注册 core APIs */
void kwcc_js_register_modules(kwcc_js_ops_t *ops) {
    kwcc_js_inject_notify(ops);

    /* 注册 core 自身 API */
    for (int i = 0; g_kwcc_js_core_apis[i].name; i++) {
        kwcc_js_dispatch_add("core", g_kwcc_js_core_apis[i].name, g_kwcc_js_core_apis[i].func);
    }

    /* 注册业务模块 */
    extern kwcc_js_module_t kwcc_js_http_module;
    kwcc_js_register_module(ops, &kwcc_js_http_module);
}
```

**适配器说明**：适配器只做 `ops → ctx` 转换 + 去掉 `this_val`，原始 handler 逻辑完全不变。

**`$config` 处理方式**：12 个 config handler 通过 `mqjs_stdlib.c` 的 `JS_CFUNC_DEF` 直接注册为 JS 全局函数，不走分发表也不走 `kwcc_js_call_c`，保持现状不动。

### 6. 代理表和旧全局函数的移除

| 移除项 | 说明 |
|--------|------|
| `g_kwcc_js_cfun_handlers[]` | 被 `g_kwcc_js_dispatch[]` 替代 |
| `kwcc_js_mquickjs_call()` | 被 `kwcc_js_call_c()` 替代 |
| `kwcc_js_cfun_t` 类型定义 | 被 `kwcc_js_handler_t` 替代 |
| `kwcc_js_cfun_entry_t` 类型定义 | 被 `kwcc_js_dispatch_t` 替代 |

### 7. JS 调用点迁移

当前走 `kwcc_js_mquickjs_call` 的调用点只有 2 个 mempool 调用（在 `kwcc_js.c` 的 JS 字符串中）：

```javascript
// 旧
kwcc_js_mquickjs_call('kwcc_js_mempool_dump_stats')
kwcc_js_mquickjs_call('kwcc_js_mempool_dump_all', p, s)

// 新
kwcc_js_call_c("core", "mempool_dump_stats")
kwcc_js_call_c("core", "mempool_dump_all", p, s)
```

**不在迁移范围内**：
- HTTP 调用点（`kwcc_js_http_request`/`kwcc_js_http_cancel`）：随 HTTP 模块实施时迁移
- `$config` 调用点：不走 `kwcc_js_mquickjs_call`，保持现状

### 8. mqjs_stdlib.c / mqjs_stdlib.h 更新

`kwcc_js_mquickjs_call` 作为 JS 全局函数注册在 `mqjs_stdlib.c` 的 `js_stdlib_funcs[]` 中。需要：

1. 声明改为 `JSValue kwcc_js_call_c(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);`
2. 注册改为 `JS_CFUNC_DEF("kwcc_js_call_c", -1, kwcc_js_call_c),`
3. 重新运行 Stage 1 build 生成新的 `mqjs_stdlib.h`

---

## 实施步骤

## Part 1: 分发机制实施

| Step | 内容 | 新增/修改文件 | 依赖 |
| 1 | kwcc_js.h 类型变更 | `src/kwcc_js.h` | 无 |
| 2 | kwcc_js.c 实现分发机制（不改 handler） | `src/kwcc_js.c` | Step 1 |
| 3 | mqjs_stdlib 更新 + Stage 1 重建 + JS 调用点迁移 | `deps/mquickjs/mqjs_stdlib.c`, `src/kwcc_js.c` | Step 2 |
| 4 | 清理：移除代理表 + 旧声明 | `src/kwcc_js.c`, `src/kwcc_js.h` | Step 3 |
| 5 | 测试更新 + 编译验证 | `tests/test_js_ops.c` | Step 4 |

### Step 1: kwcc_js.h 类型变更

**改动**：

1. 新增 `kwcc_js_handler_t` 类型定义
2. 新增 `kwcc_js_api_t` 类型定义
3. 新增 `kwcc_js_dispatch_t` 类型定义
4. `kwcc_js_module_t` 中 `register_cfun` → `apis`（改为 `const kwcc_js_api_t *apis`）
5. 新增 `kwcc_js_call_c` 声明
6. 保留 `kwcc_js_mquickjs_call` 声明（Step 3 重建后再删）

**验证**：`make` 编译通过

### Step 2: kwcc_js.c 实现分发机制（不改 handler）

**改动**：

1. 新增 `g_kwcc_js_dispatch[]` + `g_kwcc_js_dispatch_count`
2. 实现 `kwcc_js_dispatch_add` / `kwcc_js_dispatch_call`
3. 修改 `kwcc_js_register_module`：读取 `mod->apis` 注册进分发表，删除 `if (mod->register_cfun)` 调用
4. 定义 2 个 mempool 适配器 + `g_kwcc_js_core_apis[]`，在 `kwcc_js_register_modules` 中注册
5. 实现 `kwcc_js_call_c` 函数（使用 `ops->to_cstring` 提取 module/func，调 `kwcc_js_dispatch_call`）
6. 保留 `kwcc_js_mquickjs_call` 作为兼容包装（内部调 `kwcc_js_call_c`），避免 Stage 1 重建前编译断
7. 保留 `g_kwcc_js_cfun_handlers[]` 代理表（Step 4 再删），确保中间态编译通过

**中间态说明**：此时存在两套并行机制——旧的代理表 + 新的分发表。`kwcc_js_mquickjs_call` 走代理表（旧路径），`kwcc_js_call_c` 走分发表（新路径）。JS 端仍调用 `kwcc_js_mquickjs_call`，功能不变。

**验证**：`make` 编译通过，`make run` 正常

### Step 3: mqjs_stdlib 更新 + Stage 1 重建 + JS 调用点迁移

**改动**：

1. `deps/mquickjs/mqjs_stdlib.c`：
   - 声明从 `kwcc_js_mquickjs_call` 改为 `kwcc_js_call_c`
   - 注册从 `JS_CFUNC_DEF("kwcc_js_mquickjs_call", -1, kwcc_js_mquickjs_call)` 改为 `JS_CFUNC_DEF("kwcc_js_call_c", -1, kwcc_js_call_c)`
2. 运行 Stage 1 build 生成新的 `mqjs_stdlib.h`（旧全局函数名 `kwcc_js_mquickjs_call` 从 JS 运行时中移除）
3. `kwcc_js.c` JS 字符串迁移（和 Stage 1 重建同步，否则运行时报错）：
   - `kwcc_register_config_js` 中：`kwcc_js_mquickjs_call('kwcc_js_mempool_dump_stats')` → `kwcc_js_call_c("core", "mempool_dump_stats")`
   - `kwcc_register_config_js` 中：`kwcc_js_mquickjs_call('kwcc_js_mempool_dump_all', p, s)` → `kwcc_js_call_c("core", "mempool_dump_all", p, s)`
4. 删除 `kwcc_js_mquickjs_call` 兼容包装函数

**验证**：`make clean && make` 编译通过，`make run` 正常

### Step 4: 清理

**改动**：

1. 删除 `g_kwcc_js_cfun_handlers[]` 及相关类型（`kwcc_js_cfun_t`、`kwcc_js_cfun_entry_t`）
2. 删除 `kwcc_js.h` 中 `kwcc_js_mquickjs_call` 声明

**验证**：`make clean && make` 编译通过

### Step 5: 测试更新 + 编译验证

**改动**：

1. `tests/test_js_ops.c` 中如有引用 `kwcc_js_mquickjs_call` 的测试，改为 `kwcc_js_call_c`
2. 新增 `kwcc_js_dispatch_call` 分发机制测试：
   - 注册一个测试模块的 API，通过 `kwcc_js_call_c` 从 JS 端调用，验证分发正确
   - 验证未注册的 module/func 返回 undefined
3. `make clean && make && make run` 全流程验证

**验证**：
- [ ] 无编译错误
- [ ] 无链接错误
- [ ] `make run` 原有功能不受影响
- [ ] `kwcc_js_call_c("core", ...)` 可在 JS 中调用
- [ ] 分发表 `g_kwcc_js_dispatch` 正确注册所有 API

---

## Part 2: core handler 签名迁移

> 前置依赖：Part 1 完成（分发机制 + `kwcc_js_call_c` 已上线）
> 被依赖：`requirements/js-http-implementation-plan.md`（**必须在 HTTP 模块实施之前完成**——core 自身必须先遵循 `kwcc_js_handler_t` 规范，再做第一个 Plugin）

### 目的

Part 1 中 core handler 通过适配器包装注册进分发表，原始函数仍使用 mquickjs 签名。本部分将所有 core handler 改为 ops 签名，删除适配器，使 core 自身也遵循 `kwcc_js_handler_t` 规范。

### 迁移范围

只包含 `kwcc_js.c` 中**留在 core 里且走分发机制的** JS 可调用 handler，共 2 个 mempool handler。

**不在范围内**：
- `kwcc_js_http_request` / `kwcc_js_http_cancel`：后续移到 HTTP 模块（`kwcc_js_http.c`），不属于 core
- 12 个 config handler：通过 `mqjs_stdlib.c` 的 `JS_CFUNC_DEF` 直接注册，不走分发表，保持现状
- `kwcc_js_value_to_tlv` / `kwcc_js_tlv_pack_cb` / `kwcc_js_tlv_pack_state_t`：内部辅助函数，不暴露给 JS

**mempool 组（2 个）**：

| 旧签名 | 新签名 |
|--------|--------|
| `JSValue kwcc_js_mempool_dump_stats(JSContext*, JSValue*, int, JSValue*)` | `kwcc_js_val_t js_core_mempool_dump_stats(kwcc_js_ops_t*, int, kwcc_js_val_t*)` |
| `JSValue kwcc_js_mempool_dump_all(JSContext*, JSValue*, int, JSValue*)` | `kwcc_js_val_t js_core_mempool_dump_all(kwcc_js_ops_t*, int, kwcc_js_val_t*)` |

### 改写规则

每个 handler 内部需要把 mquickjs API 替换为 ops 调用：

| mquickjs API | ops 替代 |
|-------------|---------|
| `JS_ToCString(ctx, val, &buf)` | `ops->to_cstring(ops, val, &buf)` |
| `JS_ToInt32(ctx, &out, val)` | `ops->to_int32(ops, val)` |
| `JS_GetPropertyStr(ctx, obj, key)` | `ops->get_str_prop(ops, obj, key)` |
| `JS_GetPropertyUint32(ctx, obj, idx)` | `ops->array_get(ops, obj, idx)` |
| `JS_GetClassID(ctx, val)` | `ops->get_class_id(ops, val)` |
| `JS_IsTrue(val)` / `JS_FALSE` | 直接比较（mquickjs 宏，不依赖 ctx，可保留） |
| `JS_UNDEFINED` | `ops->undefined` |
| `(void)this_val` | 不再需要（ops 签名没有 this_val） |

### 实施步骤

| Step | 内容 | 依赖 |
|------|------|------|
| 2a | mempool 组（2 个）签名迁移 | Part 1 |
| 2b | 删除适配器 + 更新 `g_kwcc_js_core_apis[]` + 清理 `.h` 声明 | Step 2a |
| 2c | 编译验证 + 测试 | Step 2b |

### Step 2a: mempool 组签名迁移

**改动**：

1. `kwcc_js_mempool_dump_stats` → `js_core_mempool_dump_stats`：改签名，内部 `JS_UNDEFINED` → `ops->undefined`
2. `kwcc_js_mempool_dump_all` → `js_core_mempool_dump_all`：改签名，`JS_ToCString` → `ops->to_cstring`，`JS_ToInt32` → `ops->to_int32`

**验证**：`make` 编译通过

### Step 2b: 删除适配器 + 更新注册

**改动**：

1. 删除 2 个适配器函数（`js_core_mempool_dump_stats_adapter`、`js_core_mempool_dump_all_adapter`）
2. `g_kwcc_js_core_apis[]` 中直接引用新签名的函数
3. `kwcc_js.h` 中删除旧的 mquickjs 签名声明（`kwcc_js_mempool_dump_stats`、`kwcc_js_mempool_dump_all`），这些函数改为 static

**验证**：`make clean && make` 编译通过

### Step 2c: 编译验证 + 测试

**验证**：
- [ ] `make clean && make` 无编译错误
- [ ] `make run` 原有功能不受影响
- [ ] `kwcc_js_call_c("core", "mempool_dump_stats")` 在 JS 端可正常调用
- [ ] 适配器全部删除，分发表直接引用 ops 签名 handler

---

## 与 js-bridge-architecture.md 的关系

本计划实施后，需要同步更新 `js-bridge-architecture.md`：

| 项目 | 变更 |
|------|------|
| `kwcc_js_module_t` | `register_cfun` → `apis`，生命周期不再有 `register_cfun` 阶段 |
| `kwcc_js_ops_t` | 不变 |
| 代理表 | 删除，由分发表替代 |
| `kwcc_js_mquickjs_call` | 删除，由 `kwcc_js_call_c` 替代 |
| core handler 签名 | Part 2 迁移 2 个 mempool handler 为 ops 签名 |
| `$config` handler | 不处理，保持 `JS_CFUNC_DEF` 直接注册 |

---

## 风险点

| 风险 | 影响 | 缓解 |
|------|------|------|
| Stage 1 重建 + JS 调用点迁移必须同步 | 重建后旧全局函数名消失，JS 字符串不同步改就会运行时报错 | Part 1 Step 3 合并处理，重建和迁移在同一步完成 |
| JS 调用点遗漏 | 某处 `kwcc_js_mquickjs_call` 未迁移到 `kwcc_js_call_c` | grep 全局搜索确保无遗漏 |
| argv 偏移量变化 | 旧：argv[0]=全名，handler 拿 argv+1；新：argv[0]=module, argv[1]=func, handler 拿 argv+2 | 适配器内部做偏移转换，原始 handler 不受影响 |
| core handler 签名迁移 | 2 个 mempool handler 需要改写 | Part 2 工作量小，直接改写 |
