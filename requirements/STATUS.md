# requirements/ 方案状态 + Backlog

> 更新于 2026-07-07

## 开发时间线

| # | 方案 | 状态 | 演进关系 | 测试 |
|---|------|------|----------|------|
| 1 | `core-memory-pool.md` | ❌ 已废弃 | → 被 mempool-design (v7) 取代 | — |
| 2 | `core-memory-pool-plan.md` | ❌ 已废弃 | → 被 mempool-design (v7) 取代 | — |
| 3 | `mempool-design.md` | ✅ 全部完成 | 取代 core-memory-pool | 24/24 |
| 4 | `store-data-flow.md` | ❌ 已废弃 | → 被 v2 取代 | — |
| 5 | `store-data-flow-v2.md` | ✅ 已完成 | 取代 v1 | — |
| 6 | `store-data-flow-v2-plan.md` | ✅ 已完成 | — | — |
| 7 | `svg-enhancement-scheme.md` | ✅ 已完成 | — | — |
| 8 | `calculator-scheme.md` | ✅ 已完成 | — | — |
| 9 | `extract-ui-module.md` | ✅ 已完成 | jsapi 重命名为 kwcc_js | — |
| 10 | `extract-bus-module.md` | ✅ 已完成 | → 被 bus-split 重构取代 | — |
| 11 | `naming-fix.md` | ✅ 已完成 | — | — |
| 12 | `bus-split-design.md` | ✅ 全部完成 | 从 extract-bus-module 演进，拆分为三层 | 19/19 |
| 13 | `bus-split-implementation-plan.md` | ✅ 全部完成 | — | — |
| 14 | `async-io-design.md` | ✅ 全部完成 | 依赖 bus-split，已解除 | — |
| 14b | `async-io-implementation-plan.md` | ✅ 全部完成 | Layer 1-4 完成，Step 8 JS 集成测试 22/22 | A15+B7 |
| 15 | `microui-id-override.md` | ⏳ 待论证 | — | — |
| 16 | `mquickjs-cfunc-registration.md` | ⚠️ 参考 | C 函数注册技术参考 | — |
| 17 | `js-bridge-architecture.md` | ✅ 已完成 | Facade + Plugin 架构，ops 测试 74/74 | 74/74 |
| 18 | `js-http-implementation-plan.md` | ✅ 全部完成 | HTTP Plugin 模块，Step 1-8 全部完成 | C11+JS22 |
| 19 | `kwcc-base-defer-cleanup.md` | ✅ 已完成 | 独立基础设施，无前置依赖 | 14/14 |
| 20 | `js-module-dispatch-plan.md` | ✅ 已完成 | module-grouped 两级分发，ops 测试 74/74 | 74/74 |
| 21 | `js-bridge-dispatch-http-progress.md` | ✅ 已完成 | #17/#18/#20 进度汇总 | — |
| 22 | `module-shutdown-spec.md` | ⏳ 设计完成，待实施 | 模块 shutdown 链架构规范 | — |
| 22b | `http-shutdown-implementation-plan.md` | ⏳ 设计完成，待实施 | HTTP 模块 shutdown 实施计划，依赖 #22 | — |

## 方案依赖关系

```
bus-split (#12) ──→ async-io-design (#14) ✅ ──→ async-io-implementation-plan (#14b) ✅
                                                         │
                                                         ├─ Layer 1 (I/O Reactor) ✅
                                                         ├─ Layer 2 (kwcc_http.c) ✅
                                                         ├─ js-bridge-architecture (#17) ✅
                                                         ├─ js-module-dispatch-plan (#20) ✅
                                                         └─ js-http-implementation-plan (#18) ✅

js-bridge-dispatch-http-progress (#21) — #17/#18/#20 进度汇总

kwcc-base-defer-cleanup (#19) ✅ ──→ async-io (#14b) Step 5 (重构 kwcc_http_request) ✅
  ↑ 独立基础设施，无前置依赖

── v0.0.4 ──

module-shutdown-spec (#22) ⏳ ──→ http-shutdown-implementation-plan (#22b) ⏳
                                    │
                                    └─→ 本地存储 (#23) ⏳ ──→ 端到端示例 (#24) ⏳
                                          见 Backlog               见 Backlog
```

## 代码清理 🧹

| 操作 | 说明 |
|------|------|
| 删除 `src/kwcc_pool.h/c` | 旧版三层 Slab 内存池，已被 kwcc_mempool (L0-L7) 取代，无引用 |

## Backlog（待细化方向）

> 方向已确认但未出正式方案。细化后拆出独立文件，编号分配到时间线表。

### P2：本地存储（#23）

**依赖**：#22（先修缺陷再做新功能）

**背景**：当前 mempool 是纯内存的，应用关闭 = 数据丢失。最小改动是给 `$config` 加 `save()/load()`，复用已有的 TLV serialize。

**方向概要**：
- C 层新增 `kwcc_config_save(domain, path)` / `kwcc_config_load(domain, path)`
- 原子写流程：write tmpfile → rename（POSIX 保证原子）
- JS 层新增 `$config.save()` / `$config.load()`
- 启动流程：mempool_init 之后调 `kwcc_config_load("app", "kwcc_config.dat")`

**设计决策待确认**：
- 默认存储路径：当前工作目录 vs macOS `~/Library/Application Support/kwcc/`
- 保存范围：全部 vs 选择性排除临时数据
- load 冲突处理：已有初始值是否覆盖

---

### P3：端到端示例（#24）

**依赖**：#23（示例可以展示本地存储能力）

**背景**：v0.0.3 有 HTTP 能力但没有示例展示。需要一个端到端示例让人一眼看出"这个引擎能做什么网络应用"。

**方向概要**：
- 示例模块：`ipcheck`（点击按钮调 httpbin.org/ip 获取 IP，显示在窗口中）
- 验证完整链路：UI 按钮 → bus 事件 → JS handler → $http.fetch → C 层 fork+curl → pipe 返回 → notify_js → JS handler → store dispatch → UI 渲染
- 如果 #23 完成，可追加 `$config.save()` 持久化查询结果
