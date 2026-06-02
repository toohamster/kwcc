# Async I/O 实施计划

> 基于已确认的方案：`requirements/async-io-promise.md`（含 5 个 Defensive Fixes）
> 状态：待确认

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

**为什么 Config 是独立步骤**：
- `kwcc_config()` 是项目的**配置管理基础设施**，定义了 JS→C 的配置传递范式
- HTTP 层的 `bin_path`、`parser_mode`、`timeout` 都依赖 config 读取
- 未来其他模块（日志级别、渲染参数等）也会复用同一套机制
- 当前代码中完全不存在 `kwcc_config` 实现（grep 确认），需要从零构建

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

**设计依据**：方案文档 "HTTP 配置选项（`kwcc_config`）" 章节

**为什么独立**：Config 是项目的配置管理基础设施，不只是 HTTP 的附属品。JS 层调用 `kwcc_config(module, options)` 设置配置，C 层通过 `kwcc_config_get(module, key)` 读取。未来所有模块共享同一机制。

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

**设计决策**：
- 纯字符串存储（简单，避免类型转换复杂度）
- HTTP 层读取后自行转换：`atoi(kwcc_config_get("http", "timeout", "30"))`
- 不需要 JSON 解析，JS 层调用时由 C binding 展平对象为 key-value 对

#### 3c. `src/jsapi.c` — 添加 `js_kwcc_config` C binding

```c
JSValue js_kwcc_config(JSContext *ctx, JSValue *this_val,
                       int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;

    JSCStringBuf mb;
    const char *module = JS_ToCString(ctx, argv[0], &mb);
    if (!module) return JS_UNDEFINED;

    JSValue options = argv[1];
    if (JS_GetClassID(ctx, options) != JS_CLASS_ARRAY) {
        /* 是对象：遍历属性 */
        JSValue keys = JS_GetPropertyStr(ctx, options, "length");
        /* 实际上 mquickjs 没有 Object.keys()，需要在 JS wrapper 中展平 */
    }

    /* JS wrapper 负责展平对象为逐 key 调用 */
    return JS_UNDEFINED;
}
```

**注册方式**：在 `kwcc_create_js()` 中通过 `JS_SetPropertyStr` + C function 挂载到 global object。

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

**注意**：mquickjs 支持 `Object.keys()`（已在 `mquickjs_es5.md` 确认）。`_native_config_set` 是 C 层注册的底层函数。

**验证**：`make` 编译通过

---

## Step 4: HTTP Process Engine（Layer 2）

**文件**：`src/kwcc_http.h`（新建）、`src/kwcc_http.c`（新建）

**设计依据**：方案文档 "Layer 2: HTTP Process Engine" 章节 + 5 个 Defensive Fixes

### `src/kwcc_http.h`

```c
#ifndef KWCC_HTTP_H
#define KWCC_HTTP_H

#include <sys/types.h>   /* pid_t */

#define KWCC_HTTP_MAX_REQS 8
#define KWCC_HTTP_INIT_CAP 4096

typedef enum {
    KWCC_HTTP_STATE_IDLE,
    KWCC_HTTP_STATE_RUNNING,
    KWCC_HTTP_STATE_DONE,
    KWCC_HTTP_STATE_ERROR
} kwcc_http_state_t;

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
    int      total_size;      /* Content-Length, 0=unknown */
    int      http_status;
    int      last_dispatched; /* 上次 dispatch progress 时的已读字节 */
    kwcc_http_state_t state;
    int      in_use;
} kwcc_http_req_t;

void kwcc_http_init(void);
void kwcc_http_request(const char *req_id, const char *method,
                       const char *url, const char **headers, int header_count,
                       const char *body, int body_len);
void kwcc_http_cancel(const char *req_id);
void kwcc_http_poll(void);  /* 每帧调用，检查进度事件 */

#endif
```

### `src/kwcc_http.c` 核心实现

#### 1. 全局状态

```c
static kwcc_http_req_t g_http_reqs[KWCC_HTTP_MAX_REQS];
```

#### 2. `kwcc_http_init()`

- 清零所有槽位

#### 3. `kwcc_http_request()` 内部流程

```
1. 查找空闲槽位，复制 req_id / method / url
2. 读取 http config（bin_path, parser_mode, timeout）
3. bin_path 可执行检测：access(bin_path, X_OK) == -1 → 报错并 cleanup
4. 构建 curl argv: "-s", "-L", "-i", "-X", method, "-H"..., "-d", body, url
5. pipe() + fork()
6. 子进程：dup2(pipefd[1], STDOUT_FILENO) → execvp(bin_path, argv)
7. 父进程：close(pipefd[1]) → fcntl(O_NONBLOCK) → malloc(response_buf)
           → kwcc_io_register(pipe_read_fd, http_on_read, req)
```

**Fix 3**：始终添加 `-L` 参数跟随重定向

**Fix 6：bin_path 可执行检测**：`fork()` 之前用 `access(bin_path, X_OK)` 检测路径是否存在且可执行。检测失败时错误信息必须包含模块名和具体路径：

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

#### 4. `http_on_read()` 回调（核心解析逻辑）

这是最复杂的部分，整合所有 5 个 fixes：

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
            if (r <= 0) break;
            ret += r;
            final_status = st;
            p = req->response_buf + ret;
            remaining = req->response_len - ret;
        }

        const char *body_start = p;
        int body_len = (int)remaining;

        http_dispatch_end(req, 0, final_status, body_start, body_len,
                          final_headers, final_num_headers);
        kwcc_http_cleanup(req);  /* Fix 5: 清理 */
        return;
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg), "http: read error (%s)", strerror(errno));
        http_dispatch_end(req, 1, 0, errmsg, (int)strlen(errmsg), NULL, 0);
        kwcc_http_cleanup(req);  /* Fix 5: 清理 */
    }
}
```

#### 5. `http_dispatch_end()` — C→JS 通信

**Fix 2**：禁止 `JS_Eval` + 字符串拼接传递 body/header

```c
static void http_dispatch_end(kwcc_http_req_t *req, int error,
                               int status, const char *body, int body_len,
                               struct phr_header *headers, size_t num_headers) {
    JSContext *ctx = /* 获取全局 JS 上下文 */;
    if (!ctx) return;

    if (error) {
        /* 错误：仅传递可控的简短描述 */
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "$bus.emit('http/end', { reqId:'%s', error:'%s' });",
            req->req_id, "parse error");
        JS_Eval(ctx, buf, strlen(buf), "<http_end>", 0);
        return;
    }

    /* 成功：C API 构建 JS response 对象 */
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

    /* 全局对象传递 — req_id 隔离变量名 */
    JSValue global_obj = JS_GetGlobalObject(ctx);
    char global_name[128];
    snprintf(global_name, sizeof(global_name), "__http_resp_%s", req->req_id);
    JS_SetPropertyStr(ctx, global_obj, global_name, resp);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "$bus.emit('http/end', new Object(), %s); delete global['%s'];",
        global_name, global_name);
    JS_Eval(ctx, buf, strlen(buf), "<http>", JS_EVAL_REPL);
}
```

#### 6. `kwcc_http_cleanup()` — Fix 5

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

#### 7. `kwcc_http_cancel()`

```c
void kwcc_http_cancel(const char *req_id) {
    kwcc_http_req_t *req = find_req(req_id);
    if (!req) return;
    kill(req->pid, SIGTERM);
    waitpid(req->pid, NULL, WNOHANG);
    kwcc_http_cleanup(req);
}
```

#### 8. `kwcc_http_poll()` — 进度事件

每帧检查 `req->response_len != req->last_dispatched`，有变化则 dispatch progress。

**验证**：`make` 编译通过

---

## Step 5: Sokol Frame Hook（Layer 3）

**文件**：`src/main.m`（修改）

**改动**：在 `frame()` 回调最前面插入 `kwcc_io_poll_once()`。

```c
#include "kwcc_io.h"
#include "kwcc_http.h"

static void frame(void) {
    int w = sapp_width();
    int h = sapp_height();

    /* 1. I/O reactor polling (non-blocking) — 必须在 JS 处理之前 */
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

### 6a. `_native_http_request` C binding（`src/jsapi.c`）

```c
#include "kwcc_http.h"

JSValue js_native_http_request(JSContext *ctx, JSValue *this_val,
                               int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 3) return JS_UNDEFINED;

    JSCStringBuf rb, mb, ub;
    const char *req_id = JS_ToCString(ctx, argv[0], &rb);
    const char *method = JS_ToCString(ctx, argv[1], &mb);
    const char *url = JS_ToCString(ctx, argv[2], &ub);
    if (!req_id || !method || !url) return JS_UNDEFINED;

    const char *header_ptrs[16] = {0};
    int header_count = 0;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) {
        JSValue headers_arr = argv[3];
        if (JS_GetClassID(ctx, headers_arr) == JS_CLASS_ARRAY) {
            JSValue len_val = JS_GetPropertyStr(ctx, headers_arr, "length");
            int arr_len = 0;
            if (JS_ToInt32(ctx, &arr_len, len_val) == 0) {
                if (arr_len > 16) arr_len = 16;
                for (int i = 0; i < arr_len; i++) {
                    JSValue item = JS_GetPropertyUint32(ctx, headers_arr, i);
                    JSCStringBuf hb;
                    const char *s = JS_ToCString(ctx, item, &hb);
                    if (s) header_ptrs[header_count++] = s;
                }
            }
        }
    }

    const char *body = "";
    int body_len = 0;
    if (argc > 4 && !JS_IsUndefined(argv[4]) && !JS_IsNull(argv[4])) {
        JSCStringBuf bb;
        const char *b = JS_ToCString(ctx, argv[4], &bb);
        if (b) { body = b; body_len = (int)strlen(b); }
    }

    kwcc_http_request(req_id, method, url,
                      header_count > 0 ? header_ptrs : NULL,
                      header_count, body, body_len);

    return JS_UNDEFINED;
}
```

**注册方式**：通过 `js_global_object[]` 在 `deps/mquickjs/mqjs_stdlib.c` 中注册（`CONFIG_KWCC` 保护）。

或者更简单的方式：在 `kwcc_create_js()` 中直接通过 C API 挂载到 global object（绕过 stdlib 重新编译）。

### 6b. JS wrapper（`kwcc_create_js` 中的 `methods_js` 字符串）

```javascript
_native_http_request = function(reqId, method, url, headers, body) {
    kwcc_http_request(reqId, method, url, headers, body);
};
```

**注意**：mquickjs 的 `kwcc_http_request` 需要从 C 层注册。有两种方案：

**方案 A**（推荐）：在 `kwcc_create_js()` 中通过 `JS_SetPropertyStr` + `JS_NewCFunctionParams` 直接挂载到 global object，无需修改 `mqjs_stdlib.c`。

**方案 B**：在 `mqjs_stdlib.c` 的 `js_global_object[]` 中注册，需要重新编译 host tool。

**选择方案 A**：避免重新编译 host tool，减少复杂度。

### 6c. 初始化（`src/kwcc.c`）

在 `kwcc_init()` 或 `kwcc_create_js()` 中：
- `kwcc_io_init()`
- `kwcc_http_init()`
- 注册 `_native_http_request` 到 global object

**依赖确认**：Step 3 已实现 config 系统，HTTP 层可通过 `kwcc_config_get("http", ...)` 读取配置

**验证**：`make` 编译通过

---

## Step 7: JS Promise + http module（Layer 4）

**文件**：
- `app/runtime/http.js`（新建）
- `app/main.js`（修改：加载 http.js）

### 7a. `app/runtime/http.js`

包含：
1. `MiniPromise` 实现（ES5 兼容，无 let/const/箭头函数）
2. `fetchAsync(reqId, url, options)` 封装
3. `$store` 注册 http module

**ES5 语法约束**（来自 `mquickjs_es5.md`）：
- 只用 `var`，不用 `let/const`
- 只用 `function() {}`，不用箭头函数
- 用 `new Object()` 代替 `{}` 在语句开头
- 不用 `...rest` 参数

### 7b. `app/main.js` 修改

```javascript
load("app/runtime/http.js");
```

在 `initStore()` 之前加载。

### 7c. 测试用例

在 test 模块中添加简单 HTTP 请求按钮：

```javascript
ui.button("Test HTTP", "test/http");
```

对应 event handler：
```javascript
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
| `kwcc_curl` 不存在 | 无法发起 HTTP 请求 | 先用系统 `curl` 命令测试（`bin_path: "curl"`） |
| macOS fork/exec 权限 | 子进程可能无法启动 | 先用简单 fork 测试验证 |
| picohttpparser 内存扫描 | Fix 1 的 +4 已覆盖 | 已有 trailing `\0` 保护 |
| JS_Eval + 用户数据注入 | Fix 2 已用 C API 构建对象 | 严格遵守安全边界 |
| mquickjs API 不匹配 | 编译错误或运行时崩溃 | 严格参照 `mquickjs_c_api.md` |
| JS 全局变量污染 | `delete global['name']` 清理 | Fix 2 的 `<req_id>` 隔离 |
