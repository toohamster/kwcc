# 实施计划：单线程异步 I/O + Promise 链式调用

> 基于 `requirements/async-io-design.md` 方案
> 创建于 2026-06-25
> 修订于 2026-06-26：架构修正（模块拆分 + 职责归位 + API 简化）

---

## Context

kwcc 缺乏网络请求能力，需要引入异步 HTTP 支持。方案设计已完成（4 层架构），Layer 1（I/O Reactor）和前置依赖（bus 拆分）已完成。本计划覆盖 Layer 2-4 的实施。

---

## 修订记录（2026-06-26）

实施过程中发现架构问题，经讨论确认后修正：

### 问题 1：回调注册表职责错位

`g_kwcc_http_cbs`（JSValue 回调注册表）放在了 `kwcc_http.c/h`，但它是 JS 桥接层的业务（存的是 resolve/reject JSValue），不属于 HTTP 服务职责。HTTP 服务只管请求/响应/进度/发布 bus 消息。

**修正**：回调注册表移到 `kwcc_js_http.c`，`kwcc_http.h` 删除 `kwcc_http_register_callback/find_callback/clear_callback` 三个 API。

### 问题 2：bus 事件路由过于庞大

`kwcc_js_on_bus_event` 混了白名单过滤 + HTTP end/error + HTTP progress + HTTP cancel + 默认 `$bus.emit` 转发，后续加功能只会更膨胀。

**修正**：按 topic 前缀分发，每个模块一个独立函数。HTTP 事件抽到 `kwcc_js_on_http_event`，后续新模块遵循同样规则。

### 问题 3：HTTP JS 桥接代码和核心 JS 桥接代码混在一起

`kwcc_js.c` 700+ 行，HTTP 相关代码独立性强，和 config/TLV/bus 白名单没有交互。

**修正**：拆出 `kwcc_js_http.c/h`，`kwcc_js.c` 保留核心职责（JS 生命周期 + bus 消费者总入口 + 代理表 + config/TLV handler）。

### 问题 4：命名不规范

`match_whitelist` 没有模块前缀。

**修正**：改名 `kwcc_js_match_whitelist`。回调注册表命名 `g_kwcc_js_http_cbs`（`kwcc_js_http_` 前缀）。

### 问题 5：JS API 设计不满足需求

原方案有 `_fetchAsync`/`_addCallback`/`_removeCallback` 等内部方法，命名丑陋且暴露给 JS 端。`$http.request` C 桥接和 JS 高层同名导致覆盖冲突。

**修正**：JS API 重新设计：
- `$http.fetch(url, options)` — 返回 MiniPromise（模拟 web fetch）
- `$http.cancel(reqId)` — 取消请求
- `$http.config(key, value)` — 设置配置
- 不暴露 `_fetchAsync`/`_addCallback`/`_removeCallback`，回调路由全在 C 侧完成
- C 桥接用全局函数 `kwcc_js_mquickjs_call('kwcc_js_http_request', ...)` 而不挂在 `$http` 上
- HTTP 响应数据通过 `$store.dispatch` 更新 state，不直接操作 UI（遵循 store 流）

### 问题 6：http/progress 未实现

`kwcc_http_check_progress` 已发布 bus 事件但 JS 端未路由。需要增量解析 header 拿 `Content-Length` 才能提供 loaded/total。

**修正**：
- `kwcc_http_on_read` 改为增量解析 header：收到数据后先尝试 `phr_parse_response`，解析成功则提取 `Content-Length` 存到 `req->total_size`
- progress bus data 传 `{loaded, total}` 结构体
- `kwcc_js_on_http_event` 实现 progress 路由

---

## 实施总览

按方案分 8 步实施。每步完成后验证编译通过。

| Step | 内容 | 新增/修改文件 | 依赖 |
|------|------|--------------|------|
| 1 | kwcc_http.h 头文件 | `src/kwcc_http.h` | 无 |
| 2 | kwcc_http.c 核心实现 | `src/kwcc_http.c` | Step 1 |
| 3 | kwcc_http 纯 C 测试 | `tests/test_http.c` | Step 2 |
| 4 | C binding + 代理表注册 | `src/kwcc_js_http.c/h`, `src/kwcc_js.c` | Step 2 |
| 5 | Frame Hook + init | `src/main.m` | Step 4 |
| 6 | Makefile 更新 | `Makefile` | Step 1-5 |
| 7 | 编译验证 | — | Step 6 |
| 8 | JS $http.fetch + MiniPromise | `app/runtime/http.js`, `app/main.js` | Step 7 |

---

## Step 1: kwcc_http.h 头文件

**目的**：定义 HTTP 模块的公共 API 和数据结构。

### `src/kwcc_http.h`

```c
#ifndef KWCC_HTTP_H
#define KWCC_HTTP_H

#define KWCC_HTTP_MAX_REQS 8
#define KWCC_HTTP_INIT_CAP 4096

void        kwcc_http_init(void);
const char *kwcc_http_request(const char *method,
                              const char *url, const char **headers, int header_count,
                              const char *body, int body_len);
void        kwcc_http_cancel(const char *req_id);
void        kwcc_http_check_progress(void);

#endif
```

**验证**：`make` 编译通过（仅头文件，无实现不影响链接）

---

## Step 2: kwcc_http.c 核心实现

**目的**：实现 fork + pipe + curl + picohttpparser 解析 + bus dispatch。

### `src/kwcc_http.c`

关键实现点：

**数据结构**：
```c
typedef struct {
    char     req_id[64];
    char     method[16];
    char     url[1024];
    char    *body;
    int      body_len;
    char    *headers[16];
    int      header_count;
    pid_t    pid;
    int      pipe_read_fd;
    char    *response_buf;
    int      response_cap;
    int      response_len;
    int      total_size;
    int      http_status;
    int      last_dispatched;
    int      in_use;
} kwcc_http_req_t;

static kwcc_http_req_t g_kwcc_http_reqs[KWCC_HTTP_MAX_REQS];
static int g_kwcc_http_next_seq = 0;
```

**依赖**：
- `kwcc_io.c/h` — `kwcc_io_register` / `kwcc_io_unregister`
- `kwcc_bus.c/h` — `kwcc_bus_publish` + `KWCC_BUS_WILDCARD`
- `kwcc_base.h` — `kwcc_base_topic_sanitize` + `kwcc_base_topic_check`
- `kwcc_config.h` — `kwcc_config_get_core` 读取 HTTP 配置
- `picohttpparser.h` — `phr_parse_response`
- `llog.h` — 日志

**核心函数**：

1. `kwcc_http_init()` — memset 请求数组
2. `kwcc_http_request()` — 查空闲槽 → 生成 req_id → bin_path 检测 → pipe + fork → kwcc_io_register
3. `kwcc_http_cancel()` — kill(pid, SIGTERM) → kwcc_http_cleanup
4. `kwcc_http_on_read()` (static) — read pipe → realloc 追加 → 增量解析 header（提取 Content-Length 存到 req->total_size）→ EOF 时完整 phr_parse_response → kwcc_http_dispatch_end
5. `kwcc_http_dispatch_end()` (static) — `kwcc_bus_publish("http/end/<req_id>", ...)` 或 `"http/error/<req_id>"`
6. `kwcc_http_cleanup()` (static) — kwcc_io_unregister → close → waitpid → free → memset
7. `kwcc_http_check_progress()` — 每帧调用，检查 response_len != last_dispatched，变化时 publish `"http/progress/<req_id>"`，bus data 传 `{loaded, total}` 结构体
8. `kwcc_http_get_result()` — 公共查询 API，供消费者（如 kwcc_js_http）读取解析结果

**Defensive Fixes**（方案已定义，实现时遵守）：
- Fix 1: realloc +4 + trailing '\0' + NULL 检查
- Fix 4: 循环 phr_parse_response 跳过重定向
- Fix 6: access(bin_path, X_OK) 检测
- Fix 8: phr_parse_response 返回 -2 时 break
- Fix 10: 子进程关闭 > STDERR_FILENO 的所有 FD

**验证**：`make` 编译通过

---

## Step 3: kwcc_http 纯 C 测试

**目的**：验证 HTTP 模块作为独立 C 基础设施的正确性（不依赖 JS）。

### `tests/test_http.c`

| # | 测试 | 验证 |
|---|------|------|
| 1 | kwcc_http_init 后不 crash | 空模块安全 |
| 2 | req_id 递增生成 | 每次 request 返回不同 req_id |
| 3 | 并发请求上限 | 第 9 个请求返回 NULL |
| 4 | bin_path 不存在时返回 NULL | access 检测生效 |
| 5 | topic sanitize 在 dispatch 中生效 | 非法 topic 不 crash |
| 6 | cancel 不存在的 req_id 不 crash | 安全性 |

**注意**：完整的 HTTP 请求测试需要 curl 可执行文件和真实网络，纯 C 测试主要验证数据结构和边界条件。端到端测试在 Step 8 进行。

**验证**：`gcc tests/test_http.c src/kwcc_http.c src/kwcc_bus.c src/kwcc_base.c src/kwcc_io.c src/kwcc_config.c src/kwcc_mempool.c deps/log/log.c deps/picohttpparser/picohttpparser.c -I. -Ideps -D_GNU_SOURCE -o tests/bin/test_http && tests/bin/test_http`

---

## Step 4: C binding + 代理表注册

**目的**：JS→C 通信桥梁，通过代理表注册 HTTP handler。HTTP JS 桥接代码独立为 `kwcc_js_http.c/h`。

### 新增 `src/kwcc_js_http.h`

```c
#ifndef KWCC_JS_HTTP_H
#define KWCC_JS_HTTP_H

#include "mquickjs/mquickjs.h"

void     kwcc_js_on_http_event(const char *topic, const void *data, size_t len, void *user_data);
JSValue  kwcc_js_http_request(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue  kwcc_js_http_cancel(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
void     kwcc_register_http_js(JSContext *ctx);

#endif
```

### 新增 `src/kwcc_js_http.c`

**1. 回调注册表**（JS 桥接层职责，不属于 HTTP 服务）：

```c
static struct {
    char     req_id[64];
    JSValue  on_end_cb;      /* resolve callback */
    JSValue  on_error_cb;    /* reject callback */
    JSValue  on_progress_cb; /* onProgress callback */
    int      in_use;
} g_kwcc_js_http_cbs[KWCC_HTTP_MAX_REQS];
```

**2. `kwcc_js_on_http_event`** — 处理 `http/end`、`http/error`、`http/progress`、`http/cancel`：
- 从 topic 提取 req_id
- 查 `g_kwcc_js_http_cbs` 注册表
- 成功事件：调 `kwcc_http_get_result` 读取解析结果，构建 JSValue 响应对象，调 resolve callback
- 错误/取消事件：构建错误 JSValue，调 reject callback
- 进度事件：从 bus data 取 `{loaded, total}`，调 onProgress callback
- 回调执行后清理注册表条目（end/error/cancel 一次性，progress 不清理直到 end）

**3. `kwcc_js_http_request`** C handler：
- 提取 argv[0]=method, argv[1]=url, argv[2]=headers, argv[3]=body, argv[4]=resolve, argv[5]=reject, argv[6]=onProgress
- 调用 `kwcc_http_request()`
- 将 resolve/reject/onProgress 存入 `g_kwcc_js_http_cbs` 注册表
- 返回 req_id 字符串

**4. `kwcc_js_http_cancel`** C handler：
- 提取 argv[0]=req_id
- 调用 `kwcc_http_cancel(req_id)`

**5. `kwcc_register_http_js`**：
- 注入 `$http` JS 对象壳：`$http.state`、`$http.cancel`、`$http.config`
- 不注入 `$http.request`（避免和 JS 高层 API 同名冲突）
- JS 高层 `$http.fetch` 在 `app/runtime/http.js` 中定义，内部调 `kwcc_js_mquickjs_call('kwcc_js_http_request', ...)`

### `src/kwcc_js.c` 改动

**1. `kwcc_js_on_bus_event` 改为按前缀分发**：
```c
static void kwcc_js_on_bus_event(const char *topic, const void *data, size_t len, void *user_data) {
    JSContext *ctx = (JSContext *)user_data;

    /* HTTP 事件路由 */
    if (strncmp(topic, "http/", 5) == 0) {
        kwcc_js_on_http_event(topic, data, len, user_data);
        return;
    }

    /* 未来其他模块事件路由 */
    /* if (strncmp(topic, "ws/", 3) == 0) { kwcc_js_on_ws_event(...); return; } */

    /* 默认：白名单过滤 + $bus.emit */
    ...
}
```

**2. 删除 HTTP 相关代码**：
- 删除代理表中 `kwcc_js_http_request`/`kwcc_js_http_cancel` 两项
- 删除 `kwcc_js_http_request`/`cancel`/`kwcc_register_http_js` 函数实现
- `match_whitelist` 改名 `kwcc_js_match_whitelist`

**3. 新增 `#include "kwcc_js_http.h"`**

### `src/kwcc_js.h` 改动

删除 `kwcc_js_http_request`/`kwcc_js_http_cancel`/`kwcc_register_http_js` 声明。

**验证**：`make` 编译通过

---

## Step 5: Frame Hook + init

**目的**：在主循环中插入 I/O polling 和 HTTP 进度检查。

### `src/main.m` 改动

**1. `frame()` 中插入 kwcc_io_poll_once**（在 `kwcc_process_js` 之前）：
```c
static void frame(void) {
    int w = sapp_width();
    int h = sapp_height();

    /* 1. I/O reactor polling (non-blocking) */
    kwcc_io_poll_once();
    kwcc_http_check_progress();

    /* 2. 原有 JS 处理 */
    kwcc_process_js(g_js_ctx, "onFrame();");
    // ...
}
```

**2. `init()` 中插入 kwcc_http_init**（在 `kwcc_mempool_init` 之后）：
```c
kwcc_io_init();
kwcc_http_init();
```

**验证**：`make` 编译通过

---

## Step 6: Makefile 更新

**目的**：将 kwcc_http.c 和 kwcc_js_http.c 加入编译列表。

### `Makefile` 改动

**1. `MQJS_SRCS` 加 `src/kwcc_http.c` 和 `src/kwcc_js_http.c`**

**2. 新增编译规则**：
```makefile
$(OBJ_DIR)/src/kwcc_http.o: src/kwcc_http.c src/kwcc_http.h src/kwcc_base.h src/kwcc_bus.h src/kwcc_io.h src/kwcc_config.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/kwcc_js_http.o: src/kwcc_js_http.c src/kwcc_js_http.h src/kwcc_http.h src/kwcc_bus.h src/kwcc_base.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@
```

**3. kwcc_js.o 依赖加 `src/kwcc_js_http.h`**

**验证**：`make clean && make` 编译通过

---

## Step 7: 编译验证

```bash
make clean && make
```

验证项：
- [ ] 无编译错误
- [ ] 无链接错误
- [ ] kwcc_http.o 生成
- [ ] `make run` 窗口正常显示
- [ ] 原有功能不受影响

---

## Step 8: JS $http.fetch + MiniPromise

**目的**：Layer 4 JS API 层，提供 Promise 风格的 HTTP 请求接口。

### `app/runtime/http.js`

**内容**：
1. `MiniPromise` 实现（ES5 兼容，方案中已有完整代码）
2. `$http.fetch(url, options)` — 返回 MiniPromise
3. 不暴露 `_fetchAsync`/`_addCallback`/`_removeCallback`，回调路由全在 C 侧完成

```javascript
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
        /* C 桥接通过全局函数调用，不挂在 $http 上，避免同名覆盖 */
        var reqId = kwcc_js_mquickjs_call('kwcc_js_http_request',
            method, url, headers, body, resolve, reject, onProgress);
        if (!reqId) {
            reject("request failed: unable to start");
            return;
        }
        $http.state.activeRequests = $http.state.activeRequests + 1;
    });
};
```

**业务代码使用模式**（遵循 store 流，不直接操作 UI）：
```javascript
// 模块 action 中发起请求，响应后 dispatch 更新 state
$http.fetch("https://api.example.com/data").then(function(resp) {
    $store.dispatch("myModule", "loadData", resp.body);
}).catch(function(err) {
    $store.dispatch("myModule", "setError", err);
});

// 带进度
$http.fetch(url, {
    method: "POST",
    headers: ["Content-Type: application/json"],
    body: '{"key":"val"}',
    onProgress: function(loaded, total) {
        $store.dispatch("myModule", "setProgress", { loaded: loaded, total: total });
    }
});
```

### `app/main.js` 改动

在 runtime 加载部分新增：
```javascript
load("app/runtime/http.js");
```

### `src/kwcc_js_http.c` 改动

在 `kwcc_register_http_js()` 中注入 `$http` 对象壳：
```javascript
var $http = new Object();
$http.state = { activeRequests: 0 };
$http.cancel = function(reqId) {
    kwcc_js_mquickjs_call('kwcc_js_http_cancel', reqId);
};
$http.config = function(key, value) {
    $config.coreSetTlv('http/' + key, value);
};
```

注意：不注入 `$http.request`，避免和 JS 高层 `$http.fetch` 同名冲突。JS 高层 API 在 `http.js` 中定义。

**验证**：
1. `make` 编译通过
2. `make run` 窗口正常显示
3. 在 JS 控制台测试：`$http.fetch("https://httpbin.org/get").then(function(r) { log(r.status); })`

---

## 风险点

| 风险 | 影响 | 缓解 |
|------|------|------|
| curl 不存在 | 无法发起请求 | Fix 6: access(bin_path, X_OK) 检测，返回明确错误 |
| macOS fork/exec 权限 | 子进程无法启动 | Step 7 先用简单 fork 测试验证 |
| realloc 失败 | crash | Fix 1: 加 NULL 检查 |
| 子进程 FD 污染 | 父进程 socket 泄漏 | Fix 10: 子进程关闭 > STDERR_FILENO 的所有 FD |
| 僵尸进程 | 资源泄漏 | kwcc_http_cleanup + waitpid(WNOHANG) |
| bus 白名单默认值 | http 事件不到达 JS | 默认 `bus/js_whitelist = *`，http/ 前缀自动通过 |
| JSValue 泄漏 | GC 压力 | req_id 回调注册表请求结束后清理 |
