# 方案：单线程异步 I/O + Promise 链式调用

> 状态：待论证
> 优先级：继 ID 覆盖机制之后的下一项
> 创建于 2026-06-22
> 合并自 `async-io-promise.md` + `async-io-implementation-plan.md`

## 背景与目标

### 当前问题
KWCC 缺乏网络请求能力，无法对接外部 API。需要引入轻量、非阻塞的异步 HTTP 支持。

### 设计约束
1. **零外部依赖**：不引入 `libcurl` 库，通过独立 `kwcc_curl` 可执行文件解决生命周期隔离
2. **无多线程**：纯单线程 Reactor 模式
3. **ES5 兼容**：遵守 mquickjs 语法约束（无 let/const/箭头函数）
4. **Promise 风格**：优雅链式调用，拒绝回调地狱

### 核心思路
**Single-Threaded Reactor Pattern**：用 `select()` 轮询非阻塞 pipe（来自独立 `kwcc_curl` 进程），在 C 层完整读取响应后一次性 dispatch 到 JS 层，由 `MiniPromise` 封装为 `.then()` 链式调用。

---

## 架构分层

```
┌─────────────────────────────────────────────────────────┐
│ Layer 4: JS $http 对象 + MiniPromise (runtime/http.js)  │
│   $http.request/cancel/config + fetchAsync + MiniPromise│
├─────────────────────────────────────────────────────────┤
│ Layer 3: Sokol frame hook (src/main.m)                  │
│   frame() → kwcc_io_poll_once() → kwcc_process_js → ... │
├─────────────────────────────────────────────────────────┤
│ Layer 2: HTTP Process Engine (src/kwcc_http.c)          │
│   fork + pipe + kwcc_curl + O_NONBLOCK + picohttpparser │
├─────────────────────────────────────────────────────────┤
│ Layer 1: I/O Reactor (src/kwcc_io.c)                    │
│   select() + FD array (max 128) + per-frame polling     │
└─────────────────────────────────────────────────────────┘
```

---

## Layer 1: I/O Reactor（`src/kwcc_io.h` / `kwcc_io.c`）

### 状态：✅ 已完成

### 职责
纯 POSIX 文件描述符管理器，维护非阻塞 FD 列表，提供 `select()` 零超时轮询。

### 数据结构
```c
#define KWCC_IO_MAX_FDS 128

typedef void (*kwcc_io_callback_t)(int fd, void *user_data);

typedef struct {
    int                fd;
    kwcc_io_callback_t callback;
    void              *user_data;
    int                in_use;
} kwcc_io_slot_t;
```

### API
```c
void kwcc_io_init(void);
void kwcc_io_register(int fd, kwcc_io_callback_t cb, void *user_data);
void kwcc_io_unregister(int fd);
void kwcc_io_poll_once(void);  /* 每帧调用，timeout=0 非阻塞 */
```

### 实现要点
1. `select()` 用 `timeout = {0, 0}`，不阻塞渲染
2. **Fix 7：EINTR 保护**：`select()` 返回 `-1` 且 `errno == EINTR` 时直接 `return`
3. `read()` 返回 > 0 时：追加到 `kwcc_http` 侧的 C buffer，**不触发 JS**
4. `read()` 返回 0（pipe closed）：标记 stream end，触发 `ON_END`
5. `read()` 返回 -1：`EAGAIN`/`EWOULDBLOCK` 正常，其他错误触发 `ON_ERROR`

---

## Layer 2: HTTP Process Engine（`src/kwcc_http.h` / `kwcc_http.c`）

### 状态：❌ 未实现

### 职责
fork 独立 `kwcc_curl` 子进程，建立非阻塞 pipe，构造 curl 参数，响应解析。

### 模块独立性

`kwcc_http` 是独立 C 模块，**不依赖 mquickjs / JSContext**：

```
kwcc_http.c 依赖：
├── kwcc_io.c/h   — FD 管理（select + 非阻塞 read）
├── kwcc_bus.c/h  — 事件发射（NORMAL + LIGHT 双模式，零业务耦合）
├── picohttpparser — HTTP 响应解析
└── libc（malloc/free/fork/exec/waitpid）

kwcc_http.c 不依赖：
├── mquickjs / JSContext / JSValue
├── kwcc_js.c
└── microui / NanoVG / Sokol
```

这保证了：
1. **独立编译**：`kwcc_http.o` 不链接任何 JS 相关代码
2. **独立测试**：可写纯 C 测试 mock `kwcc_bus_emit` 回调，验证 HTTP 解析逻辑
3. **可替换**：未来 `kwcc_curl` 可替换为自研 socket 实现，接口不变

### 配置管理

HTTP 配置走现有 config 层（`kwcc_config.c`）：

```c
/* kwcc_http.c 内部读取，带默认值 */
const char *bin_path   = kwcc_config_get_core("http/bin_path", "curl");
const char *parser_mode = kwcc_config_get_core("http/parser_mode", "raw");
int timeout             = atoi(kwcc_config_get_core("http/timeout", "30"));
```

JS 侧可选通过 `$config` 覆盖：
```javascript
$config.coreSetTlv("http/bin_path", "/usr/local/bin/curl");
$config.coreSetTlv("http/timeout", "60");
```

### 响应缓冲区：L7 内存池

HTTP 响应大小不定（KB~MB），使用 L7 动态内存池（`kwcc_mempool_alloc_dynamic`）替代 `malloc/realloc`：

```c
/* 响应缓冲 — L7 动态分配 */
req->response_buf = kwcc_mempool_alloc_dynamic(req->req_id, KWCC_HTTP_INIT_CAP, 0);
/* 扩容时 */
req->response_buf = kwcc_mempool_alloc_dynamic(req->req_id, new_cap, 0);
/* 请求结束时释放 */
kwcc_mempool_release(req->response_slot);
```

好处：
- 自动 GC 回收，不需要手动 `free`
- 统一内存管理，可 dump / 统计
- key = `req_id`，方便按请求追踪
```c
#define KWCC_HTTP_MAX_REQS 8
#define KWCC_HTTP_INIT_CAP 4096  // 初始缓冲 4KB，按需 realloc

typedef struct {
    char     req_id[64];      // 唯一标识
    char     method[16];      // GET/POST/PUT/DELETE
    char     url[1024];
    char    *body;            // 请求体（POST），动态分配
    int      body_len;
    char    *headers[16];     // 请求头数组
    int      header_count;
    pid_t    pid;             // curl 子进程 PID
    int      pipe_read_fd;    // pipe 读端（已设 O_NONBLOCK）
    char    *response_buf;    // 动态分配的响应缓冲区（realloc）
    int      response_cap;    // response_buf 当前容量
    int      response_len;    // 已读字节数
    int      total_size;      // 从 Content-Length 解析的总大小（0=未知）
    int      http_status;     // HTTP 状态码
    int      last_dispatched; // 上次 dispatch 进度时的已读字节数
    int      in_use;
} kwcc_http_req_t;
```

### req_id 管理

**req_id 由 kwcc_http 统一生成**，调用方不需要管：

```c
/* kwcc_http_request 返回生成的 req_id（内部递增计数器）*/
const char *kwcc_http_request(const char *method,
                              const char *url, const char **headers, int header_count,
                              const char *body, int body_len);
```

生成规则：`"req_<seq>"`，seq 为内部递增计数器，循环复用。

### API
```c
void        kwcc_http_init(void);
const char *kwcc_http_request(const char *method,
                              const char *url, const char **headers, int header_count,
                              const char *body, int body_len);  /* 返回 req_id */
void        kwcc_http_cancel(const char *req_id);
void        kwcc_http_set_config(const char *key, const char *value);  /* 可选 */
```

### 依赖：picohttpparser

**状态**：✅ 已下载到 `deps/picohttpparser/`

**使用**：`phr_parse_response()` 增量解析 HTTP 响应头：
- 返回 `-2`：数据不完整，继续累积
- 返回 `-1`：HTTP 协议错误
- 返回 `> 0`：header 解析成功，返回值 = body 起始偏移

### 配置管理

C 层默认值，不依赖外部 config 系统：
```c
static const char *g_kwcc_http_bin_path = "curl";
static const char *g_kwcc_http_parser_mode = "raw";  /* "raw" or "body" */
static int g_kwcc_http_timeout = 30;
```

JS 侧可选通过 `$config` 覆盖：
```javascript
$config.coreSetTlv("http/bin_path", "/usr/local/bin/curl");
$config.coreSetTlv("http/timeout", "60");
```

### kwcc_http_request() 流程

```
1. 查找空闲槽位，复制 req_id / method / url
2. 读取配置（C 默认值）
3. bin_path 可执行检测：access(bin_path, X_OK) == -1 → 报错并 cleanup
4. 构建 curl argv: "-s", "-L", "-i", "-X", method, "-H"..., "-d", body, url
5. pipe() → fcntl(F_SETFD, FD_CLOEXEC) → fork()
6. 子进程：close(pipefd[0]) → 关闭 > STDERR_FILENO 的所有 FD
           → dup2(pipefd[1], STDOUT_FILENO) → execvp(bin_path, argv) → _exit(1)
7. 父进程：close(pipefd[1]) → fcntl(O_NONBLOCK) → malloc(response_buf)
           → kwcc_io_register(pipe_read_fd, kwcc_http_on_read, req)
```

**Fix 10：子进程 FD 污染防护**：
```c
/* 子进程 */
close(pipefd[0]);
for (int fd = STDERR_FILENO + 1; fd < sysconf(_SC_OPEN_MAX); fd++) {
    if (fd != pipefd[1]) close(fd);
}
dup2(pipefd[1], STDOUT_FILENO);
close(pipefd[1]);
execvp(bin_path, argv);
_exit(1);
```

### kwcc_http_on_read() 回调

```c
static void kwcc_http_on_read(int fd, void *user_data) {
    kwcc_http_req_t *req = (kwcc_http_req_t *)user_data;
    char buf[4096];
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        /* Fix 1: realloc +4 字节 + trailing '\0' */
        if (req->response_len + n + 4 > req->response_cap) {
            req->response_cap = (req->response_len + n + 4) * 2;
            req->response_buf = realloc(req->response_buf, req->response_cap);
        }
        memcpy(req->response_buf + req->response_len, buf, n);
        req->response_len += n;
        req->response_buf[req->response_len] = '\0';
    }

    if (n == 0) {
        /* EOF: curl 退出，解析响应 */
        const char *p = req->response_buf;
        size_t remaining = req->response_len;
        int ret = 0;
        int final_status = 0;
        struct phr_header final_headers[64];
        size_t final_num_headers = 64;

        /* Fix 4: 循环解析跳过中间响应（302/301 重定向）*/
        while (remaining >= 9 && memcmp(p, "HTTP/1.", 7) == 0) {
            size_t nh = 64;
            int mv, st;
            const char *msg;
            size_t msg_len;
            int r = phr_parse_response(p, remaining, &mv, &st, &msg, &msg_len,
                                       final_headers, &nh, 0);
            if (r == -2) break;   /* Fix 8: 数据不完整，跳出 */
            if (r <= 0) break;    /* -1 = 协议错误 */
            ret += r;
            final_status = st;
            final_num_headers = nh;
            p += r;
            remaining -= r;
        }

        kwcc_http_dispatch_end(req, 0, final_status, p, (int)remaining,
                                final_headers, final_num_headers);
        kwcc_http_cleanup(req);
        return;
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        kwcc_http_dispatch_end(req, 1, 0, NULL, 0, NULL, 0);
        kwcc_http_cleanup(req);
    }
}
```

### kwcc_http_dispatch_end() — C→JS 通信

**机制**：topic 由 HTTP 模块内部管理（动态 `http/end/<req_id>`），JS 侧无感知。

```c
static void kwcc_http_dispatch_end(kwcc_http_req_t *req, int error,
                                   int status, const char *body, int body_len,
                                   struct phr_header *headers, size_t num_headers) {
    /* 动态 topic，按 req_id 路由，确保每个请求只触发自己的 handler */
    char topic[64];
    snprintf(topic, sizeof(topic), error ? "http/error/%s" : "http/end/%s", req->req_id);

    kwcc_bus_emit(topic, NULL, 0);  /* JS 桥接层收到 topic 后，从回调注册表查找对应 handler */
}
```

**topic 设计**：
- `http/end/<req_id>` — 每个请求的响应走独立 topic
- `http/progress/<req_id>` — 每个请求的进度走独立 topic
- JS 桥接层收到 `http/end/req_1` 后，从 req_id 回调注册表取出对应 resolve/reject
- `*` 通配仍可收所有事件：`kwcc_bus_subscribe("*", log_all)`

### JS→C 通信：代理机制

C handler 注册到 `g_kwcc_js_cfun_handlers[]` 代理表，不需要改 `mqjs_stdlib.c`：

```c
/* kwcc_js.c — 代理表 */
static kwcc_js_cfun_entry_t g_kwcc_js_cfun_handlers[] = {
    { "kwcc_js_mempool_dump_stats", kwcc_js_mempool_dump_stats },
    { "kwcc_js_mempool_dump_all",   kwcc_js_mempool_dump_all },
    { "kwcc_js_http_request",       kwcc_js_http_request },  /* HTTP handler */
    { "kwcc_js_http_cancel",        kwcc_js_http_cancel },   /* HTTP cancel */
    { NULL, NULL }
};
```

C handler 实现（`kwcc_js.c`）：
```c
JSValue kwcc_js_http_request(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_http_cancel(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
```

### kwcc_http_cleanup()

```c
static void kwcc_http_cleanup(kwcc_http_req_t *req) {
    if (req->pipe_read_fd >= 0) {
        close(req->pipe_read_fd);
        req->pipe_read_fd = -1;
    }
    kwcc_io_unregister(req->pipe_read_fd);
    waitpid(req->pid, NULL, WNOHANG);  /* 非阻塞收割，防僵尸进程 */
    free(req->response_buf);
    free(req->body);
    for (int i = 0; i < req->header_count; i++) free(req->headers[i]);
    memset(req, 0, sizeof(kwcc_http_req_t));
}
```

### 进度事件（ON_PROGRESS，按帧限频）

每帧检查 `req->response_len != req->last_dispatched`，有变化则通过 bus 发射动态 topic：

```c
char topic[64];
snprintf(topic, sizeof(topic), "http/progress/%s", req->req_id);
kwcc_bus_emit(topic, NULL, 0);
```

60fps 下每秒最多 60 次 dispatch，JS 桥接层收到 topic 后从 req_id 回调注册表路由到对应 handler。

---

## Layer 3: Sokol Frame Hook

在 `src/main.m` 的 `frame()` 中插入：

```c
static void frame(void) {
    /* 1. I/O reactor polling (non-blocking) */
    kwcc_io_poll_once();

    /* 2. 原有 JS 处理 + microui 渲染 */
    kwcc_process_js(g_js_ctx, "onFrame();");

    /* 3. 原有 NanoVG 渲染 */
    render_mu_commands();
}
```

**位置**：`kwcc_process_js` 之前，确保当帧的 pipe 数据在 JS 处理前被读取。

---

## Layer 4: JS $http 对象 + MiniPromise

### $http 命名空间

`$http` 是一个对象，提供多个方法和属性：

```javascript
$http.request(url, options)            // 发起 HTTP 请求，返回 Promise
$http.cancel(reqId)                     // 取消请求
$http.config(key, value)                // 设置配置项
$http.state = { activeRequests: 0 }    // 运行状态
```

**注意**：`$http.request` 不再需要 reqId 参数，req_id 由 C 侧 kwcc_http 生成并返回。

### 注入方式

在 `kwcc_create_js()` 中通过 `JS_Eval` 注入 global object：

```c
/* kwcc_js.c — kwcc_register_http_js() */
void kwcc_register_http_js(JSContext *ctx) {
    const char *code =
        "var $http = new Object();\n"
        "$http.request = function(reqId, url, options) {\n"
        "    var method = 'GET';\n"
        "    var headers = [];\n"
        "    var body = '';\n"
        "    var onProgress = null;\n"
        "    if (options) {\n"
        "        if (options.method) method = options.method;\n"
        "        if (options.headers) headers = options.headers;\n"
        "        if (options.body) body = options.body;\n"
        "        if (options.onProgress) onProgress = options.onProgress;\n"
        "    }\n"
        "    return $http._fetchAsync(reqId, method, url, headers, body, onProgress);\n"
        "};\n"
        "$http.cancel = function(reqId) {\n"
        "    kwcc_js_mquickjs_call('_http_cancel', reqId);\n"
        "};\n"
        "$http.config = function(key, value) {\n"
        "    kwcc_js_mquickjs_call('_http_config', key, value);\n"
        "};\n"
        "$http.state = { activeRequests: 0 };\n"
    ;
    JS_Eval(ctx, code, strlen(code), "<http>", JS_EVAL_REPL);

    /* 注入内部实现 */
    const char *internal =
        "$http._fetchAsync = function(reqId, method, url, headers, body, onProgress) {\n"
        "    /* MiniPromise 实现见下方 */\n"
        "};\n"
    ;
    JS_Eval(ctx, internal, strlen(internal), "<http_internal>", JS_EVAL_REPL);
}
```

### MiniPromise 实现（ES5 兼容）

```javascript
function MiniPromise(executor) {
    var self = this;
    self.status = "PENDING";
    self.value = null;
    self.callbacks = [];

    executor(function(val) {
        if (self.status === "PENDING") {
            self.status = "FULFILLED";
            self.value = val;
            for (var i = 0; i < self.callbacks.length; i++) {
                self.callbacks[i].fn(val);
            }
        }
    }, function(err) {
        if (self.status === "PENDING") {
            self.status = "REJECTED";
            self.value = err;
            for (var i = 0; i < self.callbacks.length; i++) {
                var cb = self.callbacks[i];
                if (cb.reject) cb.reject(err);
            }
        }
    });
}

MiniPromise.prototype.then = function(onFulfilled, onRejected) {
    var self = this;
    return new MiniPromise(function(resolve, reject) {
        function wrapped(val) {
            try {
                var result = onFulfilled ? onFulfilled(val) : val;
                if (result && typeof result.then === "function") {
                    result.then(resolve, reject);
                } else {
                    resolve(result);
                }
            } catch (e) {
                reject(e);
            }
        }
        if (self.status === "FULFILLED") {
            wrapped(self.value);
        } else {
            self.callbacks.push({ fn: wrapped, reject: reject });
        }
    });
};

MiniPromise.prototype.catch = function(onRejected) {
    return this.then(null, onRejected);
};
```

### $http._fetchAsync 内部实现

**设计原则**：JS 侧不需要知道 topic，topic 由 C 侧 HTTP 模块管理。req_id 由 C 侧生成。

```javascript
$http._fetchAsync = function(method, url, headers, body, onProgress) {
    return new MiniPromise(function(resolve, reject) {
        /* C handler 返回生成的 req_id */
        var reqId = kwcc_js_mquickjs_call("_http_request", method, url, headers, body);

        /* C 侧通过 kwcc_bus_emit("http/end/<reqId>", ...) 发射事件 */
        /* JS 桥接层收到后，从 req_id 回调注册表中取出对应 resolve/reject */

        var onEnd = function(action, data) {
            if (data.reqId !== reqId) return;
            $http._removeCallback(reqId);
            if (data.body) data.body = null;
            if (data.headers) data.headers = null;
            resolve(data);
        };

        var onError = function(action, data) {
            if (data.reqId !== reqId) return;
            $http._removeCallback(reqId);
            reject(data.error);
        };

        var onProgressFiltered = function(action, data) {
            if (data.reqId !== reqId) return;
            onProgress(data.loaded, data.total);
        };

        /* 注册回调到 req_id 注册表（C 侧通过动态 topic 路由到正确 handler）*/
        $http._addCallback(reqId, "http/end", onEnd);
        $http._addCallback(reqId, "http/error", onError);
        if (onProgress) $http._addCallback(reqId, "http/progress", onProgressFiltered);

        $http.state.activeRequests++;
    });
};
```
```

**C 侧 req_id 回调注册表**（在 `kwcc_js.c` 中维护）：

```c
/* kwcc_js.c — req_id → JS callback 映射（一个大 map）*/
#define KWCC_HTTP_CB_MAX 8
static struct {
    char     req_id[64];
    JSValue  on_end_cb;    /* resolve callback */
    JSValue  on_error_cb;  /* reject callback */
    JSValue  on_progress_cb;
    int      in_use;
} g_kwcc_http_cbs[KWCC_HTTP_CB_MAX];

void kwcc_js_http_add_cb(const char *req_id, JSValue on_end, JSValue on_error, JSValue on_progress);
void kwcc_js_http_remove_cb(const char *req_id);
void kwcc_js_http_on_bus_event(const char *topic, const void *data, size_t len);
```

**req_id map 超时清理**：如果请求超时或取消，map 里的回调条目需要清理，防止泄漏。

```c
/* kwcc_http_cleanup 中发射超时/取消事件 */
char topic[64];
snprintf(topic, sizeof(topic), "http/cancel/%s", req->req_id);
kwcc_bus_emit(topic, NULL, 0);  /* JS 桥接收到后清理 map 条目 */
```

当 bus 发射 `http/end/req_1` 时，JS 桥接层从 topic 中提取 req_id，查注册表，调用对应的 resolve callback。收到 `http/cancel/req_1` 或 `http/timeout/req_1` 时同样清理 map 条目并 reject Promise。

### 业务代码调用示例

```javascript
// 简单请求
$http.request("https://httpbin.org/get").then(function(resp) {
    ui.label("Response: " + resp.body);
}).catch(function(err) {
    ui.label("Error: " + err);
});

// 带进度条的 POST
$http.request("https://example.com/upload", {
    method: "POST",
    headers: ["Content-Type: application/json"],
    body: '{"data":"test"}',
    onProgress: function(loaded, total) {
        if (total > 0) {
            ui.label("Progress: " + (loaded * 100 / total) + "%");
        } else {
            ui.label("Loading: " + loaded + " bytes");
        }
    }
}).then(function(resp) {
    ui.label("Upload complete! Status: " + resp.status);
}).catch(function(err) {
    ui.label("Upload failed: " + err);
});

// 取消请求（需要保留 reqId 用于 cancel）
var reqId = $http.request("https://example.com/slow", { ... });
$http.cancel(reqId);
```

---

## 待论证问题

### 1. kwcc_curl 的编译与分发

| 问题 | 方案 |
|------|------|
| curl 源码是否引入 `deps/`？ | 是，从 `https://curl.se/download/curl-*.tar.gz` 下载 |
| 编译产物放哪？ | 项目根目录或 `bin/` 目录 |
| macOS 签名/沙箱影响？ | 待验证：fork/exec 在 macOS 默认环境下是否正常 |

### 2. picohttpparser 集成

| 问题 | 状态 |
|------|------|
| 是否引入 `deps/picohttpparser/`？ | ✅ 已完成 |
| setup.sh 需要改动？ | ✅ 已完成 |
| Makefile 需要改动？ | ✅ 已完成（picohttpparser.o 已在 DEP_SRCS 中） |

### 3. 超时控制

curl 自带 `--max-time` 参数，C 层不需要额外处理。

### 4. HTTPS 支持

依赖 curl 是否支持。系统 curl 通常支持。

### 5. 并发请求数

`KWCC_IO_MAX_FDS = 128`，`KWCC_HTTP_MAX_REQS = 8`，最多 8 个并发。UI 场景通常 1-3 个并发，8 个足够。

### 6. Bus 事件 vs Store dispatch

用 `$bus.emit`：HTTP 响应不是应用 state，不应混入 store。业务代码通过 `$http.request()` 封装消费。

---

## 前置依赖：kwcc_bus 拆分

在实施 Step 3 之前，需要先完成 `kwcc_bus` 的拆分（详见 `requirements/bus-split-design.md`）：

1. **kwcc_bus.c 重写为 topic 属性 map + NORMAL 链表** — `kwcc_bus_register_topic/subscribe/unsubscribe/emit`，零业务耦合
2. **topic map 移到 kwcc_ui.c** — microui 专用，不再在 bus 中
3. **kwcc_js.c 新增 `kwcc_js_on_bus_event`** — 作为 bus consumer 注册回调，不耦合到 bus
4. **现有调用点替换** — `kwcc_dispatch_event` → `kwcc_bus_emit`

**Topic 规范**（详见 bus-split-design.md）：
- C bus topic：`ui/calc/btn0`, `http/end/req_1`（LIGHT / NORMAL）
- JS bus topic：JS 侧独立，通过桥接层连通
- 匹配规则：精确 / `*` 通配 / 前缀匹配

完成后再实施 Step 3（HTTP Process Engine）。

---

## 实施步骤

按 4 层架构，分 5 步实施。每步完成后验证编译通过。

| Step | 内容 | 新增/修改文件 | 依赖 |
|------|------|--------------|------|
| 1 | picohttpparser 编译验证 | 无（已有） | 无 |
| 2 | I/O Reactor（Layer 1） | `src/kwcc_io.h/c`（已完成） | 无 |
| 3 | HTTP Process Engine（Layer 2） | `src/kwcc_http.h`, `src/kwcc_http.c` | Step 2（bus 拆分） |
| 4 | C binding + JS API + Frame Hook | `src/kwcc_js.c`, `src/kwcc_js.h`, `src/main.m` | Step 3 |
| 5 | JS $http 对象 + MiniPromise | `app/runtime/http.js`, `app/main.js` | Step 4 |

### Step 1: picohttpparser 编译验证 ✅

`deps/picohttpparser/picohttpparser.c/h` 存在，Makefile 已包含。`make clean && make` 验证即可。

### Step 2: I/O Reactor（Layer 1）✅

`src/kwcc_io.h/c` 已实现。`KWCC_IO_MAX_FDS = 128`，API 完全匹配方案。

### Step 3: HTTP Process Engine（Layer 2）

新建 `src/kwcc_http.h` + `src/kwcc_http.c`，实现 fork + pipe + curl + picohttpparser 解析 + dispatch。

新增 JS 桥接回调到 `src/kwcc_js.c`。

### Step 4: C binding + JS API + Frame Hook

- `src/kwcc_js.c`：新增 `kwcc_js_http_request` / `kwcc_js_http_cancel` 到代理表
- `src/kwcc_js.c`：新增 `kwcc_register_http_js()` 注入 `$http` 对象
- `src/main.m`：`frame()` 中插入 `kwcc_io_poll_once()`

### Step 5: JS $http 对象 + MiniPromise

新建 `app/runtime/http.js`，包含 MiniPromise 实现和 `$http` 对象。`app/main.js` 加载。

**ES5 语法约束**：
- 只用 `var`，不用 `let/const`
- 只用 `function() {}`，不用箭头函数
- 用 `new Object()` 代替 `{}` 在语句开头
- 不用 `...rest` 参数

---

## 风险点

| 风险 | 影响 | 缓解 |
|------|------|------|
| `curl` 不存在 | 无法发起 HTTP 请求 | 先用 `which curl` 检测，报明确错误 |
| macOS fork/exec 权限 | 子进程可能无法启动 | 先用简单 fork 测试验证 |
| picohttpparser 内存扫描 | Fix 1 的 +4 已覆盖 | 已有 trailing `\0` 保护 |
| mquickjs API 不匹配 | 编译错误或运行时崩溃 | 严格参照 `mquickjs_c_api.md` |
| select() 被信号中断 | Fix 7 EINTR 保护 | `if (errno == EINTR) return;` |
| 不完整 header 卡住主线程 | Fix 8 incomplete 保护 | `if (r == -2) break;` |
| 子进程继承父进程 FD | Fix 10 FD_CLOEXEC + 关闭循环 | 子进程 execvp 前关闭 > STDERR_FILENO 的所有 FD |

---

## Defensive Fixes 清单

| # | 名称 | 层 | 说明 |
|---|------|-----|------|
| 1 | picohttpparser Buffer Margin | Layer 2 | `realloc +4` + trailing `\0` |
| 2 | Ban JS_Eval string injection | Layer 2→4 | C API 构建对象，不拼接用户数据 |
| 3 | Follow-Redirects | Layer 2 | curl `-L` 参数 |
| 4 | Redirect Loop Parsing | Layer 2 | EOF 后循环 `phr_parse_response` |
| 5 | Zombie Process Cleanup | Layer 2 | `kwcc_http_cleanup()` + `waitpid(WNOHANG)` |
| 6 | bin_path Executable Check | Layer 2 | `access(bin_path, X_OK)` |
| 7 | select() EINTR Protection | Layer 1 | `if (errno == EINTR) return;`（已实现） |
| 8 | Incomplete Header Loop Guard | Layer 2 | `if (r == -2) break;` |
| 9 | Global Shape Table Leak | Layer 4 | `js_bus_dispatch_obj`（内部）不用全局变量 |
| 10 | FD Pollution via fork | Layer 2 | `FD_CLOEXEC` + 关闭 > STDERR_FILENO 的所有 FD |
| 11 | Heap Fragmentation Guard | Layer 4 | 显式 nullify body 再 delete |
