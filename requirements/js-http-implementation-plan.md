# kwcc_js_http 实施计划

> 创建于 2026-06-28
> 前置依赖：`requirements/js-bridge-architecture.md`（Facade + Plugin 架构调整完成）
> 关联方案：`requirements/async-io-implementation-plan.md`（Layer 2 kwcc_http.c 已实施，Step 4 需要本计划替代）
> 被依赖：无

---

## Context

`requirements/async-io-implementation-plan.md` 的 Layer 2（`kwcc_http.c`）和 Layer 4（`http.js`）已实施。原 Step 4（C binding + 代理表注册）因架构问题需要重构。

本计划是基于 Facade + Plugin 架构的 HTTP 模块落地实施方案。`kwcc_js_http` 作为第一个 Plugin，遵循 `kwcc_js_module_t` 规范注册进 core。

---

## 修订记录

基于 `requirements/async-io-implementation-plan.md` 修订记录（2026-06-26）的 6 个修正，本计划延续以下决策：

1. 回调注册表属于 JS 桥接层（`kwcc_js_http`），不属于 HTTP 服务
2. bus 事件路由按 topic 前缀分发
3. HTTP JS 桥接代码独立为 `kwcc_js_http.c/h`
4. 命名规范：`kwcc_js_match_whitelist`、`g_kwcc_js_http_cbs`
5. JS API：`$http.fetch(url, options)` 返回 MiniPromise，不暴露内部方法
6. http/progress 增量解析 header 提取 Content-Length，bus data 传 `{loaded, total}`

---

## 实施总览

| Step | 内容 | 新增/修改文件 | 依赖 |
|------|------|--------------|------|
| 1 | kwcc_http.h 隐藏 phr_header | `src/kwcc_http.h` | 无 |
| 2 | kwcc_http.c 适配 header 访问 API + progress + 增量解析 | `src/kwcc_http.c` | Step 1 |
| 3 | kwcc_js_http.h 模块声明 | `src/kwcc_js_http.h` | 架构调整完成 |
| 4 | kwcc_js_http.c 模块实现 | `src/kwcc_js_http.c` | Step 2 + Step 3 |
| 5 | kwcc_js.c 删除内联 HTTP 代码，注册 http 模块 | `src/kwcc_js.c` | Step 4 |
| 6 | Makefile 更新 | `Makefile` | Step 4 |
| 7 | promise.js + http.js | `app/runtime/promise.js`, `app/runtime/http.js`, `app/main.js` | Step 5 |
| 8 | 编译验证 | — | Step 7 |

---

## Step 1: kwcc_http.h 隐藏 phr_header

**目的**：`kwcc_http_result_t` 当前暴露 `const struct phr_header *headers`，导致消费者必须 include `picohttpparser.h`。这违反了 Layer 2 不依赖解析库实现细节的原则。

### 改动

删除 `kwcc_http_result_t` 中的 `headers` 和 `num_headers` 字段，替换为 header 访问 API：

```c
/* 旧 */
typedef struct {
    int      error;
    int      status;
    const char *body;
    int      body_len;
    const struct phr_header *headers;  /* ← 暴露实现细节 */
    size_t   num_headers;
} kwcc_http_result_t;

/* 新 */
typedef struct {
    int      error;
    int      status;
    const char *body;
    int      body_len;
} kwcc_http_result_t;

/* Header 访问 API（隐藏 phr_header） */
int kwcc_http_result_header_count(const char *req_id);
int kwcc_http_result_get_header(const char *req_id, int index,
                                const char **name, int *name_len,
                                const char **value, int *value_len);
```

子模块通过 `kwcc_http_result_header_count` + `kwcc_http_result_get_header` 访问 header，不需要知道 `phr_header`。

**验证**：`make` 编译通过

---

## Step 2: kwcc_http.c 适配

**目的**：实现 header 访问 API + progress bus data + 增量 header 解析。

### 改动

1. 删除回调注册表（`g_kwcc_http_cbs` + `register/find/clear_callback`）— 回调注册属于 JS 桥接层
2. 实现 `kwcc_http_result_header_count` / `kwcc_http_result_get_header`
3. `kwcc_http_check_progress` 传 `kwcc_http_progress_t` 结构体作为 bus data
4. `kwcc_http_on_read` 增量解析 header 提取 `Content-Length` 存到 `req->total_size`

**验证**：`make` 编译通过

---

## Step 3: kwcc_js_http.h 模块声明

**目的**：定义 HTTP 模块描述符和对外接口。

### `src/kwcc_js_http.h`

```c
#ifndef KWCC_JS_HTTP_H
#define KWCC_JS_HTTP_H

#include "kwcc_js.h"    /* kwcc_js_ops_t, kwcc_js_module_t */

/* HTTP 模块描述符 — 供 core 注册用 */
extern kwcc_js_module_t kwcc_js_http_module;

#endif
```

**设计要点**：
- 只 include `kwcc_js.h`，不 include `mquickjs.h`
- 不暴露 `kwcc_js_http_request`/`kwcc_js_http_cancel` 等内部函数
- 模块描述符是唯一的对外接口

**验证**：`make` 编译通过

---

## Step 4: kwcc_js_http.c 模块实现

**目的**：HTTP Plugin 的完整实现，通过 `kwcc_js_ops_t` 操作 JS，不直接碰 mquickjs。

### `src/kwcc_js_http.c`

**依赖**：`kwcc_js.h` + `kwcc_http.h`（不 include mquickjs.h / picohttpparser.h / llog.h）

**1. 模块级 ops 指针**：

```c
static kwcc_js_ops_t *s_ops;
```

**2. 回调注册表**（JS 桥接层职责）：

```c
static struct {
    char          req_id[64];
    kwcc_js_val_t on_end_cb;
    kwcc_js_val_t on_error_cb;
    kwcc_js_val_t on_progress_cb;
    int           in_use;
} g_kwcc_js_http_cbs[KWCC_HTTP_MAX_REQS];
```

**3. `http_load`** — 注入 `$http` 对象壳：

```c
static void http_load(kwcc_js_ops_t *ops) {
    s_ops = ops;
    const char *code =
        "var $http = new Object();\n"
        "$http.state = { activeRequests: 0 };\n"
        "$http.cancel = function(reqId) {\n"
        "    kwcc_js_mquickjs_call('kwcc_js_http_cancel', reqId);\n"
        "};\n"
        "$http.config = function(key, value) {\n"
        "    $config.coreSetTlv('http/' + key, value);\n"
        "};\n";
    ops->eval(ops, code, strlen(code), "<$http>", JS_EVAL_REPL);
}
```

**4. `http_register_cfun`** — 注册代理表 C handler（返回模块名+函数指针供 core 添加到代理表）

**5. `http_on_bus_event`** — 处理 `http/end`、`http/error`、`http/progress`、`http/cancel`：

- 全部通过 `s_ops->new_object`、`s_ops->set_str_prop`、`s_ops->call_cb` 等 ops 接口操作
- `kwcc_http_get_result` 读取解析结果
- `kwcc_http_result_get_header` 访问 header（不碰 phr_header）
- `kwcc_http_progress_t` 结构体从 bus data 读取 loaded/total

**6. 模块描述符**：

```c
kwcc_js_module_t kwcc_js_http_module = {
    .name = "http",
    .load = http_load,
    .register_cfun = http_register_cfun,
    .on_bus_event = http_on_bus_event,
};
```

**验证**：`make` 编译通过

---

## Step 5: kwcc_js.c 删除内联 HTTP 代码

**目的**：`kwcc_js.c` 只保留 core 职责，HTTP 由模块提供。

### 改动

1. 删除 `kwcc_js_http_request` / `kwcc_js_http_cancel` / `kwcc_register_http_js` 函数实现
2. 删除代理表中 `kwcc_js_http_request` / `kwcc_js_http_cancel` 两项（模块的 `register_cfun` 负责添加）
3. 删除 `kwcc_js_on_bus_event` 中 HTTP 事件路由（模块的 `on_bus_event` 负责）
4. `match_whitelist` 改名 `kwcc_js_match_whitelist`
5. `kwcc_js_on_bus_event` 改为遍历模块列表分发
6. `kwcc_create_js` 中调用 `kwcc_js_register_modules(&g_kwcc_js_ops)` 替代 `kwcc_register_http_js(ctx)`
7. 删除 `kwcc_js.h` 中的 `kwcc_register_http_js` / `kwcc_js_http_request` / `kwcc_js_http_cancel` 声明

**验证**：`make` 编译通过

---

## Step 6: Makefile 更新

### 改动

1. `MQJS_SRCS` 加 `src/kwcc_js_http.c`
2. 新增 `kwcc_js_http.o` 编译规则
3. `kwcc_js.o` 依赖加 `src/kwcc_js_http.h`

**验证**：`make clean && make` 编译通过

---

## Step 7: promise.js + http.js

**目的**：Layer 4 JS API，提供 Promise 风格的 HTTP 请求接口。MiniPromise 作为通用基础设施独立为单独文件。

### `app/runtime/promise.js`

内容：`MiniPromise` 实现（ES5 兼容）

- 独立文件，不与 HTTP 耦合
- 未来任何需要异步的场景（WebSocket、定时器链式调用等）都可复用
- `$http.fetch` 和其他模块只负责使用 MiniPromise，不负责定义它

### `app/runtime/http.js`

内容：`$http.fetch(url, options)` — 返回 MiniPromise

- 内部调 `kwcc_js_mquickjs_call('kwcc_js_http_request', method, url, headers, body, resolve, reject, onProgress)`
- 不暴露 `_fetchAsync` / `_addCallback` / `_removeCallback`
- 不包含 MiniPromise 定义

### `app/main.js` 加载顺序

```javascript
load("app/runtime/store.js");
load("app/runtime/bus.js");
load("app/runtime/promise.js");   // MiniPromise — 通用基础设施
load("app/runtime/http.js");      // $http.fetch（依赖 MiniPromise）
```

业务代码遵循 store 流：
```javascript
$http.fetch("https://api.example.com/data").then(function(resp) {
    $store.dispatch("myModule", "loadData", resp.body);
}).catch(function(err) {
    $store.dispatch("myModule", "setError", err);
});
```

**验证**：`make` 编译通过

---

## Step 8: 编译验证

```bash
make clean && make
make run
```

验证项：
- [ ] 无编译错误
- [ ] 无链接错误
- [ ] `kwcc_js_http.o` 生成
- [ ] `make run` 原有功能不受影响
- [ ] `$http.fetch` 可在 JS 控制台调用（需要 curl 和网络）

---

## 与原方案的关系

| 原方案 Step | 状态 | 说明 |
|-------------|------|------|
| Step 1: kwcc_http.h | ✅ 已完成 | 本计划 Step 1 在此基础上隐藏 phr_header |
| Step 2: kwcc_http.c | ✅ 已完成 | 本计划 Step 2 在此基础上删除回调 API + 加 progress + 增量解析 |
| Step 3: 纯 C 测试 | ✅ 已完成 | 不需要改动 |
| Step 4: C binding + 代理表 | 🔄 本计划替代 | 原方案已拆分为架构调整 + 本计划 |
| Step 5: Frame Hook + init | ✅ 已完成 | 不需要改动 |
| Step 6: Makefile | 🔄 本计划 Step 6 | 加 kwcc_js_http.c |
| Step 7: 编译验证 | 🔄 本计划 Step 8 | |
| Step 8: JS $http.fetch | 🔄 本计划 Step 7 | promise.js 独立 + http.js 重写 |
