# 实施计划：Async I/O 完整方案 — 6 个步骤

> 基于：`requirements/async-io-promise.md`（已修正 mquickjs API）
> 范围：全部 6 步（Layer 1 ~ Layer 4 + 准备 + C binding）

---

## 架构回顾

```
┌─────────────────────────────────────────────────┐
│ Layer 4: JS Promise + http module (runtime/)   │  ← Step 5
├─────────────────────────────────────────────────┤
│ Layer 3: Sokol frame hook (src/main.m)         │  ← Step 4
├─────────────────────────────────────────────────┤
│ Layer 2: HTTP Process Engine (kwcc_http.c)     │  ← Step 3
├─────────────────────────────────────────────────┤
│ Layer 1: I/O Reactor (kwcc_io.c)               │  ← Step 2
└─────────────────────────────────────────────────┘

+ Config System — Step 3（伴随 HTTP 引擎）
+ picohttpparser 集成 — Step 1
+ C binding 注册 — Step 6
```

---

## Step 1: 引入 picohttpparser

### 来源
`https://github.com/h2o/picohttpparser` — MIT 许可

### 文件
| 文件 | 操作 |
|------|------|
| `deps/picohttpparser/picohttpparser.h` | 从 /tmp 复制 |
| `deps/picohttpparser/picohttpparser.c` | 从 /tmp 复制 |

### 关键 API
```c
int phr_parse_response(buf, len, &minor, &status, &msg, &msg_len,
                       headers, &num_headers, last_len);
```
- 返回 `> 0`：header 结束位置（body 起始偏移），`*status` 直接包含 HTTP 状态码
- 返回 `-2`：数据不完整，继续累积
- 返回 `-1`：协议错误
- `phr_header` 使用零拷贝指针（`name`, `name_len`, `value`, `value_len`）

### setup.sh 变更
- 在 nanosvg 之后、font 之前添加 picohttpparser 下载
- 步骤编号更新为 `[5/7]` / `[6/7]` / `[7/7]`

### Makefile 变更
- `DEP_SRCS` 添加 `deps/picohttpparser/picohttpparser.c`
- 添加构建规则 `$(OBJ_DIR)/deps/picohttpparser/%.o`
- 添加目录 `$(OBJ_DIR)/deps/picohttpparser` 到 mkdir 列表

---

## Step 2: I/O Reactor (`src/kwcc_io.h` + `kwcc_io.c`)

### 职责
纯 POSIX 文件描述符管理器，维护非阻塞 FD 列表，提供 `select()` 零超时轮询。

### 数据结构
```c
#define KWCC_IO_MAX_FDS 16

typedef void (*kwcc_io_callback_t)(int fd, void *user_data);

typedef struct {
    int                fd;
    kwcc_io_callback_t callback;
    void              *user_data;
    int                in_use;
} kwcc_io_slot_t;
```

### API
| 函数 | 说明 |
|------|------|
| `kwcc_io_init()` | 初始化，清空所有槽位 |
| `kwcc_io_register(fd, cb, user_data)` | 注册 FD 和回调 |
| `kwcc_io_unregister(fd)` | 注销 FD 并 `close(fd)` |
| `kwcc_io_poll_once()` | 每帧调用，`timeout=0` 非阻塞 |

### 实现要点
- `select()` 用 `timeout = {0, 0}`，不阻塞渲染
- 仅处理可读事件（`rfds`）
- `read()` 返回 > 0：追加到 `kwcc_http` 侧的 C buffer
- `read()` 返回 0：pipe closed，标记 stream end
- `read()` 返回 -1：`EAGAIN` = 正常，其他 = 错误

---

## Step 3: HTTP Process Engine + Config 系统

### 3a. HTTP Engine (`src/kwcc_http.h` + `kwcc_http.c`)

#### 数据结构
```c
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
    char    *response_buf;    /* 动态分配，realloc 扩容 */
    int      response_cap;
    int      response_len;
    int      total_size;      /* Content-Length (0=未知) */
    int      http_status;
    int      last_dispatched; /* 上次 dispatch 进度时的已读字节数 */
    int      in_use;
} kwcc_http_req_t;
```

#### `kwcc_http_request()` 流程
1. 查找空闲槽位，复制 req_id / method / url
2. 读取 config：`kwcc_config_get("http")` → `bin_path`（默认 "curl"）
3. `pipe()` + `fork()`
4. **子进程**：`dup2(pipefd[1], STDOUT_FILENO)` → `execvp(bin_path, argv)`
5. **父进程**：`close(pipefd[1])` → `fcntl(O_NONBLOCK)` → `malloc(response_buf)` → `kwcc_io_register()`

#### `http_on_read(fd, user_data)` — I/O 回调
1. `read(fd, tmp, sizeof(tmp))`
2. `n > 0`：realloc 扩容 → 追加 → `phr_parse_response()` 增量解析
   - `-2`：数据不完整 → dispatch progress
   - `-1`：协议错误 → dispatch error → unregister
   - `> 0`：解析成功 → 记录 `http_status` → dispatch progress
3. `n == 0`（EOF）：最终解析 → `http_dispatch_end()` → unregister
4. `n == -1`：`EAGAIN` = 正常，其他 = error

#### `http_dispatch_end()` — C→JS dispatch（mquickjs 正确方式）

**核心模式**：`JS_Eval` + 全局对象传递复杂数据（与 `kwcc_dispatch_event` 一致）

```c
static void http_dispatch_end(kwcc_http_req_t *req, int error,
                               int status, const char *body, int body_len,
                               struct phr_header *headers, size_t num_headers) {
    JSContext *ctx = g_js_ctx;
    if (!ctx) return;

    if (error) {
        /* 错误：转义消息 */
        char escaped[512];
        int j = 0;
        for (int i = 0; i < body_len && j < (int)(sizeof(escaped) - 2); i++) {
            char c = body[i];
            if (c == '\\' || c == '\'' || c == '\n' || c == '\r')
                escaped[j++] = '\\';
            escaped[j++] = c;
        }
        escaped[j] = '\0';

        char buf[1024];
        snprintf(buf, sizeof(buf),
            "$bus.emit('http/end', { reqId:'%s', error:'%s' });",
            req->req_id, escaped);
        JS_Eval(ctx, buf, strlen(buf), "<http_end>", 0);
        return;
    }

    /* 成功：构建 JS response 对象 */
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

    /* 通过全局对象保护 resp（GC 安全），然后 JS_Eval emit */
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global_obj, "__http_resp", resp);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "$bus.emit('http/end', new Object(), __http_resp);");
    JS_Eval(ctx, buf, strlen(buf), "<http>", JS_EVAL_REPL);

    /* 清理全局变量 */
    JS_SetPropertyStr(ctx, global_obj, "__http_resp", JS_UNDEFINED);
}
```

**禁止使用的 API**（mquickjs 中不存在）：
- ~~`JS_Call(ctx, func, this, argc, argv)`~~
- ~~`JS_FreeValue(ctx, val)`~~
- ~~`JS_Duplicate(ctx, val)`~~

#### `http_dispatch_progress()` — 进度事件
每帧检查 `response_len != last_dispatched` → `$bus.emit('http/progress', { reqId, loaded, total })`

#### `kwcc_http_cancel(req_id)`
- `kill(pid, SIGTERM)` + `waitpid(pid, WNOHANG)` + `free()` + `kwcc_io_unregister()`

### 3b. Config 系统 (`src/kwcc.h` + `kwcc.c`)

#### API
```javascript
kwcc_config("http", { bin_path: "curl", chunked_support: 0 });  // set
kwcc_config("http");                                             // get → JSValue
```

#### C 侧存储（纯值赋值，不需要 GC ref）

`JSValue` 是 `uint64_t` 值类型，直接赋值即可。对象在 JS 层有引用，mquickjs GC 会处理：

```c
#define CONFIG_MAX 32
static JSValue  g_configs[CONFIG_MAX];
static char     g_config_keys[CONFIG_MAX][64];
static int      g_config_count = 0;

JSValue kwcc_config_get(const char *module) {
    for (int i = 0; i < g_config_count; i++) {
        if (strcmp(g_config_keys[i], module) == 0) {
            return g_configs[i];
        }
    }
    return JS_UNDEFINED;
}

void kwcc_config_set(const char *module, JSValue val) {
    for (int i = 0; i < g_config_count; i++) {
        if (strcmp(g_config_keys[i], module) == 0) {
            g_configs[i] = val;  /* uint64_t 直接赋值 */
            return;
        }
    }
    if (g_config_count < CONFIG_MAX) {
        strncpy(g_config_keys[g_config_count], module, 63);
        g_config_keys[g_config_count][63] = '\0';
        g_configs[g_config_count] = val;
        g_config_count++;
    }
}
```

#### JS 包装器
```javascript
kwcc_config = function(module, value) {
    if (value !== undefined) {
        kwcc_ui('configSet', module, value);
    } else {
        return kwcc_ui('configGet', module);
    }
};
```

---

## Step 4: Sokol Frame Hook (`src/main.m`)

### 在 `frame()` 回调中插入 I/O polling

```c
void frame(void) {
    /* 1. I/O reactor polling (non-blocking) — 必须在 kwcc_process_js 之前 */
    kwcc_io_poll_once();

    /* 2. 原有 JS 处理 + microui 渲染 */
    kwcc_process_js(g_js_ctx, app_js_text);

    /* 3. 原有 NanoVG 渲染 */
    render_mu_commands();
}
```

**初始化**：在 `init()` 中调用 `kwcc_io_init()` 和 `kwcc_http_init()`。

---

## Step 5: JS Promise + http module (`app/runtime/http.js`)

### 5a. MiniPromise（ES5 兼容）
```javascript
function MiniPromise(executor) {
    var self = this;
    self.status = "PENDING";
    self.value = null;
    self.callbacks = [];
    // resolve/reject 逻辑 + then/catch ...
}
```

### 5b. fetchAsync 封装
```javascript
function fetchAsync(reqId, url, options) {
    // 通过 _native_http_request 发起请求
    // 监听 $bus 'http/end'/'http/error'/'http/progress' 事件
    // 返回 MiniPromise
}
```

### 5c. $store 注册 http module（可选，用于跟踪活跃请求数）

---

## Step 6: C binding 注册 (`deps/mquickjs/mqjs_stdlib.c` + `src/jsapi.c`)

### 在 `js_global_object[]` 中注册
```c
#ifdef CONFIG_KWCC
    JS_CFUNC_DEF("kwcc_ui", 3, js_kwcc_ui),
    JS_CFUNC_DEF("_native_http_request", 5, js_native_http_request),
#endif
```

### `js_native_http_request` 实现（`src/jsapi.c`）
```c
JSValue js_native_http_request(JSContext *ctx, JSValue *this_val,
                               int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 3) return JS_UNDEFINED;

    JSCStringBuf rb, mb, ub;
    const char *req_id = JS_ToCString(ctx, argv[0], &rb);
    const char *method = JS_ToCString(ctx, argv[1], &mb);
    const char *url = JS_ToCString(ctx, argv[2], &ub);
    if (!req_id || !method || !url) return JS_UNDEFINED;

    // headers (arg 3, optional array), body (arg 4, optional string)
    const char **headers = NULL;
    int header_count = 0;
    const char *body = NULL;
    int body_len = 0;

    kwcc_http_request(req_id, method, url, headers, header_count, body, body_len);
    return JS_UNDEFINED;
}
```

---

## mquickjs API 速查（关键差异）

| 用途 | mquickjs API | 标准 QuickJS（不存在于 mquickjs） |
|------|-------------|--------------------------------|
| 调用 JS 函数 | `JS_PushArg(ctx, val)` + `JS_Call(ctx, n)` | ~~`JS_Call(ctx, func, this, argc, argv)`~~ |
| GC 栈式保护 | `JS_PUSH_VALUE(ctx, v)` / `JS_POP_VALUE(ctx, v)` | ~~`JS_FreeValue(ctx, v)`~~ |
| GC 列表保护 | `JS_AddGCRef(ctx, &ref)` / `JS_DeleteGCRef(ctx, &ref)` | —— |
| 复制 JSValue | **不需要**（`JSValue` 是 `uint64_t` 值类型） | ~~`JS_Duplicate(ctx, v)`~~ |
| C→JS dispatch | `JS_Eval(ctx, code, len, filename, flags)` + 全局对象 | —— |

**关键**：`JS_Call(ctx, call_flags)` 只有 2 个参数，不是标准 QuickJS 的 5 参数版本。
**关键**：`JSValue` 是 `uint64_t`（tagged integer），不是结构体，无需引用计数。

---

## 文件清单

| 文件 | 步骤 | 操作 |
|------|------|------|
| `deps/picohttpparser/picohttpparser.h` | 1 | **新建** |
| `deps/picohttpparser/picohttpparser.c` | 1 | **新建** |
| `setup.sh` | 1 | **修改** |
| `Makefile` | 1,6 | **修改** |
| `src/kwcc.h` | 3 | **修改** |
| `src/kwcc.c` | 3 | **修改** |
| `src/kwcc_io.h` | 2 | **新建** |
| `src/kwcc_io.c` | 2 | **新建** |
| `src/kwcc_http.h` | 3 | **新建** |
| `src/kwcc_http.c` | 3 | **新建** |
| `src/main.m` | 4 | **修改** |
| `app/runtime/http.js` | 5 | **新建** |
| `deps/mquickjs/mqjs_stdlib.c` | 6 | **修改** |
| `src/jsapi.c` | 6 | **修改** |

---

## 验证

1. `make clean && make` — 编译通过
2. `./kwcc` — 正常运行
3. JS 侧测试 `kwcc_config("test", { foo: "bar" })` → `kwcc_config("test")` 返回 `{ foo: "bar" }`
4. 添加 test 模块 HTTP 请求示例
