# picohttpparser 分析

> 来源：`https://github.com/h2o/picohttpparser`（MIT 许可）
> 文件：`picohttpparser.h` + `picohttpparser.c`（~400 行 C）

## 核心 API

### phr_parse_response（我们主要用的）

```c
int phr_parse_response(const char *_buf, size_t len,
                       int *minor_version,    // 输出：HTTP 小版本号（如 1 表示 HTTP/1.1）
                       int *status,           // 输出：HTTP 状态码（如 200）
                       const char **msg,      // 输出：状态消息指针（如 "OK"）
                       size_t *msg_len,       // 输出：状态消息长度
                       struct phr_header *headers,  // 输出：header 数组
                       size_t *num_headers,   // 输入/输出：最大 header 数 → 实际解析数
                       size_t last_len);      // 输入：上次已累积字节数（用于 slowloris 检测）
```

**返回值**：
- `> 0`：成功，返回 header 结束的字节偏移（即 body 起始位置）
- `-2`：数据不完整，需要更多字节，下帧继续
- `-1`：协议错误（格式不正确）

**关键**：picohttpparser **直接返回状态码**到 `*status`，不需要额外 sscanf！这比方案文档中说的更简单。

### phr_header 结构

```c
struct phr_header {
    const char *name;      // header 名指针（指向 buf 内部，非 NULL 结尾）
    size_t name_len;       // 名称长度
    const char *value;     // header 值指针（指向 buf 内部，非 NULL 结尾）
    size_t value_len;      // 值长度
};
```

**注意**：所有字符串指针都是 **零拷贝** 的，直接指向输入 buffer 内部。使用时需要用 `strndup` 或 `JS_NewStringLen` 提取。

### phr_parse_request / phr_parse_headers

- `phr_parse_request`：解析 HTTP 请求（服务器端用，我们不需要）
- `phr_parse_headers`：仅解析 header 部分（无状态行）

### phr_decode_chunked

```c
ssize_t phr_decode_chunked(struct phr_chunked_decoder *decoder, char *buf, size_t *bufsz);
```

用于解码 chunked transfer encoding。返回 `-2` 表示不完整，`-1` 错误，`>= 0` 表示成功（返回值是未解码的剩余字节数）。

**我们暂时不需要**：因为 curl `-i` 输出会自动解 chunked，但如果未来替换为自研 socket 实现，可能需要处理。

## 使用模式

```c
struct phr_header headers[64];
size_t num_headers = 64;
int minor_version, status;
const char *msg;
size_t msg_len;

int ret = phr_parse_response(buf, buf_len,
    &minor_version, &status, &msg, &msg_len,
    headers, &num_headers, prev_len);

if (ret == -2) {
    // 数据不完整，继续累积
} else if (ret == -1) {
    // 协议错误
} else {
    // ret = body 起始偏移
    // status = HTTP 状态码（如 200）
    // headers[0..num_headers-1] 包含所有 header
    // body = buf + ret
}
```

## 增量解析机制

`last_len` 参数是上次已累积的字节数。如果 `last_len != 0`，解析器会先快速检查 `\r\n\r\n` 是否已出现（`is_complete()` 函数），避免完整解析不完整的帧。

**我们的用法**：`last_len` 传 `req->response_len`（当前已读字节数），每帧累积后重新调用 `phr_parse_response`，直到返回 `> 0` 或 `-1`。

## 与 mquickjs 集成要点

1. **header 值提取**：`headers[i].name` 和 `headers[i].value` 不是 NULL 结尾，需要用 `JS_NewStringLen(ctx, ptr, len)` 创建 JS 字符串
2. **状态码**：直接 `JS_NewInt32(ctx, status)` 即可
3. **body**：`buf + ret` 开始的字节，长度 `response_len - ret`，用 `JS_NewStringLen` 创建
4. **零拷贝优势**：不需要复制 header 字符串，直接从 buffer 指针创建 JS 值
5. **JS response 对象构建**：
   ```c
   JSValue resp = JS_NewObject(ctx);
   JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, status));
   JS_SetPropertyStr(ctx, resp, "body", JS_NewStringLen(ctx, body_start, body_len));
   JSValue headers_obj = JS_NewObject(ctx);
   for (int i = 0; i < num_headers; i++) {
       JS_SetPropertyStr(ctx, headers_obj,
           /* header name as key */,
           JS_NewStringLen(ctx, headers[i].value, headers[i].value_len));
   }
   JS_SetPropertyStr(ctx, resp, "headers", headers_obj);
   ```

## 编译集成

- 只需 include `picohttpparser.h` 和编译 `picohttpparser.c`
- 依赖：`<stdint.h>`, `<sys/types.h>`, `<string.h>`, `<stddef.h>`
- 可选 SSE4.2 优化（自动检测 `__SSE4_2__` 宏，macOS 默认启用）
- 无任何外部库依赖

## 关键限制：只支持 HTTP/1.x

`parse_http_version()` 函数（picohttpparser.c:280-296）**硬编码检查 `HTTP/1.`**：

```c
EXPECT_CHAR_NO_CHECK('H');
EXPECT_CHAR_NO_CHECK('T');
EXPECT_CHAR_NO_CHECK('T');
EXPECT_CHAR_NO_CHECK('P');
EXPECT_CHAR_NO_CHECK('/');
EXPECT_CHAR_NO_CHECK('1');    // ← 硬编码 '1'，遇到 HTTP/2 时 '2' != '1' 返回 -1
EXPECT_CHAR_NO_CHECK('.');
PARSE_INT(minor_version, 1);
```

**后果**：如果 curl 使用 HTTP/2 协议，`-i` 输出的状态行是 `HTTP/2 200`，picohttpparser 解析失败返回 -1，headers 全部丢失。

**解决方案**：curl argv 必须加 `--http1.1` 强制 HTTP/1.1 协议。这不是用户偏好，是解析能力的硬约束。

**实测**：不加 `--http1.1` 时 headers=0（解析失败），加上后 headers=7（正常解析）。

## 返回值速查

| 返回值 | 含义 | 处理方式 |
|--------|------|----------|
| `> 0` | 成功，body 起始偏移 | 解析完成，dispatch ON_END |
| `-2` | 数据不完整 | 继续累积，下帧再试 |
| `-1` | 协议错误 | dispatch ON_ERROR |
