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

1. ~~回调注册表属于 JS 桥接层（`kwcc_js_http`），不属于 HTTP 服务~~ → **进一步**：C 端不存回调，JS 端通过 `$http.callbacks` 自己做映射
2. bus 事件路由按 topic 前缀分发
3. HTTP JS 桥接代码独立为 `kwcc_js_http.c/h`
4. 命名规范：`kwcc_js_match_whitelist`
5. JS API：`$http.fetch(url, options)` 返回 MiniPromise，不暴露内部方法
6. http/progress 增量解析 header 提取 Content-Length，bus data 传 `{loaded, total}`

新增修正（2026-06-28 讨论）：

7. **C 端不存 JSValue 回调**：C 端只管 publish 事件 + 调 `ops->notify_js` 通知 JS 端，JS 端通过 `$notify.on('http', handler)` 注册处理器，自己维护 `$http.callbacks` 做 id → resolve/reject 映射
8. **`$notify` 作为通用 C→JS 通知通道**：不走 `$bus.emit`（避免字符串拼接和 bus 匹配开销），C 端通过 `ops->notify_js` 直达 `$notify.emit`，按 type 分发给模块 handler。`$notify` 不局限于 HTTP，未来 WS/Timer 等模块遵循同样规则
9. **MiniPromise 独立为 `promise.js`**：通用基础设施，不与 HTTP 耦合
10. **cancel 必须触发清理事件**：`kwcc_http_cancel` 发布 `http/cancel/<reqId>` 后，JS 端的 `_onNotify` 必须处理 `cancel` 事件（和 `error` 一样调 `reject` + `delete callbacks[id]`），否则 Promise 永久悬挂
11. **`kwcc_http_get_result` 时序契约 + `ack_cleanup` 自动释放**：`dispatch_end` 后不再立刻 cleanup，标记 `PENDING_ACK`。`http_on_bus_event` 构建 JSValue（数据拷贝进 GC 堆）后调 `notify_js`，`ack_cleanup`（即 `kwcc_http_release_result`）在投递前自动释放 C 侧资源。此契约支持同步和异步 bus
12. **僵尸进程回收**：`cleanup` 中 `waitpid(WNOHANG)` 单次尝试可能回收不到（cancel 后子进程还没退出）。需在 `kwcc_http_check_progress` 中加一轮对所有 in_use slot 的 `waitpid(WNOHANG)` 扫描，回收已退出的子进程

---

## C→JS 通知模式

### 职责划分

| 层 | 职责 | 不负责 |
|----|------|--------|
| C 端（kwcc_http） | 发请求、监控进度、publish bus 事件 | 不知道谁在等结果 |
| C 端（kwcc_js_http） | 收 bus 事件、构建 JSValue 响应、调 `ops->notify_js` | 不存 resolve/reject |
| JS 端（$notify） | 按 type 分发 C→JS 通知给模块 handler | 不关心具体业务逻辑 |
| JS 端（$http） | 维护 `$http.callbacks` 映射、调 resolve/reject | 不关心数据怎么来的 |

### 数据流

```
kwcc_http (fork+pipe+curl)
    │ publish bus event
    ▼
kwcc_js_http (on_bus_event)
    │ 读取 kwcc_http_get_result → 构建 JSValue 响应
    │ ops->notify_js(ops, "http", "end", reqId, dataObj, kwcc_http_release_result)
    ▼
kwcc_js_ops.notify_js (core 实现)
    │ ① ack_cleanup(reqId) — 释放 C 侧 buffer（数据已在 GC 堆）
    │ ② call_cb($notify.emit, [type, event, id, data])
    ▼
$notify.emit("http", "end", reqId, data)
    │ 查 _handlers["http"]
    │ 调匿名 handler(event, id, data)
    ▼
匿名 handler → $http.callbacks[reqId] → resolve/reject
    ▼
MiniPromise resolved/rejected
    │ .then() / .catch()
    ▼
业务代码 ($store.dispatch)
```

### 为什么不走 `$bus.emit`

1. `$bus.emit` 需要字符串拼接 topic + 数据 → 有注入风险和性能开销
2. `$bus.emit` 内部要做 topic 匹配 + 遍历订阅者 → 多一层间接
3. C→JS 通知是原生事件，和 JS→JS 业务事件职责不同

`$notify` 是 C→JS 的**通用通知通道**，和 `$bus`（JS→JS 事件总线）对称。C 端通过 `ops->notify_js` 直达 `$notify.emit`，零字符串拼接，零 bus 匹配。

### `$notify` vs `$bus`

| 通道 | 方向 | 场景 |
|------|------|------|
| `$notify` | C → JS | 原生事件（HTTP 响应、WS 消息、Timer 触发） |
| `$bus` | JS → JS | 业务事件（按钮点击、state 变更） |

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

1. 删除回调注册表（`g_kwcc_http_cbs` + `register/find/clear_callback`）— 回调注册不属于 HTTP 服务
2. 实现 `kwcc_http_result_header_count` / `kwcc_http_result_get_header`
3. `kwcc_http_check_progress` 传 `kwcc_http_progress_t` 结构体作为 bus data
4. `kwcc_http_on_read` 增量解析 header 提取 `Content-Length` 存到 `req->total_size`
5. **僵尸回收**：`kwcc_http_check_progress` 中加一轮对所有 `in_use` slot 的 `waitpid(pid, NULL, WNOHANG)` 扫描，回收已退出但未被 cleanup 捕获的子进程。如果 `waitpid` 返回 `> 0`（子进程已退出），发布 `http/end` 或 `http/error` 事件并调 cleanup
6. **请求状态机**：`dispatch_end` 后不再立刻 cleanup，改为 `PENDING_ACK` 状态。新增 `kwcc_http_release_result(req_id)` 供 `ack_cleanup` 调用，释放 buffer + cleanup。`check_progress` 中加 `PENDING_ACK` 超时兜底（防止消息丢失导致 slot 泄漏）
7. **无 Content-Length 哨兵值**：`kwcc_http_progress_t.total` 在无 Content-Length（如 chunked 编码）时设为 `-1`，JS 端据此跳过百分比计算，避免除零。`$http._onNotify` 的 progress 分支检查 `data.total === -1` 时只传 `loaded`

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

**2. `http_load`** — 创建 `$http` 对象 + 注入 C→JS 桥接方法：

```c
static void http_load(kwcc_js_ops_t *ops) {
    s_ops = ops;
    /* 创建 $http 对象（C API，不是 JS 字符串里的 var） */
    kwcc_js_val_t http_obj = ops->new_object(ops);
    ops->set_str_prop(ops, ops->global_obj, "$http", http_obj);

    /* 注入 C→JS 桥接方法（和 kwcc_register_config_js 风格一致） */
    const char *code =
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

**设计要点**：
- `$http` 对象通过 `ops->new_object` + `set_str_prop` 创建，和 `kwcc_register_config_js` 风格一致
- C 端只注入 C→JS 桥接方法（`cancel`/`config`），纯 JS 逻辑在 `http.js` 中定义
- `$http.callbacks`/`$http.onNotify`/`$notify.on('http', ...)` 全部移到 `http.js`，C 端不注入
- C 端通过 `ops->notify_js(ops, "http", event, id, data, ack_cleanup)` 通知 JS 端

**3. `http_register_cfun`** — 注册代理表 C handler

**4. `http_on_bus_event`** — 收 bus 事件后构建 JSValue 响应，调 `ops->notify_js`：

- 判断 topic 前缀（`http/end`、`http/error`、`http/progress`、`http/cancel`）
- 从 topic 提取 req_id
- `kwcc_http_get_result` 读取解析结果
- `kwcc_http_result_get_header` 访问 header
- 构建 JSValue 响应对象（通过 `ops->new_object`、`ops->set_str_prop`）— **数据通过 `JS_NewStringLen` 拷贝进 GC 堆，不再依赖 C buffer**
- 调 `ops->notify_js(ops, "http", event, req_id, data_obj, kwcc_http_release_result)` — `ack_cleanup` 在投递前自动释放 C 侧资源
- **不存任何 JSValue 回调，不维护回调注册表，不持有 JS 函数引用，不手动调 cleanup**

**5. 模块描述符**：

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

- `$http.callbacks` — req_id → {resolve, reject, onProgress} 映射表（纯 JS 端数据）
- `$notify.on('http', handler)` — 注册匿名处理器处理 C→JS 通知
- `$http.fetch` — 调 `kwcc_js_mquickjs_call('kwcc_js_http_request', method, url, headers, body)` 发起请求
- 不暴露 `_fetchAsync` / `_addCallback` / `_removeCallback`
- 不包含 MiniPromise 定义

```javascript
// $http.callbacks — 纯 JS 端逻辑（C 端不注入）
$http.callbacks = {};

$notify.on('http', function(event, id, data) {
    var cb = $http.callbacks[id];
    if (!cb) return;
    if (event === 'end') {
        cb.resolve(data); delete $http.callbacks[id];
    } else if (event === 'error') {
        cb.reject(data); delete $http.callbacks[id];
    } else if (event === 'cancel') {
        cb.reject(data || 'request cancelled'); delete $http.callbacks[id];
    } else if (event === 'progress') {
        if (cb.onProgress) cb.onProgress(data.loaded, data.total);
    }
});

$http.fetch = function(url, options) {
    var method = "GET";
    var headers = [];
    var body = "";
    var onProgress = null;
    if (options) {
        if (options.method) method = options.method;
        if (options.headers) headers = options.headers;
        if (options.body) body = options.body;
        if (options.onProgress) onProgress = options.onProgress;
    }

    return new MiniPromise(function(resolve, reject) {
        var reqId = kwcc_js_mquickjs_call('kwcc_js_http_request',
            method, url, headers, body);
        if (!reqId) {
            reject("request failed: unable to start");
            return;
        }
        $http.callbacks[reqId] = {
            resolve: resolve,
            reject: reject,
            onProgress: onProgress
        };
        $http.state.activeRequests = $http.state.activeRequests + 1;
    });
};
```

**说明**：`$http.callbacks[id]` 的 key 是 reqId，和 `$notify` payload 中的 `id` 字段对应。`$http.onNotify(event, id, data)` 收到通知后用 `id` 查 `callbacks[id]`。

**C 端 vs JS 端职责**：
- C 端（`http_load`）：`ops->new_object` 创建 `$http` + 注入 C→JS 桥接方法（`cancel`/`config`）
- JS 端（`http.js`）：`callbacks`/`onNotify`/`$notify.on`/`fetch` — 纯 JS 逻辑

### `app/main.js` 加载顺序

```javascript
load("app/runtime/store.js");
load("app/runtime/bus.js");
load("app/runtime/promise.js");   // MiniPromise — 通用基础设施
load("app/runtime/http.js");      // $http.fetch（依赖 MiniPromise + $notify）
```

**注意**：`$notify` 由 core（`kwcc_js.c`）在模块注册前注入，不需要 JS 文件加载。模块的 `load` 回调中通过 `$notify.on(type, handler)` 注册处理器。

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
