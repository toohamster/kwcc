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
