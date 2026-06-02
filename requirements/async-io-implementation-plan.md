# Async I/O 实施计划

> 基于已确认的方案：`requirements/async-io-promise.md`（含 11 个 Defensive Fixes）
> 状态：进行中 — Step 1/2/3 已完成 ✅

---

## 实施总览

按方案文档的 4 层架构，分 7 步实施。每步完成后验证编译通过。

| Step | 内容 | 新增/修改文件 | 依赖 |
|------|------|--------------|------|
| 1 | picohttpparser 编译验证 | Makefile（已有，无需改动） | 无 |
| 2 | I/O Reactor（Layer 1） | `src/kwcc_io.h`, `src/kwcc_io.c` | 无 |
| 3 | **Config 系统**（C↔JS 配置管理基础设施） | `src/kwcc.c`, `src/kwcc.h`, `src/jsapi.c`, `src/jsapi.h` | 无 |
| 4 | HTTP Process Engine（Layer 2） | `src/kwcc_http.h`, `src/kwcc_http.c` | Step 2, 3 |
| 5 | Sokol Frame Hook（Layer 3） | `src/main.m` | Step 2, 4 |
| 6 | C binding + JS API | `src/jsapi.h`, `src/jsapi.c`, `src/kwcc.c` | Step 4 |
| 7 | JS Promise + http module（Layer 4） | `app/runtime/http.js`, `app/main.js` | Step 6 |

---

## Step 1: picohttpparser 编译验证

**目的**：确认已有的 `deps/picohttpparser/` 能正确编译进项目。

**检查项**：
- `Makefile` 已有 `deps/picohttpparser/picohttpparser.c` 在 `DEP_SRCS` 中 ✅（已有）
- `Makefile` 已有对应的编译规则 ✅（已有）
- `setup.sh` 已有下载步骤 ✅（已有）

**验证**：`make clean && make` 编译通过即可。

**预期结果**：编译通过，无需改动任何文件。

---

## Step 2: I/O Reactor（Layer 1）

**文件**：`src/kwcc_io.h`（新建）、`src/kwcc_io.c`（新建）

**设计依据**：方案文档 "Layer 1: I/O Reactor" 章节

### `src/kwcc_io.h`

```c
#ifndef KWCC_IO_H
#define KWCC_IO_H

#define KWCC_IO_MAX_FDS 16

typedef void (*kwcc_io_callback_t)(int fd, void *user_data);

typedef struct {
    int                fd;
    kwcc_io_callback_t callback;
    void              *user_data;
    int                in_use;
} kwcc_io_slot_t;

void kwcc_io_init(void);
void kwcc_io_register(int fd, kwcc_io_callback_t cb, void *user_data);
void kwcc_io_unregister(int fd);
void kwcc_io_poll_once(void);  /* 每帧调用，timeout=0 非阻塞 */

#endif
```

### `src/kwcc_io.c` 实现要点

1. 全局槽位数组 `g_io_slots[KWCC_IO_MAX_FDS]`
2. `kwcc_io_init()`：全部槽位 `in_use = 0`
3. `kwcc_io_register()`：找空闲槽位，存 fd/callback/user_data
4. `kwcc_io_unregister()`：设 `in_use = 0`，close fd（由调用方负责关闭，reactor 只做解绑）
5. `kwcc_io_poll_once()`：
   - 构建 `fd_set`，收集所有 `in_use` 的 fd
   - `select(max_fd+1, &readfds, NULL, NULL, &zero_timeout)`
   - **Fix 7：EINTR 保护**：`select()` 返回 `-1` 且 `errno == EINTR` 时，说明被 OS 信号中断（窗口 resize、SIGCHLD 等），直接 `return`：
     ```c
     int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
     if (ret < 0) {
         if (errno == EINTR) return;  /* 信号中断，下帧继续 */
         return;
     }
     if (ret == 0) return;  /* 超时，无数据 */
     ```
   - 对有数据的 fd 调用 `read()`：
     - `read() > 0`：回调，传递数据
     - `read() == 0`：pipe closed，标记 EOF，回调
     - `read() == -1 && EAGAIN/EWOULDBLOCK`：正常，无数据
     - `read() == -1 && other`：错误，回调

**关键设计决策**：
- `select()` timeout 用 `{0, 0}`，不阻塞渲染主线程
- `read()` 的 buffer 由回调方（HTTP 层）管理，reactor 不持有
- Reactor 是纯 POSIX FD 管理器，不依赖任何 HTTP/JS 知识

**验证**：`make` 编译通过

---

## Step 3: Config 系统（C↔JS 配置管理）

**文件**：`src/kwcc.h`（修改）、`src/kwcc.c`（修改）、`src/jsapi.h`（修改）、`src/jsapi.c`（修改）

### 方案文档定义的 API

```javascript
kwcc_config("http", {
    bin_path: "curl",
    parser_mode: "raw",
    chunked_support: 0,
    timeout: 30,
});
```

### 实现设计

#### 3a. `src/kwcc.h` — 添加 config API 声明

```c
/* Config system */
#define KWCC_CONFIG_MAX_MODULES 16
#define KWCC_CONFIG_MAX_KEY_LEN 64
#define KWCC_CONFIG_MAX_VALUE_LEN 256

typedef struct {
    char key[KWCC_CONFIG_MAX_KEY_LEN];
    char value[KWCC_CONFIG_MAX_VALUE_LEN];
} kwcc_config_entry_t;

typedef struct {
    char module[KWCC_CONFIG_MAX_KEY_LEN];
    kwcc_config_entry_t entries[16];
    int entry_count;
    int in_use;
} kwcc_config_module_t;

void kwcc_config_set(const char *module, const char *key, const char *value);
const char *kwcc_config_get(const char *module, const char *key, const char *default_value);
```

#### 3b. `src/kwcc.c` — 实现 config 存储

- 全局数组 `g_config_modules[KWCC_CONFIG_MAX_MODULES]`
- `kwcc_config_set(module, key, value)`：查找/创建模块槽位，写入 key-value
- `kwcc_config_get(module, key, default_value)`：查找模块→查找 key→返回 value，找不到返回 default

#### 3c. `src/jsapi.c` — 添加 `js_kwcc_config` C binding

#### 3d. JS wrapper（`methods_js` 字符串）

```javascript
kwcc_config = function(module, options) {
    var keys = Object.keys(options);
    for (var i = 0; i < keys.length; i++) {
        var key = keys[i];
        _native_config_set(module, key, options[key]);
    }
};
```

**验证**：`make` 编译通过

---

## Step 4: HTTP Process Engine（Layer 2）

**文件**：`src/kwcc_http.h`（新建）、`src/kwcc_http.c`（新建）

**设计依据**：方案文档 "Layer 2: HTTP Process Engine" 章节 + 11 个 Defensive Fixes

### `src/kwcc_http.h`

```c
#ifndef KWCC_HTTP_H
#define KWCC_HTTP_H

#include <sys/types.h>

#define KWCC_HTTP_MAX_REQS 8
#define KWCC_HTTP_INIT_CAP 4096

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

void kwcc_http_init(void);
void kwcc_http_request(const char *req_id, const char *method,
                       const char *url, const char **headers, int header_count,
                       const char *body, int body_len);
void kwcc_http_cancel(const char *req_id);
void kwcc_http_poll(void);

#endif
```

### `src/kwcc_http.c` 核心实现

#### 1. 全局状态

```c
static kwcc_http_req_t g_http_reqs[KWCC_HTTP_MAX_REQS];
```

#### 2. `kwcc_http_request()` 内部流程

```
1. 查找空闲槽位，复制 req_id / method / url
2. 读取 http config（bin_path, parser_mode, timeout）
3. bin_path 可执行检测：access(bin_path, X_OK) == -1 → 报错并 cleanup
4. 构建 curl argv: "-s", "-L", "-i", "-X", method, "-H"..., "-d", body, url
5. pipe() → fcntl(F_SETFD, FD_CLOEXEC) on pipefd[0]（Fix 10）→ fork()
6. 子进程（fork == 0）：
   a. 关闭所有无关继承的 FD（Fix 10）：close(pipefd[0]) + 关闭父进程所有其他 pipe FD
   b. dup2(pipefd[1], STDOUT_FILENO) → close(pipefd[1])
   c. execvp(bin_path, argv)
   d. execvp 失败 → _exit(1)
7. 父进程：close(pipefd[1]) → fcntl(O_NONBLOCK) → malloc(response_buf)
           → kwcc_io_register(pipe_read_fd, http_on_read, req)
```

**Fix 3**：始终添加 `-L` 参数跟随重定向

**Fix 6：bin_path 可执行检测**：

```c
const char *bin_path = kwcc_config_get("http", "bin_path", "curl");
if (access(bin_path, X_OK) == -1) {
    char errmsg[256];
    snprintf(errmsg, sizeof(errmsg),
             "http: bin_path '%s' not found or not executable", bin_path);
    http_dispatch_end(req, 1, 0, errmsg, (int)strlen(errmsg), NULL, 0);
    kwcc_http_cleanup(req);
    return;
}
```

**Fix 10：子进程 FD 污染防护** — `fork()` 后子进程继承父进程所有 FD。如果不关闭，子进程持有父进程的 socket/pipe FD，导致资源死锁：

```c
if (pid == 0) {
    /* 子进程 */

    /* Fix 10a: pipe 创建时已设 FD_CLOEXEC（pipefd[0] 读端） */
    /* 子进程只需关闭 pipe 读端（不需要读取） */
    close(pipefd[0]);

    /* Fix 10b: 关闭其他所有可能继承的 FD（遍历 g_io_slots 解绑的 FD） */
    /* 最安全做法：关闭 > STDERR_FILENO 的所有 FD */
    for (int fd = STDERR_FILENO + 1; fd < sysconf(_SC_OPEN_MAX); fd++) {
        if (fd != pipefd[1]) {  /* 不要关闭待 dup2 的写端 */
            close(fd);
        }
    }

    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    execvp(bin_path, argv);
    _exit(1);  /* execvp 失败 */
}
```

#### 3. `http_on_read()` 回调（核心解析逻辑）

```c
static void http_on_read(int fd, void *user_data) {
    kwcc_http_req_t *req = (kwcc_http_req_t *)user_data;
    char buf[4096];
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        /* Fix 1: realloc +4 字节 + trailing \0 */
        if (req->response_len + n + 4 > req->response_cap) {
            req->response_cap = (req->response_len + n + 4) * 2;
            req->response_buf = realloc(req->response_buf, req->response_cap);
        }
        memcpy(req->response_buf + req->response_len, buf, n);
        req->response_len += n;
        req->response_buf[req->response_len] = '\0';
    }

    if (n == 0) {
        /* EOF: pipe closed, curl 退出 */
        /* Fix 4: 循环解析跳过中间响应 */
        const char *p = req->response_buf;
        size_t remaining = req->response_len;
        int ret = 0;
        int final_status = 0;
        struct phr_header final_headers[64];
        size_t final_num_headers = 64;

        while (remaining >= 9 && memcmp(p, "HTTP/1.", 7) == 0) {
            size_t nh = 64;
            int mv, st;
            const char *msg;
            size_t msg_len;
            int r = phr_parse_response(p, remaining, &mv, &st, &msg, &msg_len,
                                       final_headers, &nh, 0);
            if (r == -2) break;  /* Fix 8: 数据不完整，跳出等待 */
            if (r <= 0) break;   /* -1 = 协议错误 */
            ret += r;
            final_status = st;
            final_num_headers = nh;
            p = req->response_buf + ret;
            remaining = req->response_len - ret;
        }

        const char *body_start = p;
        int body_len = (int)remaining;

        http_dispatch_end(req, 0, final_status, body_start, body_len,
                          final_headers, final_num_headers);
        kwcc_http_cleanup(req);
        return;
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg), "http: read error (%s)", strerror(errno));
        http_dispatch_end(req, 1, 0, errmsg, (int)strlen(errmsg), NULL, 0);
        kwcc_http_cleanup(req);
    }
}
```

**Fix 8：不完整 Header 循环保护**：`phr_parse_response` 返回 `-2` 时立即 `break`，绝不能继续循环。

#### 4. `http_dispatch_end()` — C→JS 通信

**Fix 2**：禁止 `JS_Eval` + 字符串拼接传递 body/header

**Fix 9：Global Property Shape 泄漏保护** — 用 `global.__http_registry` 容器：

```c
static void http_dispatch_end(kwcc_http_req_t *req, int error,
                               int status, const char *body, int body_len,
                               struct phr_header *headers, size_t num_headers) {
    JSContext *ctx = /* 获取全局 JS 上下文 */;
    if (!ctx) return;

    if (error) {
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "$bus.emit('http/end', { reqId:'%s', error:'%s' });",
            req->req_id, "parse error");
        JS_Eval(ctx, buf, strlen(buf), "<http_end>", 0);
        return;
    }

    /* C API 构建 JS response 对象 */
    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, resp, "body", JS_NewStringLen(ctx, body, body_len));

    JSValue headers_obj = JS_NewObject(ctx);
    for (size_t i = 0; i < num_headers; i++) {
        char *hname = strndup(headers[i].name, headers[i].name_len);
        JS_SetPropertyStr(ctx, headers_obj, hname,
            JS_NewStringLen(ctx, headers[i].value, headers[i].value_len));
        free(hname);
    }
    JS_SetPropertyStr(ctx, resp, "headers", headers_obj);

    /* Fix 9: 挂载到 __http_registry 容器 */
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue registry = JS_GetPropertyStr(ctx, global_obj, "__http_registry");
    if (JS_IsUndefined(registry)) {
        registry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global_obj, "__http_registry", registry);
    }
    JS_SetPropertyStr(ctx, registry, req->req_id, resp);

    /* Fix 11: JS 层负责先 nullify body 再 delete，触发立即 GC */
    char buf[512];
    snprintf(buf, sizeof(buf),
        "$bus.emit('http/end', new Object(), __http_registry['%s']);"
        "delete __http_registry['%s'];",
        req->req_id, req->req_id);
    JS_Eval(ctx, buf, strlen(buf), "<http>", JS_EVAL_REPL);
}
```

#### 5. `kwcc_http_cleanup()` — Fix 5

```c
static void kwcc_http_cleanup(kwcc_http_req_t *req) {
    if (req->pipe_read_fd >= 0) {
        close(req->pipe_read_fd);
        req->pipe_read_fd = -1;
    }
    kwcc_io_unregister(req->pipe_read_fd);
    waitpid(req->pid, NULL, WNOHANG);
    free(req->response_buf);
    free(req->body);
    for (int i = 0; i < req->header_count; i++) free(req->headers[i]);
    memset(req, 0, sizeof(kwcc_http_req_t));
}
```

#### 6. `kwcc_http_cancel()`

```c
void kwcc_http_cancel(const char *req_id) {
    kwcc_http_req_t *req = find_req(req_id);
    if (!req) return;
    kill(req->pid, SIGTERM);
    waitpid(req->pid, NULL, WNOHANG);
    kwcc_http_cleanup(req);
}
```

#### 7. `kwcc_http_poll()` — 进度事件

每帧检查 `req->response_len != req->last_dispatched`，有变化则 dispatch progress。

**验证**：`make` 编译通过

---

## Step 5: Sokol Frame Hook（Layer 3）

**文件**：`src/main.m`（修改）

```c
#include "kwcc_io.h"
#include "kwcc_http.h"

static void frame(void) {
    int w = sapp_width();
    int h = sapp_height();

    /* 1. I/O reactor polling (non-blocking) */
    kwcc_io_poll_once();

    /* 2. 原有 JS 处理 + microui 渲染 */
    kwcc_process_js(js_ctx, "onFrame();");

    /* ... 原有渲染代码 ... */
}
```

**验证**：`make` 编译通过，程序正常运行

---

## Step 6: C binding + JS API

**文件**：
- `src/jsapi.h`（修改：添加 HTTP binding 声明）
- `src/jsapi.c`（修改：添加 `_native_http_request` C binding 实现）
- `src/kwcc.c`（修改：初始化 HTTP + 注册 JS wrapper）

### 6a. `_native_http_request` C binding

```c
JSValue js_native_http_request(JSContext *ctx, JSValue *this_val,
                               int argc, JSValue *argv);
```

### 6b. JS wrapper

```javascript
_native_http_request = function(reqId, method, url, headers, body) {
    kwcc_http_request(reqId, method, url, headers, body);
};
```

### 6c. 初始化

在 `kwcc_init()` 或 `kwcc_create_js()` 中：
- `kwcc_io_init()`
- `kwcc_http_init()`
- 注册 `_native_http_request` 到 global object

**验证**：`make` 编译通过

---

## Step 7: JS Promise + http module（Layer 4）

**文件**：
- `app/runtime/http.js`（新建）
- `app/main.js`（修改：加载 http.js）

### 7a. `app/runtime/http.js`

包含：
1. `MiniPromise` 实现（ES5 兼容）
2. `fetchAsync(reqId, url, options)` 封装
3. `$store` 注册 http module

**ES5 语法约束**：
- 只用 `var`，不用 `let/const`
- 只用 `function() {}`，不用箭头函数
- 用 `new Object()` 代替 `{}` 在语句开头
- 不用 `...rest` 参数

**Fix 11：mquickjs 内存碎片防护** — 在 `ON_END` handler 中，先显式 nullify `body` 大字符串，再 delete 注册表项，触发立即 GC：

```javascript
var onEnd = function(action, data) {
    if (data.reqId !== reqId) return;
    /* Fix 11: 显式 nullify 大字符串，触发 GC 释放 C heap */
    if (__http_registry && __http_registry[reqId]) {
        __http_registry[reqId].body = null;
        __http_registry[reqId].headers = null;
        delete __http_registry[reqId];
    }
    cleanup();
    resolve(data.response);
};
```

**为什么需要**：mquickjs 没有 compacting GC，大量动态创建/释放大字符串会碎片化 C heap。显式 nullify body（通常是最大的属性）后再 delete，让 GC 有机会在 delete 前回收字符串内存，保护 heap 连续性。

### 7b. `app/main.js` 修改

```javascript
load("app/runtime/http.js");
```

### 7c. 测试用例

```javascript
ui.button("Test HTTP", "test/http");

$bus.on("test/http", function(action, data) {
    fetchAsync("api_test", "https://httpbin.org/get").then(function(resp) {
        print("Response: " + resp.body);
    }).catch(function(err) {
        print("Error: " + err);
    });
});
```

**验证**：`make` 编译通过，程序可运行

---

## 编译验证策略

每个 Step 完成后执行 `make` 确认编译通过。最终 `make clean && make` 完整验证。

## 风险点

| 风险 | 影响 | 缓解 |
|------|------|------|
| `kwcc_curl` 不存在 | 无法发起 HTTP 请求 | 先用系统 `curl` 命令测试 |
| macOS fork/exec 权限 | 子进程可能无法启动 | 先用简单 fork 测试验证 |
| picohttpparser 内存扫描 | Fix 1 的 +4 已覆盖 | 已有 trailing `\0` 保护 |
| JS_Eval + 用户数据注入 | Fix 2 已用 C API 构建对象 | 严格遵守安全边界 |
| mquickjs API 不匹配 | 编译错误或运行时崩溃 | 严格参照 `mquickjs_c_api.md` |
| JS 全局变量 shape 泄漏 | Fix 9 已用 `__http_registry` 容器 | 所有动态属性挂载到单一容器 |
| select() 被信号中断 | Fix 7 EINTR 保护 | `if (errno == EINTR) return;` |
| 不完整 header 卡住主线程 | Fix 8 incomplete 保护 | `if (r == -2) break;` |
| 子进程继承父进程 FD | Fix 10 FD_CLOEXEC + 关闭循环 | 子进程 execvp 前关闭 > STDERR_FILENO 的所有 FD |
| mquickjs heap 碎片化 | Fix 11 显式 nullify 大字符串 | ON_END 时先 null body 再 delete |

## Defensive Fixes 清单

| # | 名称 | Step | 状态 |
|---|------|------|------|
| 1 | picohttpparser Buffer Margin | Step 4 | `realloc +4` + trailing `\0` |
| 2 | Ban JS_Eval string injection | Step 4 | C API 构建对象 |
| 3 | Follow-Redirects | Step 4 | curl `-L` 参数 |
| 4 | Redirect Loop Parsing | Step 4 | EOF 后循环 `phr_parse_response` |
| 5 | Zombie Process Cleanup | Step 4 | `kwcc_http_cleanup()` + `waitpid(WNOHANG)` |
| 6 | bin_path Executable Check | Step 4 | `access(bin_path, X_OK)` |
| 7 | select() EINTR Protection | Step 2 | `if (errno == EINTR) return;` |
| 8 | Incomplete Header Loop Guard | Step 4 | `if (r == -2) break;` |
| 9 | Global Shape Table Leak | Step 4 | `__http_registry` 容器对象 |
| 10 | FD Pollution via fork | Step 4 | `FD_CLOEXEC` + 关闭循环 |
| 11 | Heap Fragmentation Guard | Step 7 | 显式 nullify body 再 delete |
