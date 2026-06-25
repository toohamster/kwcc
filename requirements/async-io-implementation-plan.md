# 实施计划：单线程异步 I/O + Promise 链式调用

> 基于 `requirements/async-io-design.md` 方案
> 创建于 2026-06-25

---

## Context

kwcc 缺乏网络请求能力，需要引入异步 HTTP 支持。方案设计已完成（4 层架构），Layer 1（I/O Reactor）和前置依赖（bus 拆分）已完成。本计划覆盖 Layer 2-4 的实施。

---

## 实施总览

按方案分 8 步实施。每步完成后验证编译通过。

| Step | 内容 | 新增/修改文件 | 依赖 |
|------|------|--------------|------|
| 1 | kwcc_http.h 头文件 | `src/kwcc_http.h` | 无 |
| 2 | kwcc_http.c 核心实现 | `src/kwcc_http.c` | Step 1 |
| 3 | kwcc_http 纯 C 测试 | `tests/test_http.c` | Step 2 |
| 4 | C binding + 代理表注册 | `src/kwcc_js.c` | Step 2 |
| 5 | Frame Hook + init | `src/main.m` | Step 4 |
| 6 | Makefile 更新 | `Makefile` | Step 1-5 |
| 7 | 编译验证 | — | Step 6 |
| 8 | JS $http 对象 + MiniPromise | `app/runtime/http.js`, `app/main.js` | Step 7 |

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
4. `kwcc_http_on_read()` (static) — read pipe → realloc 追加 → EOF 时 phr_parse_response → kwcc_http_dispatch_end
5. `kwcc_http_dispatch_end()` (static) — `kwcc_bus_publish("http/end/<req_id>", ...)` 或 `"http/error/<req_id>"`
6. `kwcc_http_cleanup()` (static) — kwcc_io_unregister → close → waitpid → free → memset
7. `kwcc_http_check_progress()` — 每帧调用，检查 response_len != last_dispatched，变化时 publish `"http/progress/<req_id>"`

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

**目的**：JS→C 通信桥梁，通过代理表注册 HTTP handler。

### `src/kwcc_js.c` 改动

**1. 代理表新增两项**（`g_kwcc_js_cfun_handlers[]`）：
```c
{ "kwcc_js_http_request", kwcc_js_http_request },
{ "kwcc_js_http_cancel",  kwcc_js_http_cancel },
```

**2. 新增 C handler 函数**：

`kwcc_js_http_request(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)`:
- 提取 argv[0]=method, argv[1]=url, argv[2]=headers, argv[3]=body
- 调用 `kwcc_http_request()`
- 返回 req_id 字符串（`JS_NewString`）

`kwcc_js_http_cancel(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)`:
- 提取 argv[0]=req_id
- 调用 `kwcc_http_cancel(req_id)`

**3. req_id 回调注册表**：

```c
#define KWCC_HTTP_CB_MAX 8
static struct {
    char     req_id[64];
    JSValue  on_end_cb;
    JSValue  on_error_cb;
    JSValue  on_progress_cb;
    int      in_use;
} g_kwcc_http_cbs[KWCC_HTTP_CB_MAX];
```

**4. bus 事件路由**：

`kwcc_js_on_bus_event` 已在 `kwcc_create_js` 中注册为 bus consumer（`KWCC_BUS_WILDCARD`），收到 `http/end/req_1` 等 topic 时：
- 从 topic 提取 req_id（`topic + strlen("http/end/")`）
- 查 `g_kwcc_http_cbs` 注册表
- 调用对应的 resolve/reject callback

**5. 新增 `kwcc_register_http_js(JSContext *ctx)`**：

注入 `$http` JS 对象和 MiniPromise（详见 Step 8）。

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

**目的**：将 kwcc_http.c 加入编译列表。

### `Makefile` 改动

**1. `MQJS_SRCS` 加 `src/kwcc_http.c`**

**2. 新增编译规则**：
```makefile
$(OBJ_DIR)/src/kwcc_http.o: src/kwcc_http.c src/kwcc_http.h src/kwcc_base.h src/kwcc_bus.h src/kwcc_io.h src/kwcc_config.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -c $< -o $@
```

**3. kwcc_js.o 依赖加 `src/kwcc_http.h`**

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

## Step 8: JS $http 对象 + MiniPromise

**目的**：Layer 4 JS API 层，提供 Promise 风格的 HTTP 请求接口。

### `app/runtime/http.js`

**内容**：
1. `MiniPromise` 实现（ES5 兼容，方案中已有完整代码）
2. `$http` 对象定义：
   - `$http.request(url, options)` — 返回 MiniPromise
   - `$http.cancel(reqId)` — 取消请求
   - `$http.config(key, value)` — 设置配置
   - `$http.state = { activeRequests: 0 }` — 运行状态
3. `$http._fetchAsync(method, url, headers, body, onProgress)` — 内部实现
4. `$http._addCallback(reqId, type, cb)` / `$http._removeCallback(reqId)` — 回调注册表管理

**回调路由机制**：
- bus consumer 收到 `http/end/<req_id>` 事件时，`$bus.on("http/end", ...)` 触发
- 从 event data 中取 req_id，查注册表，调用对应 resolve/reject

### `app/main.js` 改动

在 runtime 加载部分新增：
```javascript
load("app/runtime/http.js");
```

### `src/kwcc_js.c` 改动

在 `kwcc_create_js()` 末尾新增：
```c
kwcc_register_http_js(ctx);
```

`kwcc_register_http_js` 通过 `JS_Eval` 注入 `$http` 对象的 C 桥接部分。

**验证**：
1. `make` 编译通过
2. `make run` 窗口正常显示
3. 在 JS 控制台测试：`$http.request("https://httpbin.org/get").then(function(r) { log(r); })`

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
