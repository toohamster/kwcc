# 方案：单线程异步 I/O + Promise 链式调用

> 状态：待论证
> 优先级：继 ID 覆盖机制之后的下一项

## 背景与目标

### 当前问题
KWCC 缺乏网络请求能力，无法对接外部 API。需要引入轻量、非阻塞的异步 HTTP 支持。

### 设计约束
1. **零外部依赖**：不引入 `libcurl` 库、不依赖系统 `curl` 命令的不确定性（通过独立 `kwcc_curl` 可执行文件解决生命周期隔离）
2. **无多线程**：纯单线程 Reactor 模式
3. **ES5 兼容**：遵守 mquickjs 语法约束（无 let/const/箭头函数）
4. **Promise 风格**：优雅链式调用，拒绝回调地狱

### 核心思路
**Single-Threaded Reactor Pattern**：用 `select()` 轮询非阻塞 pipe（来自独立 `kwcc_curl` 进程），在 C 层完整读取响应后一次性 dispatch 到 JS 层，由 `MiniPromise` 封装为 `.then()` 链式调用。

---

## 架构分层

```
┌─────────────────────────────────────────────────────────┐
│ Layer 4: JS Promise + http module (runtime/http.js)     │
│   MiniPromise / fetchAsync / $store.registerModule      │
├─────────────────────────────────────────────────────────┤
│ Layer 3: Sokol frame hook (src/main.m)                  │
│   frame() → kwcc_io_poll_once() → microui → NanoVG      │
├─────────────────────────────────────────────────────────┤
│ Layer 2: HTTP Process Engine (src/kwcc_http.c)          │
│   fork + pipe + kwcc_curl + O_NONBLOCK                  │
├─────────────────────────────────────────────────────────┤
│ Layer 1: I/O Reactor (src/kwcc_io.c)                    │
│   select() + FD array (max 16) + per-frame polling      │
└─────────────────────────────────────────────────────────┘
```

---

## Layer 1: I/O Reactor (`src/kwcc_io.h` / `kwcc_io.c`)

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

### API 契约
```c
void kwcc_io_init();
void kwcc_io_register(int fd, kwcc_io_callback_t cb, void *user_data);
void kwcc_io_unregister(int fd);
void kwcc_io_poll_once();  // 每帧调用，timeout=0 非阻塞
```

### 实现要点
1. `select()` 用 `timeout = {0, 0}`，不阻塞渲染
2. **Fix 7：EINTR 保护**：`select()` 返回 `-1` 且 `errno == EINTR` 时，说明被 OS 信号中断（窗口 resize、SIGCHLD 等），这不是致命错误，直接 `return` 退出本帧，下帧继续：
   ```c
   int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
   if (ret < 0) {
       if (errno == EINTR) return;  /* 信号中断，下帧继续 */
       /* 其他错误：log_error */
       return;
   }
   if (ret == 0) return;  /* 超时，无数据 */
   ```
3. `read()` 返回 > 0 时：追加到 `kwcc_http` 侧的 C buffer，**不触发 JS**
4. `read()` 返回 0（pipe closed）：标记 stream end，触发 `ON_END`
5. `read()` 返回 -1：
   - `EAGAIN`/`EWOULDBLOCK`：正常，无数据
   - 其他错误：触发 `ON_ERROR`
6. macOS 上 pipe read-end 需要正确处理 `EAGAIN`

---

## Layer 2: HTTP Process Engine (`src/kwcc_http.h` / `kwcc_http.c`)

### 职责
fork 独立 `kwcc_curl` 子进程，建立非阻塞 pipe，构造 curl 参数，响应解析。

### 子进程管理
```c
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

#define KWCC_HTTP_MAX_REQS 8
#define KWCC_HTTP_INIT_CAP 4096  // 初始缓冲 4KB，按需 realloc
```

### 依赖：picohttpparser（HTTP 响应解析器）

**来源**：`https://github.com/h2o/picohttpparser`
**下载**：`picohttpparser.h` + `picohttpparser.c` → `deps/picohttpparser/`
**许可**：MIT，~400 行 C，零外部依赖

**为什么引入**：
- `kwcc_curl` 是外部工具，未来可能被自研实现替换，新工具不一定支持 `-i` 标志
- picohttpparser 提供 `phr_parse_response()` 增量解析：
  - 返回 `-2`：数据不完整，继续累积，下帧再试
  - 返回 `-1`：HTTP 协议错误
  - 返回 `> 0`：header 解析成功，返回 header 结束位置
- 处理 `\r\n\r\n` 不完整、多行 header、Transfer-Encoding 等边缘情况
- 比手写 `strstr("\r\n\r\n")` 更健壮，面向未来可移植性

**使用方式**：
```c
int minor_version;
const char *msg;
size_t msg_len;
struct phr_header headers[64];
size_t num_headers = 64;
int prev_len = req->response_len;  // 已累积的字节数

int ret = phr_parse_response(req->response_buf, req->response_len,
                             &minor_version, &msg, &msg_len,
                             headers, &num_headers, 0);

if (ret == -2) {
    // 数据不完整，继续累积，下帧再试
    return;
}
if (ret == -1) {
    // HTTP 协议错误，dispatch error
    return;
}
if (ret > 0) {
    // header 解析成功
    // ret = header 结束位置（即 body 起始偏移）
    // msg = 状态消息（如 "OK"）
    // 从第一行提取 http_status（需要额外解析）
    // headers[] 数组包含所有 header，遍历找 Content-Length
    // body = response_buf + ret
}
```

**注意**：picohttpparser **直接返回** 状态码到 `*status` 参数中（无需 sscanf 从首行提取）。

### 动态缓冲区管理（picohttpparser 安全边界）

```c
// 初始分配
req->response_buf = malloc(KWCC_HTTP_INIT_CAP);
req->response_cap = KWCC_HTTP_INIT_CAP;

// 每帧 read() 后追加 — 每次追加后必须追加 trailing '\0'
while ((n = read(req->pipe_read_fd, buf, sizeof(buf))) > 0) {
    if (req->response_len + n + 4 > req->response_cap) {
        req->response_cap = (req->response_len + n + 4) * 2;
        req->response_buf = realloc(req->response_buf, req->response_cap);
    }
    memcpy(req->response_buf + req->response_len, buf, n);
    req->response_len += n;
    req->response_buf[req->response_len] = '\0';  // picohttpparser 安全边界
}
```

**为什么需要 trailing `\0`**：`phr_parse_response` 内部做指针扫描（`scan_quote`、`is_token_char`），如果 buffer 末尾没有 null 终止符可能越界读取。`+4` 字节保证 SIMD/指针扫描不越界。

### curl 参数规范
```bash
kwcc_curl -s -L -i \
  -X POST \
  -H "Authorization: Bearer token" \
  -H "Content-Type: application/json" \
  -d '{"username":"hamster"}' \
  "https://example.com/api"
```

参数说明：
- `-s`：silent 模式，不输出进度
- `-L`：**自动跟随 HTTP 301/302 重定向**（避免中间响应头泄漏到 picohttpparser）
- `-i`：包含 HTTP 响应头（用于解析状态码和 Content-Length）
- 未来替换为自研实现时，**输入输出格式保持不变**

### kwcc_curl 分发策略
```
kwcc/
├── kwcc           # 主程序
└── kwcc_curl      # 内置轻量 HTTP 客户端（独立可执行文件）
```

- `kwcc_curl` 可以是裁剪版 curl 编译的二进制，与主程序打包分发
- 生命周期独立：crash 不影响主程序，退出时 pipe 自动关闭
- 未来可替换为自研实现（标准 socket HTTP），接口不变（stdin/stdout + exit code）

### C 层响应解析（picohttpparser）

curl `-i` 输出格式：
```
HTTP/1.1 200 OK\r\n
Content-Type: application/json\r\n
Content-Length: 1234\r\n
\r\n
{"result":"ok","data":...}
```

**解析流程**：
1. 每帧 `read()` 后，动态扩容 `response_buf`（realloc +4 字节 + trailing `\0`）
2. 调用 `phr_parse_response(response_buf, response_len, ...)`：
   - 返回 `-2` → 数据不完整，退出，下帧继续
   - 返回 `-1` → 协议错误，dispatch `ON_ERROR`
   - 返回 `> 0` → header 解析成功，body 起始偏移 = ret（正常情况）
3. **EOF 后（`n == 0`）重定向循环解析**：`curl -i -L` 遭遇 302/301 重定向时，buffer 会包含所有中间响应。必须循环调用 `phr_parse_response`，每次从上次返回值（ret）继续，直到 buffer 剩余部分不再以 "HTTP/1.x" 开头：
   ```c
   const char *p = req->response_buf;
   size_t remaining = req->response_len;
   while (remaining >= 9 && memcmp(p, "HTTP/1.", 7) == 0) {
       // phr_parse_response 解析当前响应
       // ret = 当前响应 headers 结束位置
       // p = req->response_buf + ret; remaining -= ret;
   }
   // 循环结束：最后一次成功解析的就是最终响应（如 200）
   ```
   **Fix 8：不完整 Header 循环保护**：循环内 `phr_parse_response` 返回 `-2`（数据不完整）时必须立即 `break`，绝不能继续循环。否则会卡住 Sokol 主线程，造成永久冻结。
4. 状态码直接从 `*status` 参数获取（picohttpparser 已解析），**无需 sscanf**
5. 遍历 `headers[]` 找 `Content-Length` → `total_size`
6. Body = 最终响应的 body 起始指针，一次性 dispatch `ON_END`

### `kwcc_http_request()` 内部流程

```c
/* 1. 查找空闲槽位，复制 req_id / method / url */

/* 2. 读取 http config */
JSValue cfg = kwcc_config_get("http");
const char *bin_path = "curl";
const char *parser_mode = "raw";
int timeout = 30;

if (!JS_IsUndefined(cfg)) {
    /* bin_path */
    JSValue bp = JS_GetPropertyStr(ctx, cfg, "bin_path");
    if (JS_IsString(ctx, bp)) { JSCStringBuf b; const char *s = JS_ToCString(ctx, bp, &b); if (s) bin_path = s; }

    /* parser_mode */
    JSValue pm = JS_GetPropertyStr(ctx, cfg, "parser_mode");
    if (JS_IsString(ctx, pm)) { JSCStringBuf b; const char *s = JS_ToCString(ctx, pm, &b); if (s) parser_mode = s; }

    /* timeout */
    JSValue to = JS_GetPropertyStr(ctx, cfg, "timeout");
    if (!JS_IsUndefined(to)) { int ret = JS_ToInt32(ctx, &timeout, to); (void)ret; }
}

/* 3. 构建 curl argv */
// 基础："-s", "-X", method
// if (parser_mode == "raw") 添加 "-i"
// 始终添加 "-L"（跟随重定向）
// 添加 "--max-time", timeout_string
// 遍历 headers 数组添加 "-H", header_string
// if (body_len > 0) 添加 "-d", body
// 最后添加 url

/* 4. pipe() + fork() */
/* Fix 10: FD Pollution — fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) 创建后立即设置 */
/* 5. 子进程：close(pipefd[0]) → 关闭 > STDERR_FILENO 的所有 FD → dup2(pipefd[1], STDOUT_FILENO) → execvp(bin_path, argv) */
/* 6. 父进程：close(pipefd[1]) → fcntl(O_NONBLOCK) → malloc(response_buf) → kwcc_io_register() */
```

**Fix 10：子进程 FD 污染防护** — `fork()` 后子进程继承父进程所有 FD。如果不关闭，子进程持有父进程所有 pipe/socket FD，导致资源死锁：
```c
/* 父进程：pipe() 创建后立即设置 FD_CLOEXEC */
fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);

/* 子进程：execvp 前关闭所有无关 FD */
close(pipefd[0]);
for (int fd = STDERR_FILENO + 1; fd < sysconf(_SC_OPEN_MAX); fd++) {
    if (fd != pipefd[1]) close(fd);
}
dup2(pipefd[1], STDOUT_FILENO);
close(pipefd[1]);
execvp(bin_path, argv);
_exit(1);
```

### HTTP 配置选项（`kwcc_config`）

```javascript
kwcc_config("http", {
    bin_path: "curl",         // HTTP 客户端可执行文件路径（默认 "curl"）
    parser_mode: "raw",       // "raw" = picohttpparser 解析原始 HTTP 文本（curl -i 输出）
                              // "body" = 后端已返回纯净 body，跳过 picohttpparser
    chunked_support: 0,       // 0 = 不支持 chunked transfer（默认）
                              // 1 = 启用 phr_decode_chunked 解码
    timeout: 30,              // curl --max-time 秒数（默认 30s）
});
```

**`parser_mode` 说明**：
- **`"raw"`（默认）**：curl 使用 `-i` 参数输出完整 HTTP 响应（status line + headers + `\r\n\r\n` + body）。C 层用 `phr_parse_response()` 解析原始 HTTP 文本，提取状态码、headers、body 偏移。
- **`"body"`**：未来如果自研 socket 实现直接返回纯净 body（无 HTTP 头），C 层跳过 `phr_parse_response()` 调用，直接将读取数据作为 body dispatch。
- 切换 `parser_mode` 不影响 JS 层消费方式，`$bus.emit('http/end', resp)` 输出的 response 对象格式保持一致。

### JS→C 通信：mquickjs binding

**不用 JSON 序列化**，C binding 直接接收多个 JSValue 参数：

```javascript
// JS 侧调用
_native_http_request(reqId, method, url, headersArray, bodyString);
```

C 侧 binding（`src/jsapi.c` 风格）：

```c
JSValue js_native_http_request(JSContext *ctx, JSValue *this_val,
                               int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 3) return JS_UNDEFINED;

    // reqId (arg 0) — 字符串
    JSCStringBuf rb;
    const char *req_id = JS_ToCString(ctx, argv[0], &rb);
    if (!req_id) return JS_UNDEFINED;

    // method (arg 1) — 字符串
    JSCStringBuf mb;
    const char *method = JS_ToCString(ctx, argv[1], &mb);
    if (!method) return JS_UNDEFINED;

    // url (arg 2) — 字符串
    JSCStringBuf ub;
    const char *url = JS_ToCString(ctx, argv[2], &ub);
    if (!url) return JS_UNDEFINED;

    // headers (arg 3, optional) — JSArray，提取为 C 字符串数组
    const char *header_ptrs[16] = {0};
    int header_count = 0;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) {
        JSValue headers_arr = argv[3];
        if (JS_GetClassID(ctx, headers_arr) == JS_CLASS_ARRAY) {
            // 获取数组长度
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

    // body (arg 4, optional) — 字符串
    const char *body = "";
    int body_len = 0;
    if (argc > 4 && !JS_IsUndefined(argv[4]) && !JS_IsNull(argv[4])) {
        JSCStringBuf bb;
        const char *b = JS_ToCString(ctx, argv[4], &bb);
        if (b) { body = b; body_len = (int)strlen(b); }
    }

    kwcc_http_request(req_id, method, url,
                      header_count > 0 ? header_ptrs : NULL,
                      header_count,
                      body, body_len);

    return JS_UNDEFINED;
}
```

**说明**：
- mquickjs 没有 `JS_GetArrayLength` 或 `JS_GetPropertyUint32_array_len`，必须用 `JS_GetPropertyStr(ctx, arr, "length")` + `JS_ToInt32`
- `JS_ToCString` 返回的指针是函数内局部使用（短字符串内联到 `JSCStringBuf` 栈缓冲区），传给 `kwcc_http_request` 后会被 `strdup` 保存，函数返回后指针失效不影响

**mquickjs API 对照**（基于 `deps/mquickjs/mquickjs.h` + `src/jsapi.c` 验证）：

| 用途 | mquickjs API | 说明 |
|------|-------------|------|
| JSValue → C 字符串 | `JS_ToCString(ctx, val, &cbuf)` | 返回 `const char*`，`cbuf` 是 `JSCStringBuf[5]` 栈缓冲区 |
| 创建 JS 字符串 | `JS_NewString(ctx, buf)` 或 `JS_NewStringLen(ctx, buf, len)` | 避免 NULL 问题 |
| 创建 JS 数字 | `JS_NewInt32(ctx, val)` / `JS_NewInt64(ctx, val)` / `JS_NewFloat64(ctx, val)` | |
| 创建 JS 对象 | `JS_NewObject(ctx)` | 不用 `{}` |
| 创建 JS 数组 | `JS_NewArray(ctx, initial_len)` | |
| 获取对象属性 | `JS_GetPropertyStr(ctx, obj, "key")` | 返回 `JSValue` |
| 设置对象属性 | `JS_SetPropertyStr(ctx, obj, "key", val)` | 注意：`val` 被 transfer 到对象上 |
| 获取数组元素 | `JS_GetPropertyUint32(ctx, arr, idx)` | 返回 `JSValue` |
| 获取数组长度 | `JS_GetPropertyStr(ctx, arr, "length")` + `JS_ToInt32` | mquickjs 没有 `JS_GetArrayLength` |
| 判断类型 | `JS_IsString(ctx, val)` / `JS_IsNumber(ctx, val)` / `JS_IsNull(val)` / `JS_IsUndefined(val)` | |
| 判断类 | `JS_GetClassID(ctx, val) == JS_CLASS_ARRAY` | 更精确 |
| 创建 bool | `JS_NewBool(val)` | |
| 未定义值 | `JS_UNDEFINED` | 宏常量 |
| C 函数注册 | `JS_CFUNC_DEF("name", argc, func)` | 在 `mqjs_stdlib.c` 的 `js_global_object[]` 中注册 |
| GC 栈式保护 | `JS_PUSH_VALUE(ctx, v)` / `JS_POP_VALUE(ctx, v)` | 保护函数内临时 JSValue，先声明 `JSGCRef v_ref` |
| GC 列表保护 | `JS_AddGCRef(ctx, &ref)` / `JS_DeleteGCRef(ctx, &ref)` | 长期保护（如 config 存储），返回 `JSValue*` |
| 调用 JS 函数 | `JS_PushArg(ctx, val)` + `JS_Call(ctx, n)` | 必须先 `JS_StackCheck(ctx, n+2)`，压栈顺序：arg[n-1]...arg[0], func, this_obj |
| 执行 JS 代码 | `JS_Eval(ctx, code, len, "<file>", flags)` | **最安全的 C→JS dispatch 方式**，配合全局对象传递复杂数据 |
| 无此函数 | ~~`JS_Duplicate`~~、~~`JS_FreeValue`~~、~~`JS_Call(ctx, func, this, argc, argv)`~~ | **标准 QuickJS 有，但 mquickjs 没有！** |

**关键陷阱**（来自 `build_pitfalls.md` + `mquickjs_es5.md`）：
- `JS_ToCString` 可能返回 NULL → 必须 NULL 检查后再使用
- `JSCStringBuf` 是 5 字节栈结构（`uint8_t buf[5]`），短字符串内联存储
- mquickjs 的 `JSValue` 是 `uint64_t`（64 位平台），不是结构体
- **没有引用计数**：`JSValue` 是值类型（tagged integer），不需要 `JS_Duplicate`
- **没有 `JS_FreeValue`**：GC 通过 `JS_PUSH_VALUE`/`JS_POP_VALUE`（栈式）或 `JS_AddGCRef`/`JS_DeleteGCRef`（列表式）管理
- **`JS_Call(ctx, call_flags)` 只有 2 个参数**，不是标准 QuickJS 的 5 参数版本。参数通过 `JS_PushArg` 预先压栈
- 不支持 `...rest` 参数 → JS 侧传固定参数，C 侧用 `argc` 判断可选参数
- `{}` 在语句开头被解析为 block → JS 侧用 `new Object()`
- C 函数必须 include 头文件声明，否则 x86_64 ABI 会 float→double 提升

### C→JS 通信：一次性 dispatch（ON_END）

**核心原则**：C 层在 pipe 端完整读取响应，只在 ON_END 时一次性 dispatch，避免 per-chunk 拖慢 60fps。

**安全原则**：
- **禁止**用 `JS_Eval` + 字符串拼接传递 body/header 数据（引号、换行会破坏 JS 语法，导致 `SyntaxError`）
- **允许**用 `JS_Eval` + 全局对象传递变量名（变量名不含用户数据，语法安全）
- 复杂对象（body、headers）必须通过 C API 构建后挂到全局对象上

**并发安全**：
- 如果同时有 2+ 个 `fetchAsync` 在同一帧或相邻帧返回，使用单一全局名 `__http_resp` 会被后一个请求覆盖
- 必须用隔离机制，每个请求有独立的变量

**Fix 9：Global Property Shape 泄漏保护** — mquickjs 是高度精简引擎，直接在 global object 上动态添加/删除唯一属性名（如 `__http_resp_req_xxx`）可能导致 global shape 表中未压缩的空洞泄漏。改用单一静态容器对象 `global.__http_registry`：

```c
static void http_dispatch_end(kwcc_http_req_t *req, int error,
                               int status, const char *body, int body_len,
                               struct phr_header *headers, size_t num_headers) {
    JSContext *ctx = g_js_ctx;
    if (!ctx) return;

    if (error) {
        /* 错误：仅传递可控的简短描述，不传递 body 原文 */
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "$bus.emit('http/end', { reqId:'%s', error:'%s' });",
            req->req_id, "parse error");
        JS_Eval(ctx, buf, strlen(buf), "<http_end>", 0);
        return;
    }

    /* 成功：用 C API 构建 JS response 对象（不拼接任何用户数据到字符串）*/
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

    /* Fix 9: 挂载到 __http_registry 容器，避免 global shape 表泄漏 */
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue registry = JS_GetPropertyStr(ctx, global_obj, "__http_registry");
    if (JS_IsUndefined(registry)) {
        registry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global_obj, "__http_registry", registry);
    }
    JS_SetPropertyStr(ctx, registry, req->req_id, resp);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "$bus.emit('http/end', new Object(), __http_registry['%s']);"
        "delete __http_registry['%s'];",
        req->req_id, req->req_id);
    JS_Eval(ctx, buf, strlen(buf), "<http>", JS_EVAL_REPL);
}
```

**为什么需要 `__http_registry`**：
- 直接在 global object 上创建/删除唯一属性名（`__http_resp_req_1`, `__http_resp_req_2`, ...）会让 mquickjs 的 global shape 表累积空洞
- 使用单一静态容器 `global.__http_registry`，所有响应通过 `registry[reqId]` 挂载
- JS 层 `delete __http_registry[reqId]` 是安全的索引属性删除，不会膨胀 shape 表

**Fix 11：mquickjs Heap 碎片防护**（JS 层 `runtime/http.js`）：

mquickjs 没有 compacting GC，大量动态创建/释放大字符串会碎片化 C heap。在 `ON_END` handler 中显式 nullify 大属性后再 delete：

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

**禁止的做法**（安全约束）：
- ~~`snprintf(buf, ..., "$bus.emit('http/end', '%s')", body)`~~ — body 含引号/换行会导致 SyntaxError
- ~~`snprintf(buf, ..., "$bus.emit('http/end', '%.*s')", body_len, body)`~~ — 同上

```
C 层 select() 每帧循环:
  while read() > 0:
    追加到 response_buf（不触发 JS）

  if read() == 0 (pipe closed / curl 退出):
    phr_parse_response() 解析 → 构建 response 对象
    dispatch ON_END → JS_Eval("$bus.emit('http/end', ...)")

  if read() == -1 (error):
    dispatch ON_ERROR → JS_Eval("$bus.emit('http/error', ...)")
```

### C→JS 通信：进度事件（ON_PROGRESS，按帧限频）

```
C 层每帧 select() 后:
  if total_bytes_read != last_dispatched_bytes:
    dispatch ON_PROGRESS → $bus.emit("http/progress", { reqId, loaded, total })
    last_dispatched_bytes = total_bytes_read
```

**按帧限频**：60fps 下每秒最多 60 次 dispatch，JS 侧完全可承受。

**进度字段**：
- `loaded`：已读字节数（必填）
- `total`：总字节数（可选，从 `Content-Length` 解析，未知时为 0）
- JS 侧：`total === 0` 时显示 "loading..."，否则显示百分比

### API 契约
```c
void kwcc_http_init();
void kwcc_http_request(const char *req_id, const char *method,
                       const char *url, const char **headers, int header_count,
                       const char *body, int body_len);
void kwcc_http_cancel(const char *req_id);
/* 内部函数：统一清理资源 + 收割子进程 */
static void kwcc_http_cleanup(kwcc_http_req_t *req);
```

### 僵尸进程清理 + 资源释放

**问题**：每个请求通过 `fork()` 创建独立 `kwcc_curl` 子进程，如果不 `waitpid()` 收割，退出的子进程会变成僵尸进程，耗尽系统进程描述符。

**方案**：实现 `kwcc_http_cleanup()` 统一销毁函数，在所有结束路径调用：

```c
static void kwcc_http_cleanup(kwcc_http_req_t *req) {
    if (req->pipe_read_fd >= 0) {
        close(req->pipe_read_fd);
        req->pipe_read_fd = -1;
    }
    kwcc_io_unregister(req->pipe_read_fd);
    waitpid(req->pid, NULL, WNOHANG);  /* 非阻塞收割，防止僵尸进程 */
    free(req->response_buf);
    free(req->body);
    for (int i = 0; i < req->header_count; i++) free(req->headers[i]);
    memset(req, 0, sizeof(kwcc_http_req_t));  /* 清空槽位，允许复用 */
}
```

**调用时机**（必须调用的路径）：
- `http_on_read()` 中 EOF（`n == 0`）后 → 循环解析 → dispatch → `kwcc_http_cleanup(req)`
- `http_on_read()` 中协议错误（`ret == -1`）→ dispatch error → `kwcc_http_cleanup(req)`
- `kwcc_http_cancel(req_id)` → `kill(SIGTERM)` → `waitpid(WNOHANG)` → `kwcc_http_cleanup(req)`

**关键点**：`waitpid` 必须用 `WNOHANG` 非阻塞标志，避免阻塞渲染主线程。

---

## Layer 3: Sokol Frame Hook

### 在 `frame()` 回调中插入

```c
void frame(void) {
    /* 1. I/O reactor polling (non-blocking) */
    kwcc_io_poll_once();

    /* 2. 原有 JS 处理 + microui 渲染 */
    kwcc_process_js(g_js_ctx, app_js_text);

    /* 3. 原有 NanoVG 渲染 */
    render_mu_commands();
}
```

**位置**：必须在 `kwcc_process_js` 之前，确保当帧的 pipe 数据在 JS 处理前被读取。

---

## Layer 4: JS Promise + http module (`app/runtime/http.js`)

### 4a. MiniPromise 实现（ES5 兼容）

```javascript
function MiniPromise(executor) {
    var self = this;
    self.status = "PENDING";
    self.value = null;
    self.callbacks = [];

    function doResolve(resolveFn, rejectFn) {
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

    doResolve();
}

MiniPromise.prototype.then = function(onFulfilled) {
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

### 4b. fetchAsync 封装（reqId 路由 + 进度监听）

```javascript
function fetchAsync(reqId, url, options) {
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
        var cleanup = function() {
            $bus.off("http/end", onEnd);
            $bus.off("http/error", onError);
            if (onProgress) {
                $bus.off("http/progress", onProgressFiltered);
            }
        };

        var onEnd = function(action, data) {
            if (data.reqId !== reqId) return;
            cleanup();
            resolve(data.response);
        };

        var onError = function(action, data) {
            if (data.reqId !== reqId) return;
            cleanup();
            reject(data.error);
        };

        var onProgressFiltered = function(action, data) {
            if (data.reqId !== reqId) return;
            onProgress(data.loaded, data.total);
        };

        $bus.on("http/end", onEnd);
        $bus.on("http/error", onError);
        if (onProgress) {
            $bus.on("http/progress", onProgressFiltered);
        }

        _native_http_request(reqId, method, url, headers, body);
    });
}
```

### 4c. $store 注册 http module

```javascript
registerModule("http", {
    state: { activeRequests: 0 },
    actions: {
        track: function(s, data) {
            if (data.action === "start") s.activeRequests++;
            else if (data.action === "done") s.activeRequests--;
        }
    },
    initEvents: function() {
        // http module 本身不处理具体请求，
        // 只是跟踪活跃请求数，用于 UI 显示 loading 指示器
    }
});
```

### 4d. 业务代码调用示例

```javascript
// 简单请求
fetchAsync("api_test", "https://httpbin.org/get").then(function(resp) {
    ui.label("Response: " + resp.body);
}).catch(function(err) {
    ui.label("Error: " + err);
});

// 带进度条的 POST
fetchAsync("upload", "https://example.com/upload", {
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
```

---

## 待论证问题

### 1. kwcc_curl 的编译与分发

| 问题 | 方案 |
|------|------|
| curl 源码是否引入 `deps/`？ | 是，从 `https://curl.se/download/curl-*.tar.gz` 下载，编译为独立可执行文件 |
| 编译产物放哪？ | 项目根目录或 `bin/` 目录 |
| macOS 签名/沙箱影响？ | 待验证：fork/exec 在 macOS 默认环境下是否正常 |

### 2. picohttpparser 集成

| 问题 | 方案 |
|------|------|
| 是否引入 `deps/picohttpparser/`？ | 是，2 个文件，MIT 许可，~400 行 |
| setup.sh 需要改动？ | 是，添加 picohttpparser 下载 |
| Makefile 需要改动？ | 是，`kwcc_http.o` 依赖 `picohttpparser.o` |
| 解析状态码 | picohttpparser **直接返回** 状态码到 `*status` 参数中（无需 sscanf 从首行提取） |
| Buffer 安全边界 | `realloc +4` 字节 + trailing `\0`，防止 `phr_parse_response` 越界扫描 |

### 3. 超时控制

| 问题 | 方案 |
|------|------|
| curl 无响应怎么办？ | curl 自带 `--max-time` 参数，C 层不需要额外处理 |
| DNS 解析慢？ | curl 默认行为，可通过 `--connect-timeout` 控制 |

### 4. HTTPS 支持

| 问题 | 方案 |
|------|------|
| 是否支持 HTTPS？ | 依赖 curl 是否支持。系统 curl 通常支持，打包版需确认编译时启用 SSL |
| 证书校验？ | curl 默认启用，可通过 `-k` 关闭（不推荐） |

### 5. 并发请求数

| 问题 | 方案 |
|------|------|
| 同时几个请求？ | `KWCC_IO_MAX_FDS = 16`，`KWCC_HTTP_MAX_REQS = 8`，最多 8 个并发 |
| 是否够用？ | UI 场景通常 1-3 个并发（API + 图片 + 其他），8 个足够 |

### 6. Bus 事件 vs Store dispatch

| 问题 | 方案 |
|------|------|
| 用 `$bus.emit` 还是 `$store.dispatch`？ | 用 `$bus.emit`：HTTP 响应不是应用 state，不应混入 store |
| 业务代码如何消费？ | 通过 `fetchAsync` 封装，业务代码不需要手动监听 bus 事件 |

---

## 实施步骤

1. **准备**: `setup.sh` 下载 picohttpparser 到 `deps/picohttpparser/`，更新 `Makefile`
2. **Layer 1**: 实现 `kwcc_io.h/c`（select + FD 管理）
3. **Layer 2**: 实现 `kwcc_http.h/c`（fork + pipe + curl 参数构造 + picohttpparser 增量解析 + 动态 buffer）
4. **Layer 3**: 在 `src/main.m` 的 `frame()` 中插入 `kwcc_io_poll_once()`
5. **Layer 4**: 实现 `app/runtime/http.js`（MiniPromise + fetchAsync + http module）
6. **C binding**: 在 `jsapi.c` 中注册 `_native_http_request`，参照 `js_kwcc_ui` 模式
7. **验证**: 在 test 模块中添加简单 HTTP 请求示例
8. `make clean && make` 验证编译 + 运行测试
