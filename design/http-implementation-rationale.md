# 设计决策记录：HTTP 模块实施阶段

> 方案文档（`requirements/js-http-implementation-plan.md`）写"做什么"，这里写"为什么"。
> 设计阶段决策在 `facade-plugin-architecture-rationale.md` Part C，这里记录实施阶段浮现的决策。

---

## 决策 1：`--http1.1` 硬编码，不做配置项

### 问题

curl 默认使用 HTTP/2，但 picohttpparser 只能解析 HTTP/1.x。是否把 HTTP 版本做成配置项？

### 决策

硬编码 `--http1.1`，不做配置项。

### 原因

1. **picohttpparser 的 HTTP/1.x 限制是源码级硬约束**：`parse_http_version()` 在 `picohttpparser.c:286` 有 `EXPECT_CHAR_NO_CHECK('1')`，遇到 `HTTP/2` 直接返回 -1。不是偏好，是能力边界。

2. **配置项应该对应真实可用的选项**：允许 HTTP/2 配置会导致功能不可用——headers 始终为 0，body 开头包含 `HTTP/2 200` 状态行。放一个打开就坏掉的开关没有意义。

3. **如果未来换解析器支持 HTTP/2**：届时再提取为配置。当前没有第二个选项，不需要配置。

### 教训

"picohttpparser 只能解析 HTTP/1.x"最初是未验证的假设，被用户质问后才去读源码确认。**对第三方库的行为假设，必须读源码验证，不能凭经验猜**。

---

## 决策 2：默认 bin_path 用完整路径 `/usr/bin/curl`

### 问题

默认 bin_path 用裸名 `curl` 还是完整路径 `/usr/bin/curl`？

### 决策

`/usr/bin/curl`。

### 原因

`access("curl", X_OK)` 不使用 PATH 查找，总是返回 -1。必须用完整路径才能通过检测。

这和 shell 中 `which curl` 的行为不同——shell 通过 PATH 查找可执行文件，但 `access()` 系统调用不查 PATH，只看给定路径是否存在且有执行权限。

---

## 决策 3：bus 回调中不能调 cleanup

### 问题

`on_read` EOF 后调 `dispatch_end` + `cleanup`，bus callback 中再调 `cleanup_by_req_id` 导致 double cleanup。

### 决策

bus callback 只读数据，不调 cleanup。资源释放由持有方负责。

### 原因

1. **bus callback 是观察者**：收到事件时数据已准备好，callback 只负责读取和传递，不应该触发资源释放。

2. **资源释放由持有方负责**：HTTP 请求的 C 侧资源由 `kwcc_http` 模块自己管理。`ack_cleanup` 在 `notify_js` 内部自动调用，不需要外部干预。

3. **double cleanup 风险**：callback 和 ack_cleanup 都调 cleanup，时序不确定，可能释放已释放的内存。

### 通用原则

**回调中不能释放资源**。callback 是"发生了什么"的通知，不是"请释放"的指令。资源生命周期由创建者管理。

---

## 决策 4：curl argv 长选项和参数必须分开

### 问题

`--max-time 30` 作为单个字符串传递给 curl，curl 无法解析。

### 决策

长选项和参数必须分开为两个 argv 元素：`argv[ai++] = "--max-time"` + `argv[ai++] = "30"`。

### 原因

`execvp` 的 argv 是字符串数组，每个元素是独立参数。shell 中 `--max-time 30` 会被 shell 拆成两个参数，但 `execvp` 不经过 shell，直接传给程序。curl 收到 `"--max-time 30"` 作为一个整体时，无法识别这个选项。

---

## 决策 5：C→JS 异步数据传输的一般契约

### 问题

HTTP 模块的 C→JS 数据传输模式，是否适用于未来其他异步模块？

### 决策

提炼为通用契约，适用于所有 C→JS 异步通知场景（HTTP、WebSocket、Timer 等）。

### 契约内容

1. **数据所有权规则**：数据通过 `JS_NewStringLen` 拷贝进 GC 堆后，C 侧 buffer 就没用了
2. **cleanup 时机**：`ack_cleanup` 在 `call_cb` 之前自动调用，调用方不需要记着释放
3. **回调只读不释放**：bus callback 只读数据，资源释放由持有方负责

### 适用场景

| 未来模块 | C→JS 通知 | 数据所有权 | cleanup 方式 |
|----------|----------|-----------|-------------|
| WebSocket | `ops->notify_js("ws", ...)` | 消息拷贝进 GC 堆 | `ack_cleanup` 释放 C 侧 buffer |
| Timer | `ops->notify_js("timer", ...)` | 无数据传输 | `ack_cleanup = NULL` |
| File I/O | `ops->notify_js("fs", ...)` | 内容拷贝进 GC 堆 | `ack_cleanup` 释放 C 侧 buffer |

---

## 决策 6：验证而非猜测——第三方库行为假设的方法论

### 问题

picohttpparser 事件中，"只能解析 HTTP/1.x"最初是未验证的假设。

### 决策

涉及第三方库行为假设时，必须读源码确认，不能凭经验猜。

### 原因

1. **假设的代价**：错误假设导致 2 天调试（headers 始终为 0），读源码确认只花了 10 分钟
2. **记忆文件不是替代**：记忆文件记录的是已确认的知识，不是猜测的来源。如果记忆文件中的知识没有标注来源（源码验证 / 官方文档 / 实测），应该重新验证
3. **渐进验证**：先验证最小假设（如"这个解析器支持 HTTP/2 吗？"），不要从大结论开始

### 适用范围

- 第三方库的能力边界（支持什么/不支持什么）
- API 的隐式行为（参数为 NULL 时 crash 吗？返回值语义是什么？）
- 平台特定行为（macOS vs Linux 的系统调用差异）
