# Async I/O Plan — Plan Mode 记录

> 状态：从 plan mode 会话恢复，待用户确认
> 恢复时间：2026-06-02
> 来源：plan mode 对话内容（系统提示中显示的摘要）

---

## Plan Mode 显示的最终摘要（原文）

```
# Async I/O Implementation Plan

> **This plan file is superseded.**
> The authoritative, up-to-date plan is now:
> **`requirements/async-io-implementation-plan.md`** — full 6-step plan with all three defensive fixes applied.
>
> **`requirements/async-io-promise.md`** — scheme document updated with mquickjs API corrections.

## Defensive Fixes Applied

| # | Fix | Status |
|---|-----|--------|
| 1 | **picohttpparser Buffer Margin** — `realloc + 4` + trailing `\0` | Applied |
| 2 | **Ban JS_Eval string injection** — global object pattern, C API for body/headers | Applied |
| 3 | **Follow-Redirects** — curl `-L` flag added to argv | Applied |

## Next: Begin coding Step 1 (picohttpparser) and Step 2 (I/O Reactor)
```

---

## 3 个 Defensive Fixes 的详细内容（来自方案文件已有代码 + plan 确认）

### Fix 1: picohttpparser Buffer Margin

**问题**：`phr_parse_response()` 内部做指针扫描（`scan_quote`、`is_token_char`），如果 buffer 末尾没有 null 终止符可能越界读取。

**实现**：
- `realloc` 时多分配 4 字节：`malloc(response_len + n + 4)`
- 每次追加数据后设置 `response_buf[response_len] = '\0'`
- 这确保 SIMD/指针扫描不越界

**代码位置**：`http_on_read()` 中的 read→append 逻辑

---

### Fix 2: Ban JS_Eval String Injection

**问题**：用 `JS_Eval` + 字符串拼接传递 body/header 数据，body 中的引号、换行会破坏 JS 语法，导致 `SyntaxError`。

**安全原则**：
- **禁止**用 `JS_Eval` + 字符串拼接传递 body/header 数据
- **允许**用 `JS_Eval` + 全局对象传递变量名（变量名不含用户数据，语法安全）
- 复杂对象（body、headers）必须通过 C API 构建后挂到全局对象上

**实现**：`http_dispatch_end()` 中：
1. 用 `JS_NewObject()` + `JS_SetPropertyStr()` + `JS_NewStringLen()` 构建 response 对象
2. 挂载到全局对象：`__http_resp_<req_id>`（唯一命名，防止并发污染）
3. 用 `JS_Eval` 执行 `$bus.emit('http/end', new Object(), __http_resp_req_xxx)`
4. **必须清理**：`delete global.__http_resp_<req_id>` 或置为 null

**关键**：为什么需要 `<req_id>` 隔离变量名：
- 如果同时有 2+ 个 fetchAsync 在同一帧或相邻帧返回
- 使用单一全局名 `__http_resp` 会被后一个请求覆盖
- 用 `__http_resp_<req_id>` 隔离，每个请求有独立的全局变量

---

### Fix 3: Follow-Redirects

**问题**：HTTP 301/302 重定向时，中间响应头会泄漏到 picohttpparser，导致解析错误。

**实现**：
- curl argv 中添加 `-L` flag：`curl -s -L -i -X <method> ...`
- `-L` 让 curl 自动跟随重定向，最终响应才是 picohttpparser 需要解析的内容

**代码位置**：`kwcc_http_request()` 中构建 curl argv 的逻辑

---

## 下一步

- Begin coding Step 1 (picohttpparser) and Step 2 (I/O Reactor)
- **前提**：确认本文件内容后，更新 requirements 文件，然后开始编码

---

## Plan Mode 新增修复（2026-06-02）

### Fix 4: 循环解析跳过中间响应（302/301 → 200）

**问题**：`curl -i -L` 遭遇 HTTP 重定向时，输出会包含所有中间响应（302 + 200），`phr_parse_response` 从 buffer 开头只解析第一套（302），导致 JS 层拿到 302 状态码和错误 body。

**方案**：EOF 后循环调用 `phr_parse_response`，每次从上次返回值（ret）继续解析，直到 buffer 剩余部分不再以 "HTTP/1.x" 开头：

```c
/* EOF 后：循环跳过所有中间响应，找到最终响应 */
int ret = 0;
int final_status = 0;
struct phr_header final_headers[64];
size_t final_num_headers = 64;

const char *p = req->response_buf;
size_t remaining = req->response_len;

while (remaining >= 9 && memcmp(p, "HTTP/1.", 7) == 0) {
    size_t nh = 64;
    int mv, st;
    const char *m;
    size_t ml;
    int r = phr_parse_response(p, remaining, &mv, &st, &m, &ml,
                               final_headers, &nh, 0);
    if (r <= 0) break;
    ret += r;
    final_status = st;
    final_num_headers = nh;
    p = req->response_buf + ret;
    remaining = req->response_len - ret;
}

/* 循环结束：final_status = 最终状态码，p = body 起始指针 */
http_dispatch_end(req, 0, final_status, p, (int)remaining, final_headers, final_num_headers);
```

**工作原理**：
1. 第一次 `phr_parse_response` → 解析 302 → ret = 302 headers 结束位置
2. 指针前进 → 再次调用 → 解析 200 → ret 更新
3. 指针再前进 → 剩余部分是 body → 不再匹配 "HTTP/1." → 循环退出
4. 最后一次成功解析的就是最终响应

**安全边界**：`remaining >= 9`（HTTP/1.x 响应行最小 9 字节），`memcmp(p, "HTTP/1.", 7)` 快速判断。

---

### Fix 5: 僵尸进程清理 + 资源释放

**问题**：每个请求通过 `fork()` 创建独立子进程，如果不 `waitpid()` 收割，退出的子进程会变成僵尸进程，耗尽系统进程描述符。

**方案**：实现 `kwcc_http_cleanup()` 统一销毁函数，在所有结束路径调用：

```c
static void kwcc_http_cleanup(kwcc_http_req_t *req) {
    /* 1. 关闭 pipe 读端 */
    if (req->pipe_read_fd >= 0) {
        close(req->pipe_read_fd);
        req->pipe_read_fd = -1;
    }
    /* 2. 从 I/O Reactor select 监听池解绑 */
    kwcc_io_unregister(req->pipe_read_fd);
    /* 3. 非阻塞收割子进程，防止僵尸进程 */
    waitpid(req->pid, NULL, WNOHANG);
    /* 4. 释放动态资源 */
    free(req->response_buf);
    free(req->body);
    for (int i = 0; i < req->header_count; i++) {
        free(req->headers[i]);
    }
    /* 5. 清空槽位，允许复用 */
    memset(req, 0, sizeof(kwcc_http_req_t));
}
```

**调用时机**（必须调用的路径）：
- `http_on_read()` 中 EOF（`n == 0`）后 → dispatch → `kwcc_http_cleanup(req)`
- `http_on_read()` 中协议错误（`ret == -1`）→ dispatch error → `kwcc_http_cleanup(req)`
- `kwcc_http_cancel(req_id)` → `kill(SIGTERM)` → `waitpid(WNOHANG)` → `kwcc_http_cleanup(req)`

**关键点**：`waitpid(req->pid, NULL, WNOHANG)` 必须用 `WNOHANG` 非阻塞标志，避免阻塞渲染主线程。桌面引擎挂机运行几天几夜不会漏任何资源。
