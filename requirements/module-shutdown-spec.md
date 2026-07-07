# kwcc 模块 Shutdown 链规范

> 前置依赖：`js-bridge-architecture.md`（`kwcc_js_module_t` 已定义 `unload` 钩子）
> 被依赖：所有现有和未来的 Plugin 模块

---

## 背景

kwcc 现有的 `kwcc_js_module_t` 已经预留了 `unload` 钩子，但没有规范约定：

- 什么时候调
- 调用顺序是什么
- 模块内部应该做什么
- 服务层（如 `kwcc_http`）和 Plugin 层（如 `kwcc_js_http`）各自负责什么

没有规范的结果是：每个模块各自猜，行为不一致，退出时资源释放顺序不可预期。

---

## 核心概念：两层 Shutdown

kwcc 的模块分两层，shutdown 也分两层，职责不同：

```
kwcc_destroy_js()              ← Plugin 层 shutdown（JS 引擎侧）
    └── mod->unload(ops)       ← 每个 Plugin 的 unload 钩子
            └── kwcc_http_shutdown(true)   ← 服务层全量释放（C 服务侧）

kwcc_http_shutdown(false)      ← 服务层轻量释放（帧循环调用，独立于 unload）
```

| 层 | 触发方 | 职责 |
|----|--------|------|
| Plugin 层 `unload` | core 统一调用 | 清理 JS 侧资源（JSValue、$notify 注册等），调服务层 `shutdown(true)` |
| 服务层 `shutdown(force)` | Plugin 的 `unload` 调用，或帧循环直接调用 | 回收 C 侧资源（子进程、fd、buffer） |

---

## 规范一：`kwcc_js_module_t.unload` 的约定

### 声明

```c
typedef struct kwcc_js_module {
    const char *name;
    void (*load)(kwcc_js_ops_t *ops);
    const kwcc_js_api_t *apis;
    void (*on_bus_event)(const char *topic, const void *data,
                         size_t len, kwcc_js_ops_t *ops);
    void (*unload)(kwcc_js_ops_t *ops);  // 可为 NULL
} kwcc_js_module_t;
```

### 约定

**何时必须实现 `unload`：**
- 模块持有任何 C 侧资源（子进程、fd、线程、buffer）
- 模块对应的服务层有 `shutdown` 函数

**何时可以为 NULL：**
- 模块只注入 JS 对象、没有 C 侧资源（如纯配置模块）

**`unload` 内部必须做的事：**

```c
static void http_unload(kwcc_js_ops_t *ops) {
    // 1. 调服务层 shutdown(true)，全量回收 C 侧资源
    kwcc_http_shutdown(true);

    // 2. 如果持有 JS 侧资源（JSValue 引用），在 JS 引擎销毁前释放
    //    注意：JS 引擎在 unload 全部完成后才销毁，ops 在此时仍然有效
}
```

**`unload` 不应该做的事：**
- 不发 bus 事件（bus 可能已经在 shutdown 路径上）
- 不调 `ops->eval`（JS 引擎即将销毁）
- 不假设其他模块还活着

---

## 规范二：服务层 `shutdown` 函数约定

每个有 C 侧资源的服务层模块必须提供一个 `shutdown` 函数。

### 命名规范

```c
void kwcc_{module}_shutdown(bool force);
```

### 两种模式

| | `force=false`（轻量释放） | `force=true`（全量释放） |
|--|--|--|
| 触发方 | 帧循环（每帧调用） | Plugin `unload` 或应用退出 |
| 阻塞 | **禁止**（帧循环中调用） | **允许**（应用退出时调用） |
| 发信号 | **禁止**（不发 SIGTERM/SIGKILL） | **允许**（SIGTERM → grace period → SIGKILL） |
| 做什么 | 只回收已自然结束的资源 | 终止所有活跃操作 + 回收所有资源 |
| 调用频率 | 每帧一次 | 仅应用退出时一次 |
| 日志 | 不打日志（高频） | `log_info` 记录回收结果 |
| 幂等 | 必须 | 必须 |

### `force=true` 实现模板

```c
void kwcc_xxx_shutdown(bool force) {
    if (!force) {
        /* 轻量释放：见下方模板 */
        return;
    }

    /* ── 全量释放 ── */

    /* 1. 停止接受新请求 */
    g_kwcc_xxx_shutdown = 1;

    /* 2. 对所有活跃资源，发终止信号 */
    for (each active resource) {
        send_termination_signal(resource);
    }

    /* 3. grace period 等待（同步阻塞，有超时） */
    for (int attempt = 0; attempt < MAX_WAIT; attempt++) {
        if (all_reaped) break;
        usleep(WAIT_INTERVAL);
    }

    /* 4. 强制终止超时未退出的资源 */
    for (each still-active resource) {
        force_kill(resource);
    }

    /* 5. 释放所有 C 侧资源 */
    for (each resource) {
        kwcc_xxx_cleanup(resource);
    }

    log_info("xxx: shutdown complete");
}
```

### `force=false` 实现模板

```c
void kwcc_xxx_shutdown(bool force) {
    if (force) {
        /* 全量释放：见上方模板 */
        return;
    }

    /* ── 轻量释放 ── */

    /* 只回收已自然结束的资源，不发信号，不等，不阻塞 */
    for (each resource) {
        if (!resource_is_active(resource)) continue;       // 正常活跃，不管
        if (!resource_has_naturally_ended(resource)) continue;  // 还没结束，不管
        kwcc_xxx_cleanup(resource);                        // 已自然结束，回收
    }
}
```

### 关键设计决策

**`force=true` 时的阻塞等待**：

直接用 `waitpid(pid, NULL, 0)` 阻塞等待，如果子进程不响应 SIGTERM 会永久阻塞。必须用 grace period + SIGKILL 兜底：

```
SIGTERM → 等待最多 N ms → SIGKILL 强杀 → 阻塞 waitpid 回收
```

这和 systemd / Docker / Kubernetes / nginx 的做法一致。

**`g_shutdown` 防御标志**：

shutdown 后，对应服务的任何 API 调用都应该安全返回（检查 `g_shutdown` 标志）：

```c
const char *kwcc_http_request(...) {
    if (g_kwcc_http_shutdown) {
        log_warn("http: rejecting request, module is shutting down");
        return NULL;
    }
    // ... 原有逻辑
}
```

---

## 规范三：core 调用顺序

`kwcc_destroy_js` 里统一按注册**逆序**调用所有模块的 `unload`：

```c
void kwcc_destroy_js(JSContext *ctx) {
    /* 1. 逆序调用所有模块 unload（后注册的先 unload） */
    for (int i = g_kwcc_js_module_count - 1; i >= 0; i--) {
        if (g_kwcc_js_modules[i]->unload) {
            log_info("js: unloading module '%s'", g_kwcc_js_modules[i]->name);
            g_kwcc_js_modules[i]->unload(&g_kwcc_js_ops);
        }
    }

    /* 2. 所有模块 unload 完成后，销毁 JS 引擎 */
    if (ctx) {
        JS_FreeContext(ctx);
    }
}
```

**逆序的原因**：后注册的模块可能依赖先注册的模块，逆序 unload 保证依赖关系不出问题——和构造/析构的对称原则一致。

**unload 完成后才销毁 JS 引擎**：因为 `unload` 内部可能需要 `ops`（如释放 JS 侧持有的 JSValue）。

---

## 规范四：`main.m` 完整 shutdown 链

```c
static void cleanup(void) {
    /* Plugin 层 + 服务层（通过 unload 钩子串联） */
    kwcc_destroy_js(g_js_ctx);

    /* I/O Reactor（在服务层之后，确保 fd 都已关闭） */
    kwcc_io_shutdown(true);

    /* Bus（在 JS 引擎之后，确保没有新事件发布） */
    kwcc_bus_shutdown();

    /* 内存池（最后释放，其他模块可能依赖它） */
    kwcc_mempool_shutdown();
}
```

**顺序原则**：
- 依赖别人的先 shutdown
- 被依赖的后 shutdown
- 内存池永远最后

---

## 规范五：Cancel 语义

Cancel 是运行时操作，不是 shutdown。但 cancel 产生的"已放弃但子进程还在跑"的资源，由 `shutdown` 负责回收。

### Cancel = 逻辑放弃

| 模型 | cancel 含义 | 对外部资源的操作 |
|------|-----------|--------------|
| 直接终止 | "我命令你停下" | `kill(SIGTERM)` 主动杀 |
| 逻辑放弃 | "我不再关心你的结果" | 关掉 pipe，外部程序自然退出 |

**采用模型 2**：cancel = 逻辑放弃。

理由：
- cancel 的本意是"不要结果"，不是"杀进程"。外部程序退出是 cancel 的**后果**，不是 cancel 的**定义**
- 关 pipe 后 curl 写 pipe 失败 → SIGPIPE → 自然退出，比 SIGTERM 强杀更干净
- 超时机制已有保底：即使 curl 卡在连接阶段，超时后也会退出

### Cancel 后的资源回收

cancel 时**不调 cleanup，不等回收**。`in_use` 保持 1，pid 保留。

回收时机：
- **运行时**：`shutdown(false)` 每帧尝试回收已自然退出的子进程（非阻塞）
- **退出时**：`shutdown(true)` 终止所有活跃请求 + 回收（阻塞）

### 三级关闭协议

当需要终止子进程时，遵循通用模式（systemd / Docker / Kubernetes / nginx 都用）：

```
Level 1: 关 pipe     → curl SIGPIPE 自然退出（通常 < 1ms）
Level 2: SIGTERM    → 礼貌请求退出（grace period 内等待）
Level 3: SIGKILL    → 强制终止（grace period 超时后）
```

cancel 时只做 Level 1。`shutdown(true)` 时按需升级到 Level 2/3。

---

## 实施计划

### Step 1：提交规范文档

新增 `requirements/module-shutdown-spec.md`，内容即本文档，作为所有模块实施的参考基准。

**验证**：文档 review 通过

---

### Step 2：HTTP 模块作为实例实施

**2a. `kwcc_http.h` 新增声明**

```c
void kwcc_http_shutdown(bool force);
```

**2b. `kwcc_http.c` 加 shutdown 防御标志**

```c
static int g_kwcc_http_shutdown = 0;
```

`kwcc_http_request` 入口加防御：

```c
const char *kwcc_http_request(...) {
    if (g_kwcc_http_shutdown) return NULL;
    // ... 原有逻辑
}
```

**2c. `kwcc_http.c` 实现 `kwcc_http_shutdown`**

- `force=true`：设 `g_kwcc_http_shutdown=1` → 关所有 pipe + SIGTERM → grace period 等待 → SIGKILL 兜底 → cleanup
- `force=false`：只回收 `pipe_read_fd < 0`（已 cancel）且 `waitpid(WNOHANG) > 0`（子进程已退出）的 slot

**2d. `kwcc_http_cancel` 简化**

```c
void kwcc_http_cancel(const char *req_id) {
    if (!req_id) return;
    kwcc_http_req_t *req = kwcc_http_req_find(req_id);
    if (!req) return;

    /* 关 pipe：curl SIGPIPE 自然退出 */
    kwcc_io_unregister(req->pipe_read_fd);
    close(req->pipe_read_fd);
    req->pipe_read_fd = -1;              // 隐式信号：已取消

    /* 发布 cancel 事件：JS 端 reject */
    // ... 原有 bus_publish 逻辑

    /* 不调 cleanup，不发 SIGTERM，不等 waitpid */
    /* in_use 保持 1，pid 保留 */
    /* 子进程回收交给 shutdown(false) 运行时轮询
     * 或 shutdown(true) 应用退出时统一回收 */
}
```

**2e. `kwcc_http_check_progress` 简化**

```c
void kwcc_http_check_progress(void) {
    for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
        kwcc_http_req_t *req = &g_kwcc_http_reqs[i];
        if (!req->in_use) continue;
        if (req->pipe_read_fd < 0) continue;  // 已取消，跳过

        /* 只做进度汇报 + 正常路径的僵尸回收（原有逻辑不变） */
    }
}
```

**2f. `kwcc_http_on_read` 防御**

```c
static void kwcc_http_on_read(int fd, void *user_data) {
    kwcc_http_req_t *req = (kwcc_http_req_t *)user_data;
    if (req->pipe_read_fd < 0) return;   // 已取消，忽略数据
    // ... 原有逻辑
}
```

**2g. `kwcc_js_http.c` 实现 `http_unload`**

```c
static void http_unload(kwcc_js_ops_t *ops) {
    kwcc_http_shutdown(true);
}

kwcc_js_module_t kwcc_js_http_module = {
    .name        = "http",
    .load        = http_load,
    .apis        = http_apis,
    .on_bus_event = http_on_bus_event,
    .unload      = http_unload,           // ← 接入 shutdown 链
};
```

**2h. `kwcc_js.c` 改动 `kwcc_destroy_js`**

逆序遍历模块调 `unload`，再 `JS_FreeContext`。

**2i. `main.m` 改动**

- `frame()` 中调 `kwcc_http_shutdown(false)`（轻量释放）
- `cleanup()` 中调 `kwcc_destroy_js`（内部触发 `http_unload` → `kwcc_http_shutdown(true)`）

**验证**：
- `make run` 正常，退出时无僵尸进程
- cancel 后 slot 可被 `shutdown(false)` 回收
- 连续 cancel 8 个请求后 slot 不耗尽

---

### Step 3：其他模块实施计划

| 模块 | 服务层 shutdown | Plugin unload | 优先级 |
|------|----------------|---------------|--------|
| `kwcc_io` | `kwcc_io_shutdown(true)` 关闭所有 FD | 无 Plugin | 随 HTTP 一起做 |
| `kwcc_bus` | `kwcc_bus_shutdown()` 清订阅表 | 无 Plugin | 随 HTTP 一起做 |
| `kwcc_timer`（未来） | `kwcc_timer_shutdown(bool force)` 清定时器表 | `timer_unload` | Timer Plugin 时做 |
| `kwcc_ws`（未来） | `kwcc_ws_shutdown(bool force)` 关所有连接 | `ws_unload` | WS Plugin 时做 |

---

## 新模块接入 shutdown 链的 Checklist

以后每新增一个 Plugin，对照这个清单：

```
□ 服务层实现 kwcc_{module}_shutdown(bool force)
□ force=false 只回收已自然结束的资源，不阻塞，不发信号
□ force=true 终止所有活跃操作，可阻塞，SIGTERM → grace → SIGKILL
□ 两种模式都幂等，重复调用无害
□ force=true 有 log_info 记录回收结果
□ 服务层 API 加 g_shutdown 防御，拒绝 shutdown 后的新请求
□ Plugin 层实现 unload 钩子，内部调 kwcc_{module}_shutdown(true)
□ kwcc_js_module_t 描述符的 unload 字段非 NULL
□ unload 不发 bus 事件，不调 eval，不假设其他模块活着
□ 帧循环中调 kwcc_{module}_shutdown(false)（如果需要轻量释放）
```
