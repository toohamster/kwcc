# lifecycle_shutdown 服务 v2 思路探讨

> 基于 v1 方案（`lifecycle-shutdown-service-spec.md` + `http-shutdown-implementation-plan.md`）的演进讨论
> 本文件是思路草稿，不是正式方案

---

## 讨论 1：服务自身的状态结构化

v1 的问题：服务自身数据散落为多个 static 变量（`g_count`、`g_shutdown`、`g_bus_sub_id`），没有内聚。

**思路**：服务有自身的 data struct + 行为函数，模块接入的 entry 也是 data struct + 行为函数（行为由模块实现）。

```c
/* 服务运行时状态 */
typedef struct {
    int exiting;        // 0=运行中，1=收到 kill 正在退出
    int module_count;   // 已注册 entry 数量
    int poll_count;     // poll 触发总次数
    // 未来：last_poll_time、shutdown_start_time 等
} kwcc_lifecycle_shutdown_state_t;
```

- `module_count` 是 entry 数量的聚合
- `exiting` 是服务自身的生命周期状态（kill 信号 → 1）
- 监控只需关注 `get_state()` 就够了

---

## 讨论 2：entry 的通用访问函数

v1 里 entry 的字段（name、dirty_count、threshold）对服务是可见的（服务定义了它）。

**问题**：外部（监控、调试）如何访问单个模块的数据？

**待确认**：
- 服务是否需要提供 `get_module_name(index)` / `get_module_dirty_count(index)` 这样的接口？
- 还是通过轮询（遍历 entries）由服务自身完成，外部只读 `state_t`？
- index vs name 作为查询键：都不合适，服务通过轮询即可，不需要外部按名查找
- 模块别名/标记是否有必要？

---

## 讨论 3：dirty_count 的维护方式

v1 里 dirty_count 是模块实现的函数指针，服务调 `entry->dirty_count()` 得到数字，和 `entry->threshold` 比较。

**待确认**：
- dirty_count 和 threshold 的关系，是模块自己定义还是服务统一管理？
- 服务的职责是"触发"——"你的脏资源数超过阈值了，该回收了"
- 阈值是模块级还是全局级？
- dirty_count 的维护机制：实时计算（当前 HTTP 方案）vs 其他方式？

---

## 讨论 4：shutdown(force=1) 运行期防御

v1 确认了 shutdown(force=1) 运行期禁止直接调，但缺少代码防御。

**思路**：`state->exiting` 作为全局守卫，模块 shutdown(force=1) 入口检查：
- `exiting == 1` → 正常执行（在 force_all 上下文中）
- `exiting == 0` → `log_warn` + 跳过（运行期误调）

服务提供 `kwcc_lifecycle_shutdown_is_exiting()` 暴露 `state->exiting`。

---

## 讨论 5：force_all 不走 bus

v1 已确认：force_all 直接调各模块的 shutdown(1)，不走 bus。原因：kill 信号时 bus 可能不可用。

---

## 讨论 6：shutdown(force=1) 不需要 grace period

v1 已确认：SIGTERM + waitpid(阻塞) 足够。curl 是可靠响应 SIGTERM 的轻量子进程，不需要 sleep 循环或 SIGKILL。

---

## 讨论 7：kill 信号处理

Sokol 的 cleanup_cb 只在窗口关闭时触发。需要注册 SIGINT/SIGTERM 处理函数，调 force_all()。

---

## 待继续讨论

- 讨论 2：entry 的访问方式
- 讨论 3：dirty_count 的维护机制
