# HTTP 模块接入 lifecycle_shutdown 服务 — 实施计划 v2

> 服务规范见 [lifecycle-shutdown-service-spec.md](lifecycle-shutdown-service-spec.md)（v2 窄边界）
> 演进：v1 → v2 见 [lifecycle-shutdown-v2-discussion.md](lifecycle-shutdown-v2-discussion.md)
> v2 变化：删除服务侧 runtime poll / dirty_count / bus 订阅；HTTP 回收改为 check_progress 自治；
> shutdown 契约简化为 `shutdown(void)` 单模式

## 执行状态

| Part | 内容 | 状态 |
|------|------|------|
| Part 1 | lifecycle_shutdown 服务实现 | ⏳ 待实施 |
| Part 2 | HTTP 模块接入 | ⏳ 待实施 |
| Part 3 | main.m 衔接（init/cleanup/signal） | ⏳ 待实施 |
| 测试 | 服务 S1-S6 + HTTP H1-H11 + 集成 E1-E3 | ⏳ 待实施 |

---

## 问题

`kwcc_http_cancel` 的现行执行路径（kwcc_http.c:262-274）：

```
kill(req->pid, SIGTERM)
kwcc_bus_publish("http/cancel/req_N")
kwcc_http_cleanup(req)
    → kwcc_io_unregister(req->pipe_read_fd)   // ⚠️ bug：pipe_read_fd 可能是 -1
    → waitpid(req->pid, NULL, WNOHANG)        // 子进程可能还没退出 → 返回 0
    → req->pid = 0                            // pid 清零
    → memset(req, 0, ...)                     // in_use = 0
```

两个缺陷：
1. 子进程还没退出 pid 就被清零 → **僵尸进程**。`check_progress` 跳过 `in_use=0` 的 slot，永久无法回收
2. `kwcc_io_unregister(req->pipe_read_fd)` 在 `>= 0` 检查**之外**，pipe_read_fd=-1 时会把 -1 传给 io_unregister

---

## 方案总览（v2）

| 职责 | 归属 | 机制 |
|------|------|------|
| 退出时全量回收 | lifecycle_shutdown 服务 | force_all → kwcc_http_shutdown() |
| 运行时 cancelled slot 回收 | HTTP 自治 | check_progress() 每帧 reap + request() on-demand |
| cancel 语义 | HTTP | 逻辑放弃：只关 pipe（Level 1），不 kill、不 cleanup |

---

## Part 1：lifecycle_shutdown 服务实现

### 新增文件

`src/kwcc_lifecycle_shutdown.h` + `src/kwcc_lifecycle_shutdown.c`

### 数据结构（kwcc_lifecycle_shutdown.c）

```c
#define KWCC_LIFECYCLE_SHUTDOWN_MAX_MODULES 8   /* 论证见 spec v2 注册容量一节 */

typedef struct {
    int exiting;        /* 0=运行中，1=force_all 已开始 */
    int module_count;
} kwcc_lifecycle_shutdown_state_t;

static kwcc_lifecycle_shutdown_state_t g_kwcc_lifecycle_state;
static kwcc_lifecycle_shutdown_entry_t g_kwcc_lifecycle_entries[KWCC_LIFECYCLE_SHUTDOWN_MAX_MODULES];
```

服务零外部模块依赖（不 include kwcc_bus.h——v1 的 bus 订阅已删除），只依赖 llog。

### API 实现

**`kwcc_lifecycle_shutdown_init()`**：
- memset state + entries

**`kwcc_lifecycle_shutdown_register(entry)`**：
- `exiting == 1` → log_warn + return（退出后不再接受注册）
- `module_count >= MAX` → log_warn + return（不静默失败）
- 复制 entry 到 entries[count]，count++，log_debug

**`kwcc_lifecycle_shutdown_force_all()`**：
- `exiting == 1` → log_warn + return（幂等守卫，防 cleanup + kill 信号双重触发）
- `exiting = 1`
- 逆序遍历 entries，直接调 `entry->shutdown()`（不走 bus）
- `log_info("lifecycle_shutdown: forced %d module(s)", module_count)`

**`kwcc_lifecycle_shutdown_is_exiting()`**：
- `return g_kwcc_lifecycle_state.exiting;`

### 涉及文件

| 文件 | 改动 |
|------|------|
| `src/kwcc_lifecycle_shutdown.h` | 新增：entry 结构体 + 4 个 API 声明 |
| `src/kwcc_lifecycle_shutdown.c` | 新增：服务实现 |
| `src/kwcc.h` | include 新头文件 |
| `Makefile` | 新增编译规则 |

---

## Part 2：HTTP 模块接入

### 状态模型：二态 + 隐式信号

不需要 `cancelled` 字段。用 `pipe_read_fd == -1` 作为"已取消"的隐式信号：

| in_use | pipe_read_fd | 含义 | check_progress | shutdown() |
|--------|-------------|------|----------------|-----------|
| 0 | — | 空闲 | 跳过 | 跳过 |
| 1 | >= 0 | 正常请求 | 进度汇报 + 僵尸回收（现有逻辑） | 关 pipe + SIGTERM + 回收 |
| 1 | -1 | 已取消，子进程可能还在跑 | **自治 reap（新增分支）** | SIGTERM + 回收 |

### kwcc_http.h 新增声明

```c
void kwcc_http_shutdown(void);
```

### 注册进服务（kwcc_http.c）

```c
static kwcc_lifecycle_shutdown_entry_t g_kwcc_http_shutdown_entry = {
    .name     = "http",
    .shutdown = kwcc_http_shutdown,
};

/* kwcc_http_init() 中注册（服务已由 main.m 先行 init） */
kwcc_lifecycle_shutdown_register(&g_kwcc_http_shutdown_entry);
```

kwcc_http.c include `kwcc_lifecycle_shutdown.h`（依赖方向：http → lifecycle，lifecycle 不反向依赖任何模块）。

### `g_kwcc_http_shutdown` 防御标志

```c
static int g_kwcc_http_shutdown = 0;

/* kwcc_http_request() 入口 */
if (g_kwcc_http_shutdown) {
    log_warn("http: rejecting request, module is shutting down");
    return NULL;
}
```

### shutdown(void) 实现（全量释放，只由 force_all 调用）

设计决策（讨论 6）：不需要 grace period 和 SIGKILL 兜底。curl 收到 SIGTERM 立刻终止、已取消的 slot 因 SIGPIPE 退出更快、curl 无需优雅关闭的资源、`--max-time` 有超时保底。所以 SIGTERM + waitpid(阻塞) 足够。

```c
void kwcc_http_shutdown(void) {
    /* 运行期防御（讨论 4）：只能在 force_all 上下文中执行 */
    if (!kwcc_lifecycle_shutdown_is_exiting()) {
        log_warn("http: shutdown called outside force_all, skipped");
        return;
    }
    if (g_kwcc_http_shutdown) return;   /* 幂等 */
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

    /* Phase 2: waitpid 阻塞回收所有子进程 */
    for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
        kwcc_http_req_t *req = &g_kwcc_http_reqs[i];
        if (!req->in_use || req->pid <= 0) continue;
        waitpid(req->pid, NULL, 0);   /* 阻塞等待，SIGTERM 后 curl 立刻退出 */
        kwcc_http_cleanup(req);
    }

    log_info("http: shutdown complete");
}
```

cancelled slot（pipe_read_fd 已是 -1）：Phase 1 跳过关 pipe，仍发 SIGTERM；Phase 2 正常 waitpid + cleanup。

### slot 复用：reap_cancelled_slot + on-demand

**核心 helper（一个实现，两处使用）**——尝试回收一个 cancelled slot，回收成功返回 1：

```c
static int kwcc_http_reap_cancelled_slot(kwcc_http_req_t *req) {
    if (req->pid > 0) {
        int status;
        if (waitpid(req->pid, &status, WNOHANG) <= 0) return 0;  /* 子进程还在跑 */
    }
    kwcc_http_cleanup(req);
    return 1;
}
```

**`kwcc_http_request` slot 查找改造（on-demand）**：

```c
/* 1. 找空闲 slot */
kwcc_http_req_t *req = NULL;
for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
    if (!g_kwcc_http_reqs[i].in_use) { req = &g_kwcc_http_reqs[i]; break; }
}

/* 2. 没空闲 → on-demand 紧急回收一个已退出的 cancelled slot */
if (!req) {
    for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
        kwcc_http_req_t *cand = &g_kwcc_http_reqs[i];
        if (cand->in_use && cand->pipe_read_fd < 0 && kwcc_http_reap_cancelled_slot(cand)) {
            req = cand;
            break;
        }
    }
}

/* 3. 仍然没有 → 真的满了 */
if (!req) { log_error(...); return NULL; }
```

### `kwcc_http_cancel` 简化（逻辑放弃）

```c
void kwcc_http_cancel(const char *req_id) {
    if (!req_id) return;
    kwcc_http_req_t *req = kwcc_http_req_find(req_id);
    if (!req) return;
    if (req->pipe_read_fd < 0) return;  /* 已取消，跳过（防连续 cancel） */

    /* Level 1: 关 pipe → curl SIGPIPE 自然退出 */
    kwcc_io_unregister(req->pipe_read_fd);
    close(req->pipe_read_fd);
    req->pipe_read_fd = -1;

    /* 发布 cancel 事件（JS 侧 reject Promise） */
    char topic[128], safe[128];
    snprintf(topic, sizeof(topic), "http/cancel/%s", req_id);
    kwcc_base_topic_sanitize(safe, sizeof(safe), topic);
    kwcc_bus_publish(safe, NULL, 0);

    /* 不调 cleanup，不发 SIGTERM，不等 waitpid */
    /* in_use 保持 1，pid 保留 → 回收交给 check_progress 自治 / on-demand / shutdown */
}
```

### `kwcc_http_check_progress` 自治回收（v2 核心：reap 并入）

```c
void kwcc_http_check_progress(void) {
    for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
        kwcc_http_req_t *req = &g_kwcc_http_reqs[i];
        if (!req->in_use) continue;

        /* cancelled slot：自治 reap（不 dispatch——JS 已收到 cancel reject） */
        if (req->pipe_read_fd < 0) {
            kwcc_http_reap_cancelled_slot(req);
            continue;
        }

        /* 以下正常请求逻辑不变：僵尸回收（含 dispatch）+ 进度汇报 */
        ...
    }
}
```

要点：
- cancelled slot 回收时**不发任何 bus 事件**。cancel reject 是最终状态，slot 回收是 C 侧内部操作，JS 侧不能收到二次通知
- 正常请求路径（waitpid 僵尸回收 + dispatch + 进度汇报）完全不变

### `kwcc_http_cleanup` 修复 unregister(-1) bug

现行代码（kwcc_http.c:514）在 `>= 0` 检查**外**调 `kwcc_io_unregister`：

```c
static void kwcc_http_cleanup(kwcc_http_req_t *req) {
    if (req->pipe_read_fd >= 0) {
        kwcc_io_unregister(req->pipe_read_fd);   /* 移入 >= 0 检查内 */
        close(req->pipe_read_fd);
        req->pipe_read_fd = -1;
    }
    /* 原有 waitpid + free + memset 逻辑不变 */
}
```

shutdown() Phase 2 和 reap_cancelled_slot 对 cancelled slot（pipe_read_fd=-1）调 cleanup 时，不会对 -1 做 unregister。

### `kwcc_http_on_read` 防御

```c
static void kwcc_http_on_read(int fd, void *user_data) {
    kwcc_http_req_t *req = (kwcc_http_req_t *)user_data;
    if (req->pipe_read_fd < 0) return;   /* 已取消，pipe 已关，防御残余事件 */
    /* ... 原有逻辑 ... */
}
```

### `kwcc_js_http.c` 注释更新

cancel 路径注释更新：旧注释 "kwcc_http_cancel already called cleanup" 不再成立（新方案 cancel 不调 cleanup，C 侧资源延迟释放）。ack_cleanup=NULL 仍然正确——cancel 时 JS 侧无需标记"C 侧可释放"，C 侧资源由 HTTP 自治回收（check_progress / on-demand / shutdown）。注释改为："cancel 不释放 C 侧资源，回收由 check_progress / on-demand / shutdown 延迟完成"。

`http_unload` 保持 NULL：HTTP Plugin 没有 JS 侧资源需要清理。`kwcc_http_shutdown()` 由 lifecycle_shutdown_force_all() 统一调用，不通过 unload 触发。

---

## Part 3：main.m 改动

### init()（两处）

```c
kwcc_mempool_init();
kwcc_io_init();
kwcc_lifecycle_shutdown_init();   /* 新增：必须在 http_init 之前 */
kwcc_http_init();

signal(SIGINT, kwcc_on_signal);   /* 新增：kill 信号处理 */
signal(SIGTERM, kwcc_on_signal);
```

信号处理函数：

```c
static void kwcc_on_signal(int sig) {
    log_info("received signal %d, forcing shutdown", sig);
    kwcc_lifecycle_shutdown_force_all();
    _exit(sig);
}
```

### frame() —— 无改动

v2 服务不订阅 bus，不发布 frame/tick。现有顺序 io_poll_once → check_progress → process_js 保持。

### cleanup()（重排）

```c
static void cleanup(void) {
    log_info("=== kwcc exiting ===");
    kwcc_lifecycle_shutdown_force_all();  /* 新增：C 侧资源全量回收（日志仍打开） */
    if (g_kwcc_vg) { nvgDeleteGL3(g_kwcc_vg); g_kwcc_vg = NULL; }
    kwcc_destroy_js(g_js_ctx);
    kwcc_ui_free();
    kwcc_mempool_shutdown();
    sg_shutdown();
    if (g_log_fp) { fflush(g_log_fp); fclose(g_log_fp); g_log_fp = NULL; }  /* 移到末尾 */
}
```

变更点：force_all 前置（先回收 C 侧资源再清 JS 侧）；**日志关闭从第一行移到最后一行**——现行代码先 fclose 导致 force_all 等所有清理日志无法落盘。

---

## 改动汇总

| 文件 | 改动 | 来源 |
|------|------|------|
| `src/kwcc_lifecycle_shutdown.h` | 新增：entry 结构体 + 4 个 API 声明 | Part 1 |
| `src/kwcc_lifecycle_shutdown.c` | 新增：服务实现（零外部模块依赖） | Part 1 |
| `src/kwcc.h` | include 新头文件 | Part 1 |
| `Makefile` | 新增编译规则 | Part 1 |
| `src/main.m` | init 加 lifecycle_init + signal 注册；cleanup 重排（force_all 前置 + 日志关闭移末尾）；frame 不变 | Part 3 |
| `src/kwcc_http.h` | 新增 `void kwcc_http_shutdown(void);` 声明 | Part 2 |
| `src/kwcc_http.c` | `g_kwcc_http_shutdown` + `kwcc_http_reap_cancelled_slot` + request on-demand + cancel 简化 + check_progress cancelled 分支 + shutdown 实现 + cleanup unregister 修复 + on_read 防御 + init 注册 entry | Part 2 |
| `src/kwcc_js_http.c` | cancel 路径注释更新（ack_cleanup 语义变化） | Part 2 |

不改动：
- `kwcc_http_req_t` 结构（不需要 cancelled 字段）
- `kwcc_bus.c/h`（服务不再依赖 bus）
- JS 层（`http.js`、`promise.js` 不变）
- `kwcc_io.c/h`（现有 API 已够用）

---

## 测试

**测试文件**：
- `tests/test_lifecycle_shutdown.c` — 纯 C 服务测试（mock entry：记录调用顺序/次数）
- `tests/test_http_shutdown.c` — HTTP C 端测试（cancel/reap/shutdown/on-demand）
- `tests/test_http_e2e.c` — 扩展：kill -TERM 僵尸验证（集成）

### 服务测试（test_lifecycle_shutdown.c）

| # | 测试 | 验证 |
|---|------|------|
| S1 | init 后 register N 个 mock entry，count 正确 | 注册表 |
| S2 | register 超过 MAX(8) → 第 9 个被拒绝 + 已注册的不受影响 | 上限防御 |
| S3 | force_all 逆序调用（记录调用顺序） | 逆序惯例 |
| S4 | force_all 幂等：二次调用 warn + skip，mock 不再被调 | 幂等守卫 |
| S5 | is_exiting：force_all 前 0，force_all 中 1（mock shutdown 内检查） | exiting 守卫 |
| S6 | shutdown 后 register → warn + 拒绝 | 退出后防御 |

### HTTP 测试（test_http_shutdown.c）

| # | 测试 | 验证 |
|---|------|------|
| H1 | cancel 后 slot 状态 `in_use=1, pipe_read_fd=-1`，pid 保留 | 逻辑放弃语义 |
| H2 | cancel 后子进程已退出 → check_progress 回收（`in_use=0`） | 自治 reap |
| H3 | cancel 后子进程未退出 → check_progress 跳过，下帧再试 | reap 条件 |
| H4 | cancelled slot 被 check_progress 回收时不 dispatch end/error | 无二次通知 |
| H5 | on-demand：8 slot 全占用 + 1 个已 cancelled 且子进程已退出 → request 成功复用 | slot 复用 |
| H6 | 连续 cancel 同一请求 → 第二次直接 return | 重复防御 |
| H7 | shutdown 后 `kwcc_http_request` 返回 NULL | g_shutdown 拒新请求 |
| H8 | 运行期直接调 `kwcc_http_shutdown`（未 force_all）→ warn + 跳过，活跃请求不受影响 | is_exiting 防御 |
| H9 | force_all 上下文中 shutdown：活跃请求被 SIGTERM + 回收，全部 slot `in_use=0` | 全量释放 |
| H10 | cleanup 对 pipe_read_fd=-1 不调 io_unregister | unregister(-1) 回归保护 |
| H11 | cancel 后 io reactor 不再触发 on_read | pipe 已关 |

### 集成测试

| # | 测试 | 验证 |
|---|------|------|
| E1 | 应用退出（窗口关闭）时无僵尸进程 | cleanup → force_all 路径 |
| E2 | `kill -TERM` / `kill -INT` 退出时无僵尸 curl | 信号 → force_all 路径 |
| E3 | JS 集成：cancel 后 Promise reject（不悬挂） | JS 侧契约不变 |

---

## 边界情况

| 场景 | 行为 |
|------|------|
| cancel 后子进程立刻退出 | check_progress 下一帧 reap |
| cancel 后子进程延迟退出 | check_progress 每帧尝试；request 满时 on-demand 紧急回收 |
| cancel 后子进程不响应 SIGPIPE | `--max-time` 超时退出；或退出时 shutdown() SIGTERM |
| 连续 cancel 同一请求 | `pipe_read_fd < 0` 防御，直接 return |
| 所有 8 slot 都 cancelled | check_progress 逐帧 reap；request 时 on-demand 紧急回收 |
| 应用退出时有活跃请求 | force_all → shutdown() → SIGTERM + waitpid 阻塞 + 全量回收 |
| on_read cancel 后仍触发 | `pipe_read_fd < 0` 防御 |
| 正常请求完成（on_read EOF） | 原有逻辑不变 |
| force_all 后再调 force_all | exiting=1 幂等守卫，warn + skip |
| force_all 后再调 register | exiting=1 防御，warn + 拒绝 |
| 窗口关闭 cleanup 期间收到 SIGINT | cleanup 已置 exiting=1 → 信号处理中 force_all 幂等跳过 → _exit |
| 运行期误调 kwcc_http_shutdown | is_exiting=0 → warn + 跳过，正常请求不受影响 |
