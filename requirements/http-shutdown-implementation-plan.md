# Shutdown 链 + HTTP cancel 僵尸进程修复 — 实施方案

> 架构规范见 [module-shutdown-spec.md](module-shutdown-spec.md)
> 本文件只记录 HTTP 模块的实施细节

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

## 状态模型：二态 + 隐式信号

不需要 `cancelled` 字段。用 `pipe_read_fd == -1` 作为"已取消"的隐式信号：

| in_use | pipe_read_fd | 含义 | check_progress | shutdown(false) | shutdown(true) |
|--------|-------------|------|---------------|-----------------|----------------|
| 0 | — | 空闲 | 跳过 | 跳过 | 跳过 |
| 1 | >= 0 | 正常请求 | 汇报进度 | 跳过 | 终止 + 回收 |
| 1 | -1 | 已取消，子进程可能还在跑 | 跳过 | waitpid(WNOHANG) 尝试回收 | 终止 + 回收 |

---

## `kwcc_http_shutdown` 实现

```c
static int g_kwcc_http_shutdown = 0;

void kwcc_http_shutdown(bool force) {
    if (force) {
        /* ── 全量释放 ── */
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
            if (req->pid > 0) {
                kill(req->pid, SIGTERM);
            }
        }

        /* Phase 2: grace period（同步阻塞，最多 100ms） */
        for (int attempt = 0; attempt < 10; attempt++) {
            int all_reaped = 1;
            for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
                kwcc_http_req_t *req = &g_kwcc_http_reqs[i];
                if (!req->in_use || req->pid <= 0) continue;
                int status;
                pid_t ret = waitpid(req->pid, &status, WNOHANG);
                if (ret > 0) {
                    kwcc_http_cleanup(req);
                } else if (ret == 0) {
                    all_reaped = 0;
                }
            }
            if (all_reaped) break;
            usleep(10000);  // 10ms
        }

        /* Phase 3: SIGKILL 强杀剩余 */
        for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
            kwcc_http_req_t *req = &g_kwcc_http_reqs[i];
            if (!req->in_use || req->pid <= 0) continue;
            kill(req->pid, SIGKILL);
            waitpid(req->pid, NULL, 0);
            kwcc_http_cleanup(req);
        }

        log_info("http: shutdown complete");
    } else {
        /* ── 轻量释放 ── */
        for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
            kwcc_http_req_t *req = &g_kwcc_http_reqs[i];
            if (!req->in_use) continue;
            if (req->pipe_read_fd >= 0) continue;  // 正常请求，不管
            if (req->pid <= 0) continue;

            int status;
            pid_t ret = waitpid(req->pid, &status, WNOHANG);
            if (ret > 0) {
                kwcc_http_cleanup(req);
            }
        }
    }
}
```

---

## `kwcc_http_cancel` 简化

```c
void kwcc_http_cancel(const char *req_id) {
    if (!req_id) return;
    kwcc_http_req_t *req = kwcc_http_req_find(req_id);
    if (!req) return;

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
    /* in_use 保持 1，pid 保留，等 shutdown 回收 */
}
```

---

## `kwcc_http_request` 防御

```c
const char *kwcc_http_request(...) {
    if (g_kwcc_http_shutdown) return NULL;
    // ... 原有逻辑
}
```

---

## `kwcc_http_check_progress` 简化

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

---

## `kwcc_http_on_read` 防御

```c
static void kwcc_http_on_read(int fd, void *user_data) {
    kwcc_http_req_t *req = (kwcc_http_req_t *)user_data;
    if (req->pipe_read_fd < 0) return;
    // ... 原有逻辑
}
```

---

## `kwcc_js_http.c` 改动

```c
static void http_unload(kwcc_js_ops_t *ops) {
    kwcc_http_shutdown(true);
}

kwcc_js_module_t kwcc_js_http_module = {
    .name        = "http",
    .load        = http_load,
    .apis        = http_apis,
    .on_bus_event = http_on_bus_event,
    .unload      = http_unload,
};
```

---

## `kwcc_js.c` 改动

`kwcc_destroy_js` 逆序遍历模块调 `unload`：

```c
void kwcc_destroy_js(JSContext *ctx) {
    for (int i = g_kwcc_js_module_count - 1; i >= 0; i--) {
        if (g_kwcc_js_modules[i]->unload) {
            log_info("js: unloading module '%s'", g_kwcc_js_modules[i]->name);
            g_kwcc_js_modules[i]->unload(&g_kwcc_js_ops);
        }
    }
    if (ctx) {
        JS_FreeContext(ctx);
    }
}
```

---

## `main.m` 改动

```c
/* frame() */
static void frame(void) {
    kwcc_io_poll_once();
    kwcc_http_check_progress();
    kwcc_http_shutdown(false);       // 轻量释放
    kwcc_process_js(g_js_ctx, "onFrame();");
    // ...
}

/* cleanup() */
static void cleanup(void) {
    kwcc_destroy_js(g_js_ctx);       // 内部：模块 unload → JS_FreeContext
    kwcc_mempool_shutdown();
}
```

---

## 改动汇总

| 文件 | 改动 |
|------|------|
| `src/kwcc_http.h` | 新增 `kwcc_http_shutdown(bool force)` 声明 |
| `src/kwcc_http.c` | 新增 `g_kwcc_http_shutdown` 标志 |
| `src/kwcc_http.c` | `kwcc_http_request` 加 shutdown 防御 |
| `src/kwcc_http.c` | `kwcc_http_cancel`：只关 pipe + 发布事件 |
| `src/kwcc_http.c` | `kwcc_http_check_progress`：跳过 `pipe_read_fd < 0` |
| `src/kwcc_http.c` | `kwcc_http_on_read`：跳过 `pipe_read_fd < 0` |
| `src/kwcc_http.c` | 新增 `kwcc_http_shutdown(bool force)` 实现 |
| `src/kwcc_js_http.c` | 新增 `http_unload`，描述符加 `.unload` |
| `src/kwcc_js.c` | `kwcc_destroy_js` 逆序调模块 `unload` |
| `src/main.m` | `frame()` 加 `kwcc_http_shutdown(false)` |

不改动：
- `kwcc_http_req_t`（不需要 `cancelled` 字段）
- `kwcc_http_cleanup`（逻辑不变）
- JS 层（`http.js`、`promise.js` 不变）

---

## 测试

| # | 测试 | 验证 |
|---|------|------|
| 1 | cancel 后 slot 状态 | `in_use=1, pipe_read_fd=-1` |
| 2 | `shutdown(false)` 回收已退出的 cancelled 请求 | `in_use=0` |
| 3 | `shutdown(true)` 终止所有活跃请求 | 所有 slot `in_use=0` |
| 4 | cancel 后新请求可复用 slot | `shutdown(false)` 回收后 slot 可用 |
| 5 | cancel 时 pipe 已关闭 | IO reactor 不再触发 `on_read` |
| 6 | cancel 后 JS 收到 reject | Promise 不悬挂 |
| 7 | 连续 cancel 安全 | 重复操作无害 |
| 8 | 应用退出时无僵尸进程 | `ps` 验证无 zombie curl |
| 9 | `kwcc_destroy_js` 调模块 unload | HTTP 请求被终止 + 回收 |
| 10 | 模块无 unload 时不 crash | `if (unload)` NULL 检查 |
| 11 | shutdown 后拒绝新请求 | `kwcc_http_request` 返回 NULL |

---

## 边界情况

| 场景 | 行为 |
|------|------|
| cancel 后子进程立刻退出 | `shutdown(false)` 下一帧回收 |
| cancel 后子进程延迟退出 | `shutdown(false)` 持续尝试 |
| cancel 后子进程不响应 SIGPIPE | `--max-time` 超时退出，或 `shutdown(true)` 时 SIGTERM → SIGKILL |
| 连续 cancel 同一请求 | `pipe_read_fd` 已为 -1，重复 close(-1) 无害 |
| 所有 8 slot 都 cancelled | `shutdown(false)` 逐帧回收；极端等 `shutdown(true)` |
| 应用退出时有活跃请求 | `kwcc_destroy_js` → `http_unload` → `shutdown(true)` |
| `on_read` cancel 后仍触发 | `pipe_read_fd < 0` 防御 |
| 正常请求完成（on_read EOF） | 原有逻辑不变 |
