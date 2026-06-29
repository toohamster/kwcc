# 设计决策记录：Facade + Plugin 架构 + $notify 通知通道 + HTTP 模块

> 记录设计讨论过程，解释"为什么这么设计"。
> 方案文档（`requirements/js-bridge-architecture.md`、`requirements/js-http-implementation-plan.md`）写结论，这里写原因。

---

## Part A：C-JS 架构拆分（Facade + Plugin）

---

### A1：为什么需要 Facade + Plugin 架构

**问题**：当前 `kwcc_js.c/h` 直接暴露 mquickjs 类型（`JSContext`、`JSValue`、`JSCStringBuf` 等）给扩展模块。每个扩展模块都必须 `#include "mquickjs/mquickjs.h"` 并直接调用 mquickjs API。

**决策**：参考 Linux 内核 core + module 模式：`kwcc_js` 封装 mquickjs 为 Facade（`kwcc_js_ops_t`），扩展模块遵循 `kwcc_js_module_t` 规范注册进 core，不直接碰 mquickjs。

**原因**：

1. **强耦合**：换引擎需要改所有模块。每个模块都直接依赖 mquickjs 的 API、类型、include 顺序
2. **重复踩坑**：每个模块都要处理 `JSCStringBuf`、`JS_StackCheck` + `JS_PushArg` + `JS_Call` 三步调用等 mquickjs 特有问题
3. **注册散乱**：每个模块暴露自己的 `kwcc_register_xxx_js()` 让外部调，没有统一规范

---

### A2：`kwcc_js_val_t` 为什么是 `typedef uint64_t`

**问题**：子模块需要操作 JS 值，但不应该直接用 `JSValue`。

**决策**：`typedef uint64_t kwcc_js_val_t;`

**原因**：

- mquickjs 的 `JSValue` 在 64 位平台上就是 `uint64_t`（已从源码验证）
- `typedef` 提供编译期类型隔离（子模块 include `kwcc_js.h` 而非 `mquickjs.h`）
- 实际二进制兼容，零运行时开销
- 子模块不直接操作 `JSValue`，只通过 `kwcc_js_ops_t` 的函数指针操作

---

### A3：`kwcc_js_cstr_buf_t` 为什么是 `buf[5]` 且不需要 free

**问题**：`JS_ToCString` 在 mquickjs 中是三参数版本（ctx, val, &buf），短字符串内联到 buf 中。子模块如何使用？

**决策**：`typedef struct { char buf[5]; } kwcc_js_cstr_buf_t;`

**讨论过程**：

1. 最初文档写 `buf[8]`，实际验证 mquickjs 源码发现是 `buf[5]`
2. `buf[5]` 对应短字符串阈值：单个 Unicode code point 最多 4 字节 UTF-8 + 1 字节 `\0`
3. mquickjs 没有 `JS_FreeCString`，长字符串指针由 GC 管理

**原因**：

- `buf[5]` 精确匹配 mquickjs 的 `JSCStringBuf` 定义
- 不需要 free：短字符串指向调用方的 buf，长字符串指向 JS 内部内存（GC 管理）
- 但长字符串指针不能跨作用域长期保存——用完即止，不要缓存
- 保持两步调用模式（调用方在栈上声明 buf，传给 `to_cstring`），和原始 `JS_ToCString` 一致

---

### A4：`kwcc_js_ops_t` 函数指针第一个参数为什么是 ops 自身

**问题**：函数指针签名为什么是 `new_object(ops)` 而不是 `new_object(ctx)`？

**决策**：所有函数指针第一个参数都是 `ops` 自身，子模块不需要知道 `ctx`。

**原因**：

- `ctx` 是 mquickjs 内部类型，暴露给子模块违反 Facade 原则
- `ops` 内部持有 `ctx`（`ops->ctx`），实现侧可以取到
- 子模块只持有 `ops` 指针，所有操作通过 `ops` 完成
- 和 Linux 内核的 file_operations 模式一致：ops 自引用

---

### A5：`call_cb` 为什么封装 `JS_StackCheck` + `JS_PushArg` + `JS_Call` 三步

**问题**：mquickjs 调用 JS 函数需要三步：`JS_StackCheck` → `JS_PushArg`（逐个压参数）→ `JS_Call`。子模块每次都要重复这个模式。

**决策**：`call_cb` 内部处理三步，子模块只传参数数组。

**原因**：

- 三步调用是 mquickjs 特有 API，不是通用 JS 引擎模式
- 每个模块重复写容易出错（忘记 StackCheck、参数顺序搞反等）
- 封装后子模块只需：`ops->call_cb(ops, fn, argc, argv)`

---

### A6：`kwcc_js_module_t` 为什么用描述符而非继承

**问题**：模块注册机制怎么设计？

**决策**：

```c
typedef struct kwcc_js_module {
    const char *name;
    void (*load)(kwcc_js_ops_t *ops);
    void (*register_cfun)(kwcc_js_ops_t *ops);
    void (*on_bus_event)(const char *topic, const void *data, size_t len, kwcc_js_ops_t *ops);
    void (*unload)(kwcc_js_ops_t *ops);  /* 可为 NULL */
} kwcc_js_module_t;
```

**原因**：

- C 语言没有继承，描述符（函数指针集合）是标准的模块注册模式
- 和 Linux 内核的 `module_init`/`module_exit` 思路一致
- `unload` 可为 NULL：简单模块（如 HTTP）不需要退出清理，WS/Timer 等管理长连接/定时器的模块必须实现
- 模块描述符是唯一的对外接口，不暴露内部函数

---

### A7：bus 事件分发为什么遍历所有模块而非按前缀路由

**问题**：`kwcc_js_on_bus_event` 收到事件后，如何分发给对应模块？

**决策**：遍历所有模块，每个模块的 `on_bus_event` 内部自行判断 topic 前缀。core 不做前缀匹配分发。

**原因**：

- 更灵活：模块可以订阅任意 topic，不限于自己的前缀
- 当前模块数量少（2-3 个），O(n) 遍历开销可忽略
- 如果未来模块数增长到十几个，可让模块在描述符中声明 `topic_prefix` 数组，core 按前缀做 O(1) 路由表分发——当前阶段不提前优化

---

## Part B：$notify 通知通道

---

### B1：C 端不存 JSValue 回调，回调映射移到 JS 端

**问题**：旧设计中，C 端通过 `g_kwcc_http_cbs` 存储 JSValue 回调（`on_end_cb`、`on_error_cb`、`on_progress_cb`），bus 事件到达时 C 端查注册表、调 `JS_Call`。

**决策**：C 端不存任何 JSValue 回调。JS 端通过 `$http.callbacks[id]` 自己做 id→resolve/reject 映射。

**原因**：

1. **GC 风险**：C 侧 static 变量里的 JSValue 不在 GC root set 中，如果 JS 侧没人引用了，GC 可能回收 → 悬空指针。旧代码碰巧安全是因为时序上 C 先用完才清理，但逻辑上有隐患
2. **职责清晰**：C 端只管"发生了什么"，不管"谁在等"。回调映射是纯 JS 逻辑，不应该跨语言边界
3. **新模块零 C 侧改动**：新模块只需加一行 `$notify.on(type, handler)` 注册，不改 core 代码

---

### B2：`$notify` 作为通用 C→JS 通知通道，不走 `$bus.emit`

**问题**：C→JS 通知（HTTP 响应、WS 消息、Timer 触发）如何到达 JS 端？

**决策**：新增 `$notify` 通道，和 `$bus`（JS→JS）对称。C 端通过 `ops->notify_js` 直达 `$notify.emit`。

**原因**：

1. `$bus.emit` 需要字符串拼接 topic + 数据 → 有注入风险和性能开销
2. `$bus.emit` 内部要做 topic 匹配 + 遍历订阅者 → 多一层间接
3. C→JS 通知是原生事件，和 JS→JS 业务事件职责不同

| 通道 | 方向 | 场景 |
|------|------|------|
| `$notify` | C → JS | 原生事件（HTTP 响应、WS 消息、Timer 触发） |
| `$bus` | JS → JS | 业务事件（按钮点击、state 变更） |

---

### B3：`notify_js` 的 `ack_cleanup` 参数

**问题**：`dispatch_end` 后 C 侧 buffer 何时释放？如果立刻 cleanup，JS 端还没读数据就没了；如果不 cleanup，谁负责释放？

**决策**：`notify_js` 接受 `ack_cleanup` 函数指针参数，在投递前自动调用。传入 NULL 表示无需清理（如 progress 事件）。

**讨论过程**：

1. 最初方案：dispatch_end 后立刻 cleanup，JS 端在 bus 回调中读数据 → 时序依赖隐式
2. 提出 ACK/NACK 机制：JS 端确认收到后才 cleanup → 过度设计，HTTP 是单消费者，不存在竞争
3. 提出 `ack_cleanup` 自动释放：数据通过 `JS_NewStringLen` 拷贝进 GC 堆后，C 侧 buffer 就没用了，投递前直接释放 → 简单明确

**命名讨论**：

- `release`：只表达了动作，没表达时机
- `ack`：表达了时机确认，但 HTTP 场景不是异步握手
- `ack_cleanup`：`ack` 表达"确认投递"的时序含义，`cleanup` 表达具体动作（释放 buffer）→ 最精确

**原因**：

1. **不是异步握手**：数据通过 `JS_NewStringLen` 拷贝进 GC 堆后，C 侧 buffer 就没用了，可以直接释放
2. **调用方不需要记着调 release**：传了 `ack_cleanup` 就自动处理
3. `ack_cleanup` 内部失败时（如 req_id 已被清理）应 log_warn 并安全返回，不影响后续 `call_cb` 执行

---

### B4：`s_notify_emit_fn` 用 static 变量缓存，不需要 GC 保护

**问题**：`js_notify_js_impl` 内部需要拿到 `$notify.emit` 的 JSValue 来调 `call_cb`。如何持有这个 JSValue？是否需要 GC 保护？

**讨论过程**：

1. **最初方案**：缓存到 `static kwcc_js_val_t s_notify_emit_fn`，加 `JS_AddGCRef` 保护
2. **质疑**：`s_notify_emit_fn` 和旧代码的 `on_error_cb` 是同一种模式——C 端持有 JSValue 回调，需要的时候 `JS_Call`。为什么它需要特殊处理？
3. **GC 机制分析**：mquickjs 用 mark-and-sweep GC。从 root set（全局对象）出发遍历，可达的对象不回收。`global.console.log`、`global.Math.abs`、`global.JSON.parse` 等内置函数不被 GC，不是因为"引擎函数有特权"，而是因为它们挂在 global 上，属性链从 root set 可达
4. **`$notify.emit` 同理**：`global.$notify.emit` 和 `console.log` 完全一样——挂在 global 上，从 root set 永远可达，GC 永远不会回收
5. **`on_error_cb` 的隐患**：旧代码的 `on_error_cb` 实际上有潜在 GC 风险——如果 JS 侧先 `delete $http.callbacks[id]`，C 侧的 JSValue 就只剩 static 变量引用，GC 不知道，可能回收。只是时序上碰巧安全。新设计用 `$notify` + JS 端 `$http.callbacks` 彻底消除了这个问题
6. **是否缓存 vs 现取**：每次现取多 2 次 `get_str_prop`，但 `notify_js` 不是热路径。缓存多一个 static 变量但更简洁。最终选择缓存——和 `on_error_cb` 同等模式，代码更直观

**决策**：

- `s_notify_emit_fn` 用 static 变量缓存，不需要 `JS_AddGCRef` 保护
- 不作为 ops 属性暴露给子模块——它是 `notify_js` 的实现细节，不是公共接口
- 不需要每次现取（2 次 `get_str_prop`），缓存更简洁

**关键认知**：mquickjs GC 回收规则

- GC 从 root set（全局对象）出发遍历，可达的标记存活，不可达的回收
- C 侧 static 变量不在 root set 中，GC 不知道它的存在
- 但如果 JSValue 从 global 属性链可达，不管 C 侧缓存不缓存，GC 都不会回收
- `global` 上的属性只有在 ctx 销毁时才释放，ctx 重建时重新初始化
- 内置函数（`console.log`、`Math.abs`、`JSON.parse`）和用户定义函数（`$notify.emit`）在 GC 眼里没有区别，不被回收都是因为从 root set 可达

**C 端持有 JSValue 的两种情况**：

| | JS 侧是否一直持有 | C 侧缓存安全吗 | 逻辑上需要 JS_AddGCRef 吗 |
|---|---|---|---|
| `on_error_cb`（旧设计） | 不一定（可能被 delete） | 碰巧安全 | 需要（旧代码没做） |
| `$notify.emit`（新设计） | 一定（global 常驻） | 绝对安全 | 不需要 |

---

### B5：`$notify` 和 `$http` 注入方式一致

**问题**：`$notify` 和 `$http` 对象如何创建？

**决策**：都用 C API：`ops->new_object` + `ops->set_str_prop`，然后 `ops->eval` 注入方法。不用 `var $http = new Object()` 这种纯 JS 方式。

**讨论过程**：

1. 最初 `http_load` 用 `var $http = new Object()` + eval 注入，`$notify` 也用同样方式
2. 用户指出 `kwcc_register_config_js()` 用 `JS_NewObject` + `JS_SetPropertyStr` 更清晰
3. 统一为 C API 方式：对象所有权和生命周期由 C 端控制，不依赖 JS 代码执行顺序

**原因**：和 `kwcc_register_config_js()` 风格一致。C API 创建对象更明确。

---

### B6：`$notify.on('http', 匿名函数)` 而非 `$http.onNotify`

**问题**：JS 端处理器怎么注册？

**决策**：`http.js` 中用匿名函数：`$notify.on('http', function(event, id, data) { ... })`。不用 `$http.onNotify` 这种具名方法。

**讨论过程**：

1. 最初用 `$http._onNotify`：`_` 前缀表示私有，但 mquickjs 没有真正的访问控制
2. 改为 `$http.onNotify`：去掉 `_` 前缀，但暴露了内部实现细节
3. 最终用匿名函数：处理逻辑和 `$http.callbacks` 映射紧密相关，放在匿名函数里内聚性更好

**原因**：

1. 具名方法暴露了内部实现细节，外部不应该直接调
2. 处理逻辑和 `$http.callbacks` 映射紧密相关，匿名函数内聚性更好
3. **变量命名不以 `_` 开头**：这是项目规则，JS/C 都要遵守。`_` 前缀是其他语言（Python）的私有约定，mquickjs/ES5 没有真正的访问控制，用 `_` 前缀只是自欺欺人，不如直接用匿名函数或不暴露

---

### B7：`call_cb` 内部做 JS_IsException 检查

**问题**：JS handler 抛异常会怎样？

**决策**：`call_cb` 内部 `JS_Call` 返回后检查 `JS_IsException(ret)`：如果是异常，log_warn 记录并清除（`JS_GetException`），防止异常状态累积影响后续 JS 执行。

**原因**：子模块不应该处理 JS 异常。如果 `call_cb` 不捕获，异常状态会累积在 ctx 里，影响后续所有 JS 执行。统一在 `call_cb` 里处理，子模块只管调，不用关心异常。

---

### B8：`$notify` 不经过 `bus/js_whitelist`

**问题**：`$notify` 是否需要白名单过滤？

**决策**：不需要。`$notify` 是 C→JS 的直达通道，由 C 端 `ops->notify_js` 触发，信任边界是模块代码本身。

**原因**：如果未来有不可信的数据源需要过滤，应在模块 handler 内部做，而不是在 `$notify` 层加白名单——这里的设计意图是原生事件的零开销直达，不应该增加过滤层。

---

## Part C：HTTP 模块设计

---

### C1：`kwcc_http_result_t` 为什么隐藏 `phr_header`

**问题**：`kwcc_http_result_t` 当前暴露 `const struct phr_header *headers`，导致消费者必须 include `picohttpparser.h`。

**决策**：删除 `headers` 和 `num_headers` 字段，替换为 header 访问 API：

```c
int kwcc_http_result_header_count(const char *req_id);
int kwcc_http_result_get_header(const char *req_id, int index,
                                const char **name, int *name_len,
                                const char **value, int *value_len);
```

**原因**：

- 暴露 `phr_header` 违反 Layer 2 不依赖解析库实现细节的原则
- `kwcc_http.h` 的消费者（`kwcc_js_http.c`）不应该知道 picohttpparser 的存在
- header 访问 API 隐藏了内部实现，换解析库不需要改消费者

---

### C2：删除回调注册表（`g_kwcc_http_cbs`）

**问题**：`kwcc_http.c` 中有 `g_kwcc_http_cbs` + `register/find/clear_callback`。回调注册是否属于 HTTP 服务？

**决策**：删除。回调注册不属于 HTTP 服务，属于 JS 桥接层。

**原因**：

1. HTTP 服务的职责是发请求、监控进度、publish bus 事件。它不应该知道谁在等结果
2. 回调注册表是 JS 桥接层的需求，放在 HTTP 服务里违反职责划分
3. 新设计中 JS 端自己维护 `$http.callbacks` 做映射，C 端不需要回调注册表

---

### C3：`kwcc_http_check_progress` 传 `kwcc_http_progress_t` 结构体作为 bus data

**问题**：progress 事件的数据从哪来？

**决策**：`kwcc_http_check_progress` 传 `kwcc_http_progress_t` 结构体作为 bus data。progress 事件的数据来源是 bus data，end/error/cancel 事件的数据来源是 `kwcc_http_get_result`。

**讨论过程**：

1. 最初 progress 也走 `kwcc_http_get_result`，但请求还在进行中没有完整结果
2. 提出 `kwcc_http_get_progress` 新 API → 过度设计，bus data 已经携带了 payload
3. 最终：`on_bus_event` 的 `data` 参数就是 `kwcc_http_progress_t *`，直接读 `loaded`/`total`

| 事件类型 | 数据来源 | ack_cleanup |
|----------|----------|-------------|
| end/error/cancel | `kwcc_http_get_result` | `kwcc_http_release_result` |
| progress | bus data（`kwcc_http_progress_t *`） | NULL |

---

### C4：`kwcc_http_on_read` 增量解析 header 提取 Content-Length

**问题**：progress 事件需要 total 值来做百分比，但响应头中才有 Content-Length。

**决策**：`kwcc_http_on_read` 增量解析 header 提取 `Content-Length` 存到 `req->total_size`。

**原因**：

- 不需要等完整响应，第一次 pipe 数据到达时就能解析出 Content-Length
- 后续 progress 事件直接读 `req->total_size`

---

### C5：无 Content-Length 哨兵值 `total: -1`

**问题**：chunked 编码没有 Content-Length，JS 端做百分比计算会除零。

**决策**：`kwcc_http_progress_t.total` 在无 Content-Length 时设为 `-1`。JS 端 `$http._onNotify` 的 progress 分支检查 `data.total === -1` 时只传 `loaded`。

**原因**：

- `-1` 是合法的哨兵值（loaded/total 不可能为负）
- 比 `0` 更明确（`0` 可能表示"还没拿到"）
- JS 端据此跳过百分比计算，避免除零

---

### C6：僵尸进程回收

**问题**：`kwcc_http_cleanup` 中 `waitpid(WNOHANG)` 单次尝试可能回收不到（cancel 后子进程还没退出），导致僵尸进程。

**决策**：`kwcc_http_check_progress` 中加一轮对所有 `in_use` slot 的 `waitpid(pid, NULL, WNOHANG)` 扫描。如果 `waitpid` 返回 `> 0`（子进程已退出），发布 `http/end` 或 `http/error` 事件并调 cleanup。

**原因**：

- `check_progress` 每帧调用，是天然的回收时机
- 单次 `waitpid` 在 cleanup 中不够可靠，子进程退出是异步的
- 扫描所有 `in_use` slot 确保不遗漏

---

### C7：PENDING_ACK 状态 + `kwcc_http_release_result`

**问题**：`dispatch_end` 后立刻 cleanup，JS 端还没读数据 buffer 就被释放了。但同步 bus 下实际安全，只是时序依赖是隐式的。

**决策**：`dispatch_end` 后不再立刻 cleanup，标记 `PENDING_ACK`。新增 `kwcc_http_release_result(req_id)` 供 `ack_cleanup` 调用，释放 buffer + cleanup。`check_progress` 中加 `PENDING_ACK` 超时兜底。

**原因**：

1. **显式时序契约**：数据通过 `JS_NewStringLen` 拷贝进 GC 堆后才调 `ack_cleanup`，不需要等 JS 端确认
2. **超时兜底**：如果消息丢失导致 slot 泄漏，超时后自动 cleanup
3. **支持同步和异步 bus**：即使未来 bus 变成异步，这个契约也成立

---

### C8：cancel 事件必须触发 JS 端 reject

**问题**：`kwcc_http_cancel` 发布 `http/cancel/<reqId>` 后，如果 JS 端不处理 cancel 事件，Promise 永久悬挂。

**决策**：JS 端的 `$notify.on('http', ...)` handler 必须处理 `cancel` 事件（和 `error` 一样调 `reject` + `delete callbacks[id]`）。

**原因**：

- cancel 和 error 一样是终止事件，必须触发 reject 才能释放 Promise
- 否则 Promise 永久悬挂，`$http.callbacks[id]` 永远不会被 delete，内存泄漏

---

### C9：MiniPromise 独立为 `promise.js`

**问题**：MiniPromise 放在 `http.js` 还是独立文件？

**决策**：独立为 `app/runtime/promise.js`。

**原因**：

- MiniPromise 是通用基础设施，不与 HTTP 耦合
- 未来任何需要异步的场景（WebSocket、定时器链式调用等）都可复用
- `$http.fetch` 只负责使用 MiniPromise，不负责定义它

---

### C10：`http_load` 只注入 C→JS 桥接方法，纯 JS 逻辑在 `http.js`

**问题**：`$http.callbacks`、`$notify.on` handler、`fetch` 方法应该在哪里定义？

**决策**：

- C 端（`http_load`）：`ops->new_object` 创建 `$http` + 注入 C→JS 桥接方法（`cancel`/`config`）
- JS 端（`http.js`）：`callbacks`/`$notify.on`/`fetch` — 纯 JS 逻辑

**原因**：

- C 端只负责桥接（JS→C 调用），不负责业务逻辑
- 回调映射、Promise 创建、事件处理都是纯 JS 逻辑，放在 JS 文件里更灵活、可读
- 和 `kwcc_register_config_js` 风格一致：C 端创建对象壳 + 注入桥接，JS 端定义业务方法

---

### C11：`kwcc_js.c` 精简 include，删除 `kwcc_http.h` 和 `picohttpparser.h`

**问题**：重构后 `kwcc_js.c` 不再有 HTTP 业务代码，但 include 列表还残留 `kwcc_http.h` 和 `picohttpparser.h`。

**决策**：删除这两个 include。core 不依赖具体模块的服务层。

**原因**：

1. **依赖方向正确**：core 不知道 HTTP 的存在，模块依赖 core，不是反过来
2. **编译隔离**：`kwcc_http.h` 改了不需要重编 `kwcc_js.c`
3. **职责清晰**：core 只管 JS 生命周期 + ops + 模块注册 + bus 分发，不碰任何模块业务
4. 如果不精简，Facade 就是半成品——代码虽然移走了 HTTP 逻辑，但编译依赖还在
