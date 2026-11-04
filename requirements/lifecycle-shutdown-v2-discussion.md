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

> **演进（讨论 3 决策后）**：`poll_count` 随 runtime 路径移除而删除（服务不再 poll，不订阅 bus）。state 收敛为 `{ exiting, module_count }`。`get_state()` 暂不提供——没有真实监控需求，`is_exiting()` 覆盖运行期防御（讨论 4），出现真实监控需求时再加是向后兼容扩展。

---

## 讨论 2：entry 的通用访问函数 ✅ 已决策

**决策：服务不提供任何 entry 访问接口，外部只读 `is_exiting()`。**

理由：

1. entry 简化为 `name + shutdown` 后（讨论 3），没有 dirty_count 等运行数据可查，"按 name/index 访问单个模块数据"已经没有对象
2. 监控需求是假设性的：kwcc 模块数编译期个位数，出现真实监控需求时再加 `get_state()` 是向后兼容扩展
3. 运行期防御需要的状态（exiting）通过 `is_exiting()` 暴露（讨论 4），这是唯一有真实调用方的查询接口

---

## 讨论 3：dirty_count 的维护方式 ✅ 已决策

**决策：窄边界（C）** — lifecycle_shutdown 服务的职责收敛为**仅 force_all（退出清理调度器）**，runtime 回收交给模块自治。

### 决策理由

1. **唯一真实客户反推**：runtime 路径的全部需求来自 HTTP cancel 僵尸场景。而 HTTP 的 `check_progress()` 每帧已经在扫 slot 数组，reap 是它的附属动作，成本为 0。让服务轮询 dirty_count 再回调 `shutdown(0)`，是把信息源和决策点人为分离，没买到任何东西。

2. **服务不可替代的价值在退出期**：force_all 的硬约束（注册逆序、kill 信号时 bus 可用、全局 `exiting` 守卫）才是"服务"该存在的原因。runtime 回收模块自己就能做。

3. **不设计假设性需求**：跨模块统一 runtime 策略（如"全局脏资源超标强制 GC"）在 kwcc 模块数（编译期个位数）下是假设性的。真出现时 entry 加字段是扩展，不是推翻。

### 决策带来的变化

- entry 结构简化：删掉 `dirty_count` 和 `threshold`，只留 `name` + `shutdown`
- 服务不再订阅 bus `frame/tick`（runtime 路径整体移除）
- HTTP 的 `reap_cancelled()` 逻辑并入 `check_progress()`，由 HTTP 自治调用
- 服务不再依赖 bus（force_all 不走 bus，runtime 也不再走 bus）
- 讨论 2（entry 访问函数）大幅简化：entry 只有 name + shutdown，没有 dirty_count 可查

### 兜底

未来若出现模块无法在 per-frame 循环中自治回收的场景（状态感知点不在帧内），再加拉模型是向后兼容扩展，不是推翻。

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

## 讨论闭环

全部 7 项讨论已闭环，决策已沉淀为正式方案：

- 服务规范 v2：[lifecycle-shutdown-service-spec.md](lifecycle-shutdown-service-spec.md)
- HTTP 接入 v2：[http-shutdown-implementation-plan.md](http-shutdown-implementation-plan.md)
