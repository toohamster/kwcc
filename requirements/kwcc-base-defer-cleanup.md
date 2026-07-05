# kwcc_base_defer_cleanup — 函数作用域资源自动清理工具

> 创建于 2026-07-05

## 1. 为什么需要

### 问题

C 语言没有 try/catch、RAII、defer，函数中有多个资源需要清理时，每个错误路径都要重复写释放逻辑。

以 `kwcc_http_request()` 为例，它持有 `bin_val`、`timeout_val`、`argv` 等资源，5 个错误点各写一遍 free，冗余且容易遗漏。

### 为什么重要

**遗漏释放不是"不优雅"，是运行时灾难**：

- 内存泄漏在长期运行进程中逐帧累积，不会自行消失。kwcc 是 60fps UI 引擎，一帧泄漏 1KB，一分钟就泄漏 3.6MB
- 文件描述符泄漏（pipe fd）会导致系统资源耗尽，最终无法创建新连接
- 子进程泄漏（未 waitpid）产生僵尸进程，占用 PID 表

**重复写的结构性风险**：

- 每新增一个资源，必须在**所有已有错误路径**都加释放——漏一个就是泄漏
- 错误路径越多 × 资源越多 = 遗漏概率指数增长
- 代码审查也很难发现：释放逻辑分散在各错误分支，没有集中点可对照
- `kwcc_http_request` 已有 5 个错误路径 × 3 个资源 = 15 处释放，后续模块只会更多

**defer_cleanup 解决的根本问题**：资源注册一次，释放保证执行——从"每个错误路径都记着释放"变为"注册一次，自动清理"，消除结构性遗漏风险。

### 现有方案对比

| 方案 | 优点 | 缺点 |
|------|------|------|
| 每个错误点重复 free | 简单 | 冗余、易遗漏 |
| goto cleanup | Linux 内核标准做法 | 清理逻辑集中在函数末尾，和正常流程分离；资源多时标签处仍需大量 free |
| C23 defer | 语言原生 | 编译器不支持 |
| `__attribute__((cleanup))` | 编译器扩展可用 | 语法侵入性强，不美观 |
| **defer_cleanup 链表（本方案）** | 显式注册、统一清理、无全局状态 | 需要自己实现 |

### 设计目标

- 函数作用域局部：每个函数自己申请实例，嵌套调用互不干扰
- 无全局变量/常量：不拍脑袋定上限
- 小巧：单向链表，头插法 O(1)
- 归属 kwcc_base：纯 C 基础设施，和 kwcc_base_str_t 同级

## 2. 如何实现

### 数据结构

```c
typedef void (*kwcc_base_defer_cleanup_fn)(void *);

typedef struct kwcc_base_defer_cleanup_node {
    void                                  *ptr;
    kwcc_base_defer_cleanup_fn                   fn;
    struct kwcc_base_defer_cleanup_node   *next;
} kwcc_base_defer_cleanup_node_t;

typedef struct {
    kwcc_base_defer_cleanup_node_t *head;
} kwcc_base_defer_cleanup_t;
```

### API

| 函数 | 功能 |
|------|------|
| `kwcc_base_defer_cleanup_t *kwcc_base_defer_cleanup_create()` | malloc 实例，head=NULL |
| `void kwcc_base_defer_cleanup_push(kwcc_base_defer_cleanup_t *dc, void *ptr, kwcc_base_defer_cleanup_fn fn)` | 头插法加节点，O(1) |
| `void kwcc_base_defer_cleanup_run(kwcc_base_defer_cleanup_t *dc)` | 遍历链表逐个执行 fn(ptr) + 释放节点 + 释放 dc 本身 |

### 关键设计点

1. **fn 类型 `void (*)(void *)`**：和 `free` 签名一致，可直接传。其他签名（如 `kwcc_base_str_free`）需强转 `(kwcc_base_defer_cleanup_fn)`，C 语言常见做法
2. **头插法**：push O(1)，run 时逆序执行（后注册先执行），与 C23 defer / Go defer 语义一致
3. **run 会释放 dc 本身**：调用者不需要单独 free dc

### 使用示例

```c
const char *kwcc_http_request(...) {
    kwcc_base_defer_cleanup_t *dc = kwcc_base_defer_cleanup_create();

    kwcc_base_str_t bin_val = kwcc_config_tlv_get_field(&http_cfg, "bin_path");
    kwcc_base_defer_cleanup_push(dc, &bin_val, (kwcc_base_defer_cleanup_fn)kwcc_base_str_free);

    kwcc_base_str_t timeout_val = kwcc_config_tlv_get_field(&http_cfg, "timeout");
    kwcc_base_defer_cleanup_push(dc, &timeout_val, (kwcc_base_defer_cleanup_fn)kwcc_base_str_free);

    char **argv = malloc(...);
    kwcc_base_defer_cleanup_push(dc, argv, free);

    if (!argv) {
        kwcc_base_defer_cleanup_run(dc);  // 释放 timeout_val + bin_val
        return NULL;
    }

    if (pipe(pipefd) == -1) {
        kwcc_base_defer_cleanup_run(dc);  // 释放 argv + timeout_val + bin_val
        return NULL;
    }

    /* 函数末尾统一 run：释放所有已注册资源 + dc 本身 */
    kwcc_base_defer_cleanup_run(dc);
    return req->req_id;
}
```

## 3. 执行计划

| Step | 内容 | 新增/修改文件 | 状态 |
|------|------|--------------|------|
| 1 | 数据结构 + API 声明 | `src/kwcc_base.h` | ✅ 完成 |
| 2 | 实现 | `src/kwcc_base.c` | ✅ 完成 |
| 3 | 纯 C 测试 | `tests/test_defer_cleanup.c` | ✅ 14/14 通过 |
| 4 | 编译验证 | `make clean && make` | ✅ 完成 |
| 5 | 用 defer_cleanup 重构 kwcc_http_request | `src/kwcc_http.c` | ✅ 完成 |
| 6 | 编译 + 测试验证 | `make` + 三个测试 | ✅ 14/14 + 6/6 通过 |
