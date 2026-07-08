# HTTP 模块接入 lifecycle_shutdown 服务 — 实施计划

> 服务规范见 [lifecycle-shutdown-service-spec.md](lifecycle-shutdown-service-spec.md)
> 本文件只记录 HTTP 模块接入的施细节 + lifecycle_shutdown 服务实现

---

## 问题

`kwcc_http_cancel` 的执行路径：

```
kill(req->pid, SIGTERM)
kwcc_bus_publish("http/cancel/req_N")
kwcc_http_cleanup(req)
    → waitpid(req->pid, NULL, WNOHANG)   // 子进程可能还没退出 → 返回 0
    → req->pid = 0                       // pid 清零
    → memset(req, 0, ...)                // in_use = 0
```

子进程还没退出 pid 就被清零 → 僵尸进程。`check_progress` 跳过 `in_use=0` 的 slot，永久无法回收。

---

## 解决方案：接入 lifecycle_shutdown 服务

HTTP 模块注册进 lifecycle_shutdown 服务，由服务统一管理废弃资源的检测和回收。同时 HTTP 保留 on-demand 回收作为紧急路径。

---

## Part 1：lifecycle_shutdown 服务实现

### 新增文件

`src/kwcc_lifecycle_shutdown.h` + `src/kwcc_lifecycle_shutdown.c`

### 数据结构

```c
#define KWCC_LIFECYCLE_SHUTDOWN_MAX_MODULES 128

typedef struct kwcc_lifecycle_shutdown_entry {
    const char *name;
    int  (*dirty_count)(void);
    int  threshold;
    void (*shutdown)(int force);       // 回收函数（force=1 全量释放，force=0 轻量释放）
} kwcc_lifecycle_shutdown_entry_t;

static kwcc_lifecycle_shutdown_entry_t g_kwcc_lifecycle_shutdown_entries[KWCC_LIFECYCLE_SHUTDOWN_MAX_MODULES];
static int g_kwcc_lifecycle_shutdown_count = 0;
static int g_kwcc_lifecycle_shutdown_shutdown = 0;  // force_all 开始时设 1
static uint64_t g_kwcc_lifecycle_shutdown_bus_sub_id = 0;
```

模块通过 `kwcc_lifecycle_shutdown_is_exiting()` 判断当前是否在 force_all 上下文中，运行期误调 shutdown(force=1) 时 warn + 跳过。

### API 实现

**`kwcc_lifecycle_shutdown_init()`**：
- memset entries
- subscribe("frame/tick", lifecycle_shutdown_on_tick, NULL)

注意："frame/tick" 是 C→C 的 bus 通信，lifecycle_shutdown 直接 subscribe，不经过 JS 白名单（白名单只控制 C→JS 转发），无需配置。

**`kwcc_lifecycle_shutdown_register(entry)`**：
- 检查 g_shutdown 标志
- 检查 count < MAX
- 复制 entry 到 entries[count]
- count++

**`kwcc_lifecycle_shutdown_on_tick(topic, data, len, user_data)`**（bus callback）：
- 检查 g_shutdown 标志
- 遍历 entries
- 调 dirty_count()
- >= threshold → 调 shutdown(force=0)

**`kwcc_lifecycle_shutdown_force_all()`**：
- 设 g_shutdown = 1
- 逆序遍历 entries，调 entry->shutdown(force=1)（不走 bus，防止 bus 不可用时 force_all 失败）
- unsubscribe bus sub_id
- log_info 记录完成

### 涉及文件

| 文件 | 改动 |
|------|------|
| `src/kwcc_lifecycle_shutdown.h` | 新增：服务声明 + entry 结构体 |
| `src/kwcc_lifecycle_shutdown.c` | 新增：服务实现（依赖 `kwcc_bus.h`） |
| `src/kwcc.h` | include 新头文件 |
| `src/main.m` | init() 调 `kwcc_lifecycle_shutdown_init()`；frame() 发 `"frame/tick"`；cleanup() 调 `force_all()` |
| `Makefile` | 新增编译规则 |

---

## Part 2：HTTP 模块接入

### 状态模型：二态 + 隐式信号

不需要 `cancelled` 字段。用 `pipe_read_fd == -1` 作为"已取消"的隐式信号：

| in_use | pipe_read_fd | 含义 | check_progress | shutdown(force=0) | shutdown(force=1) |
|--------|-------------|------|---------------|-------------------|-------------------|
| 0 | — | 空闲 | 跳过 | 跳过 | 跳过 |
| 1 | >= 0 | 正常请求 | 汇报进度 | 跳过 | 终止 + 回收 |
| 1 | -1 | 已取消，子进程可能还在跑 | 跳过 | waitpid(WNOHANG) 尝试回收 | 终止 + 回收 |

### dirty_count 实现

```c
static int kwcc_http_dirty_count(void) {
    int count = 0;
    for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
        if (g_kwcc_http_reqs[i].in_use && g_kwcc_http_reqs[i].pipe_read_fd < 0)
            count++;
    }
    return count;
}
```

threshold = 1：有 1 个 cancelled slot 就尝试回收。

### kwcc_http.h 新增声明

```c
void kwcc_http_shutdown(int force);
```

### shutdown(int force) 实现

设计决策：不需要 grace period 和 SIGKILL 兜底。理由：
- curl 收到 SIGTERM 后会立刻终止（SIGTERM 是可靠的终止信号，curl 没有忽略它的理由）
- 已取消的 slot（pipe 已关闭），curl 因 SIGPIPE 更快退出
- curl 没有需要优雅关闭的资源（不写文件、不维护连接池），强杀没有额外损失
- curl 有 `--max-time` 超时保底，极端情况也能自行退出
- 所以 SIGTERM + waitpid(阻塞) 足够，不需要 sleep 循环等待或 SIGKILL 升级

```c
static int g_kwcc_http_shutdown = 0;

void kwcc_http_shutdown(int force) {
    if (force) {
        /* ── 全量释放：只由 force_all 调用，运行期禁止直接调 ── */
        g_kwcc_http_shutdown = 1;

        /* Phase 1: 关闭所有 pipe + 发 SIGTERM */
        for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
            kwcc_http_req_t *req = &g_kwcc_http_reqs[i];
            if (!req->in_use) continue;
            if (req->pipe_read_fd >= 0) {
                kwcc_io_unregister(req->pipe_read_fd);
                close(req->pipe_read_fd);
                req->pipe_read_fd = -1;
            }
            if (req->pid > 0) kill(req->pid, SIGTERM);
        }

        /* Phase 2: waitpid 回收所有子进程 */
        for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
            kwcc_http_req_t *req = &g_kwcc_http_reqs[i];
            if (!req->in_use || req->pid <= 0) continue;
            waitpid(req->pid, NULL, 0);   // 阻塞等待，SIGTERM 后 curl 会立刻退出
            kwcc_http_cleanup(req);
        }

        log_info("http: shutdown complete");
    } else {
        /* ── 轻量释放：循环调 reap_cancelled 直到返回 NULL ── */
        while (kwcc_http_reap_cancelled()) {}
    }
}
```

### on-demand 回收（HTTP 内部优化）

**`kwcc_http_find_free_slot`**：从现有 `kwcc_http_request` 中提取的 static 函数。

```c
static kwcc_http_req_t *kwcc_http_find_free_slot(void) {
    for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
        if (!g_kwcc_http_reqs[i].in_use)
            return &g_kwcc_http_reqs[i];
    }
    return NULL;
}
```

**`kwcc_http_reap_cancelled`：尝试回收一个已退出的 cancelled slot，shutdown(force=0) 和 on-demand 共享。

```c
static kwcc_http_req_t *kwcc_http_reap_cancelled(void) {
    for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
        kwcc_http_req_t *req = &g_kwcc_http_reqs[i];
        if (!req->in_use || req->pipe_read_fd >= 0) continue;
        if (req->pid <= 0) {
            kwcc_http_cleanup(req);
            return req;
        }
        int status;
        pid_t ret = waitpid(req->pid, &status, WNOHANG);
        if (ret > 0) {
            kwcc_http_cleanup(req);
            return req;
        }
    }
    return NULL;
}
```

**`kwcc_http_request` 改造**：

```c
const char *kwcc_http_request(...) {
    if (g_kwcc_http_shutdown) return NULL;

    /* 1. 找空闲 slot */
    kwcc_http_req_t *req = kwcc_http_find_free_slot();

    /* 2. 没空闲 → on-demand 回收 cancelled slot */
    if (!req) {
        req = kwcc_http_reap_cancelled();
    }

    /* 3. 仍然没有 → 真的满了 */
    if (!req) return NULL;

    /* 4. 正常发起请求（原有逻辑不变） */
}
```

### 注册进服务

```c
static kwcc_lifecycle_shutdown_entry_t g_kwcc_http_shutdown_entry = {
    .name        = "http",
    .dirty_count = kwcc_http_dirty_count,
    .threshold   = 1,
    .shutdown    = kwcc_http_shutdown,
};

/* kwcc_http_init() 中注册 */
kwcc_lifecycle_shutdown_register(&g_kwcc_http_shutdown_entry);
```

### `kwcc_http_cancel` 简化

```c
void kwcc_http_cancel(const char *req_id) {
    if (!req_id) return;
    kwcc_http_req_t *req = kwcc_http_req_find(req_id);
    if (!req) return;
    if (req->pipe_read_fd < 0) return;  // 已取消，跳过（防连续 cancel）

    /* 关 pipe：curl SIGPIPE 自然退出 */
    kwcc_io_unregister(req->pipe_read_fd);
    close(req->pipe_read_fd);
    req->pipe_read_fd = -1;

    /* 发布 cancel 事件 */
    char topic[128], safe[128];
    snprintf(topic, sizeof(topic), "http/cancel/%s", req_id);
    kwcc_base_topic_sanitize(safe, sizeof(safe), topic);
    kwcc_bus_publish(safe, NULL, 0);

    /* 不调 cleanup，不发 SIGTERM，不等 waitpid */
    /* in_use 保持 1，pid 保留 */
    /* 回收交给 lifecycle_shutdown poll + on-demand */
}
```

### 其他改动

**`kwcc_http_check_progress` 简化**：

取消 cancelled slot 的僵尸回收（原 check_progress 对所有 `in_use=1` 的 slot 做 waitpid，现改为跳过 cancelled slot），回收路径转移到 lifecycle_shutdown poll + on-demand。

```c
void kwcc_http_check_progress(void) {
    for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
        kwcc_http_req_t *req = &g_kwcc_http_reqs[i];
        if (!req->in_use) continue;
        if (req->pipe_read_fd < 0) continue;  // 已取消，跳过
        /* 只做进度汇报 + 正常路径僵尸回收（原有逻辑不变） */
    }
}
```

**`kwcc_http_on_read` 防御**：

```c
static void kwcc_http_on_read(int fd, void *user_data) {
    kwcc_http_req_t *req = (kwcc_http_req_t *)user_data;
    if (req->pipe_read_fd < 0) return;
    // ... 原有逻辑
}
```

**`kwcc_js_http.c` — cancel 路径注释更新 + `http_unload` 保持 NULL**：

cancel 路径注释更新：旧注释 "kwcc_http_cancel already called cleanup" 不再成立（新方案 cancel 不调 cleanup，C 侧资源延迟释放）。ack_cleanup=NULL 仍然正确——cancel 时 JS 侧无需标记"C 侧可释放"，C 侧资源由 lifecycle_shutdown 管理，等子进程退出后再回收。注释改为："cancel 不释放 C 侧资源，ack_cleanup 不负责触发回收，延迟回收由 lifecycle_shutdown 管理"。

`http_unload` 保持 NULL：当前 HTTP Plugin 没有 JS 侧资源需要清理，`kwcc_js_http_module.unload` 保持 NULL。`shutdown(force=1)` 由 lifecycle_shutdown_force_all() 统一调用，不再通过 unload 触发。

**`kwcc_http_cleanup` 防御 pipe_read_fd = -1**：

现有 cleanup 代码：`kwcc_io_unregister(req->pipe_read_fd)` 在 `pipe_read_fd=-1` 时传 -1 给 io_unregister（bug）。close 有 `>= 0` 检查，但 unregister 没有。需要把 unregister 也放入 `>= 0` 检查内：

```c
static void kwcc_http_cleanup(kwcc_http_req_t *req) {
    if (req->pipe_read_fd >= 0) {
        kwcc_io_unregister(req->pipe_read_fd);
        close(req->pipe_read_fd);
        req->pipe_read_fd = -1;
    }
    // ... 原有 waitpid + free + memset 逻辑不变
}
```

shutdown(force=1) Phase 2 对 cancelled slot（pipe_read_fd=-1）调 cleanup 时，不会对 -1 做 unregister/close。

**cancelled slot 回收时不发布 bus 事件**：

cancel 时 JS 侧已收到 cancel reject。之后 lifecycle_shutdown poll / on-demand 通过 `reap_cancelled` → `cleanup` 回收 slot 时，`cleanup` 不发布 bus 事件，JS 侧不会收到二次通知。这是正确的——cancel reject 是最终状态，slot 回收是 C 侧内部操作。

---

## Part 3：main.m 改动

### init()

```c
kwcc_lifecycle_shutdown_init();
```

### frame()

```c
/* 顺序约束：io_poll_once → check_progress → frame/tick → process_js
 * io_poll_once 必须在 frame/tick 之前：IO 事件（HTTP 响应到达）需要先被处理，
 * lifecycle_shutdown poll 才能看到正确的 dirty_count */
kwcc_io_poll_once();
kwcc_http_check_progress();
kwcc_bus_publish("frame/tick", NULL, 0);
kwcc_process_js(g_js_ctx, "onFrame();");
```

### cleanup()

```c
kwcc_lifecycle_shutdown_force_all();  // 逆序调各模块 shutdown(force=1)，不走 bus
kwcc_destroy_js(g_js_ctx);           // 逆序调 unload（只清 JS 侧） + FreeContext
kwcc_mempool_shutdown();
```

### kill 信号处理

Sokol 的 cleanup_cb 只在窗口关闭时触发，kill 信号（SIGINT/SIGTERM）不会走 cleanup()。需要注册信号处理函数，在收到 kill 信号时调 `kwcc_lifecycle_shutdown_force_all()` 防止僵尸进程。

```c
/* main.m init() 中注册 */
signal(SIGINT, kwcc_on_signal);
signal(SIGTERM, kwcc_on_signal);

static void kwcc_on_signal(int sig) {
    log_info("received signal %d, forcing shutdown", sig);
    kwcc_lifecycle_shutdown_force_all();
    _exit(sig);
}
```

---

## 改动汇总

| 文件 | 改动 | 来源 |
|------|------|------|
| `src/kwcc_lifecycle_shutdown.h` | 新增：服务声明 + entry 结构体 | Part 1 |
| `src/kwcc_lifecycle_shutdown.c` | 新增：服务实现 | Part 1 |
| `src/kwcc.h` | include 新头文件 | Part 1 |
| `src/main.m` | init/frame/cleanup 三处改动 + kill 信号处理 | Part 1 + Part 3 |
| `Makefile` | 新增编译规则 | Part 1 |
| `src/kwcc_http.h` | 新增 `kwcc_http_shutdown(int force)` 声明 | Part 2 |
| `src/kwcc_http.c` | 新增 `g_kwcc_http_shutdown` + `kwcc_http_reap_cancelled` + `kwcc_http_find_free_slot` + dirty_count + shutdown + on-demand + cancel 简化 + check_progress 简化 + on_read 防御 | Part 2 |
| `src/kwcc_js_http.c` | cancel 路径注释更新（ack_cleanup 语义变化）+ `http_unload` 保持 NULL | Part 2 |

不改动：
- `kwcc_http_req_t`（不需要 `cancelled` 字段）
- `kwcc_http_cleanup`（逻辑有调整：`kwcc_io_unregister` 移入 `>= 0` 检查内，避免对 -1 做 unregister）
- JS 层（`http.js`、`promise.js` 不变）

---

## 测试

| # | 测试 | 验证 |
|---|------|------|
| 1 | lifecycle_shutdown_init 后 subscribe "frame/tick" | bus 注册成功 |
| 2 | HTTP 注册进服务后 dirty_count 返回正确值 | 0 / 1 / N |
| 3 | cancel 后 slot 状态 | `in_use=1, pipe_read_fd=-1` |
| 4 | bus "frame/tick" 触发后，dirty_count >= threshold → shutdown(force=0) | cancelled slot 回收 |
| 5 | shutdown(force=0) 回收已退出的 cancelled 请求 | `in_use=0` |
| 6 | shutdown(force=1) 终止所有活跃请求 | 所有 slot `in_use=0` |
| 7 | on-demand 回收：slot 满时 request 仍能成功 | cancelled slot 被复用 |
| 8 | cancel 时 pipe 已关闭 | IO reactor 不再触发 on_read |
| 9 | cancel 后 JS 收到 reject | Promise 不悬挂 |
| 10 | 连续 cancel 安全 | 重复操作无害 |
| 11 | force_all 逆序调 shutdown(force=1) | HTTP 请求被终止 + 回收 |
| 12 | 模块 unload 不调 shutdown(force=1) | unload 只清 JS 侧 |
| 13 | shutdown 后拒绝新请求 | `kwcc_http_request` 返回 NULL |
| 14 | 应用退出时无僵尸进程 | `ps` 验证无 zombie curl |
| 15 | cleanup 对 pipe_read_fd=-1 不调 io_unregister | 修复 bug 的回归保护 |

---

## 边界情况

| 场景 | 行为 |
|------|------|
| cancel 后子进程立刻退出 | lifecycle_shutdown poll 下一帧回收 |
| cancel 后子进程延迟退出 | poll 每帧尝试；on-demand 在 request 时紧急回收 |
| cancel 后子进程不响应 SIGPIPE | `--max-time` 超时退出，或 shutdown(force=1) 时 SIGTERM → SIGKILL |
| 连续 cancel 同一请求 | `pipe_read_fd < 0` 防御，跳过重复 cancel |
| 所有 8 slot 都 cancelled | poll 逐帧回收；request 时 on-demand 紧急回收；极端等 force_all |
| 应用退出时有活跃请求 | force_all → shutdown(force=1) → 全量回收 |
| on_read cancel 后仍触发 | `pipe_read_fd < 0` 防御 |
| 正常请求完成（on_read EOF） | 原有逻辑不变 |
| force_all 后再调 poll | g_shutdown=1 防御，安全跳过 |
| force_all 后再调 register | g_shutdown=1 防御，安全跳过 |
