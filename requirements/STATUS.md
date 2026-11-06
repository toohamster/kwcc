# requirements/ 方案状态 + Backlog

> 更新于 2026-11-04

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
| 22 | `lifecycle-shutdown-service-spec.md` | ⏸ 服务暂缓（泛化蓝图） | 方案一降级：单客户世界不建服务，校验规则应用于服务自身 | — |
| 22b | `http-shutdown-implementation-plan.md` | ✅ 方案一完成 | cancel 逻辑放弃 + 自治 reap + shutdown 直调（服务壳移除），L1 11/11 + 实机 kill 冒烟 | 11/11 |
| 22c | `lifecycle-shutdown-v2-discussion.md` | ✅ 讨论闭环 | v1→v2 演进讨论（7 项），决策沉淀到 #22/#22b | — |
| 25 | `edit-control-implementation-plan.md` | ✅ 完成 | 编辑控件：GL 展示 + 原生编辑委托（AppKit 后端）+ ops 接口隔离；L1 60/60 + L2 用户验收（中文 IME ✓）；实施期事故立法见章程 | 60/60 |

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

── shutdown 方向（v0.0.5+）──

lifecycle-shutdown-service (#22, v2 窄边界) ⏳ ──→ http-shutdown (#22b, v2) ⏳
                                    │
                                    └─→ 本地存储 (#23) ⏳ ──→ 端到端示例 (#24) ⏳
                                          见 Backlog（降级）      见 Backlog（改造为 UI 演示）

v2 讨论闭环（2026-11-04）：#22/#22b 设计收敛，见 #22c；v2 讨论稿 + 方案修订未提交，待用户确认
```

## 修复记录

| 日期 | 修复 | 根因 | 验收 |
|------|------|------|------|
| 2026-11-04 | 窗口 X 关闭（title→topic map + 可见性挡板 widget 级补全方案 A） | 挡板只拦 begin/endWindow，body widget 落 microui 空栈 → expect abort（用户实机 microui.c:261）；dispatch 层 title/topic 不匹配 | L1 7/7（tests/TESTING_WINDOW_CLOSE.md）+ L2 用户验收通过；章程流程首次全程执行，催生《验证与验收章程》 |

## 代码清理 🧹

| 操作 | 说明 |
|------|------|
| 删除 `src/kwcc_pool.h/c` | 旧版三层 Slab 内存池，已被 kwcc_mempool (L0-L7) 取代，无引用 |

## Backlog（待细化方向）

> 方向已确认但未出正式方案。细化后拆出独立文件，编号分配到时间线表。
>
> **立项校验规则（2026-11-04 规划评审确定）**：每个新方案立项前先回答——"这个功能让'用 JS 构建 UI'变强了吗，还是只有系统层受益？"系统能力只有在阻塞真实 UI 应用时才做。背景：v0.0.3 后工作集中在系统层（bus/mempool/HTTP/dispatch），UI 主业（控件/布局/交互）自 SVG 后未再前进，存在偏差。
>
> **演进（2026-11-04）**：规则适用范围修正——**能力增类**功能（给引擎加能力）仍按本规则执行；**结构预留类**决策（耦合点、消息边界、契约标定等"装不回去"的属性）不受"当前无证据"否决，以成熟期目标应用形态为架构基准（目标形态与判定标准见 [design/architecture-boundary-rationale.md](../design/architecture-boundary-rationale.md)）。

### P1：UI 主业回归（Phase 2，Phase 1 收尾后开始）

**背景**：kwcc 定位是桌面 UI 引擎，但 UI 核心层控件停在 SVG（9 个控件），最大短板是 UI 本身。检验标准：能否用 kwcc 写出"应用级"界面（表单/设置面板）——目前卡在没有文本输入框。

**方向概要**（编号预分配）：
- **#25 `ui.textbox` 文本输入**：✅ 已出正式方案（[edit-control-implementation-plan.md](edit-control-implementation-plan.md)）——编辑委托 OS 原生控件（AppKit 后端），mu_textbox 桥接/自研核心两方向已否决，待审定后实施
- **#26 控件补齐**：checkbox → icon / header / treenode（microui 已有实现，主要是桥接成本，按 ui.* 标准四步流程推进）
- **#27 布局增强**：自动宽度 / 比例列 / 超 4 列——由 #24 示例需求倒逼，按需做

### P2：本地存储（#23）— 降级：不独立立项，示例需要持久化时顺势实现

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

### P3：端到端示例（#24）— 改造为 UI 能力演示

**依赖**：#25/#26（示例展示 UI 能力为主，HTTP 作为可选数据源）

**背景**：原计划 ipcheck（HTTP 演示）方向偏移——展示的是网络能力而非 UI 能力。改造为让开发者一眼看出"用 JS 能构建什么样的桌面应用界面"。

**方向概要**：
- 示例模块：设置面板/表单类（textbox + checkbox + slider 组合），展示 UI 构建能力
- 可选数据源：HTTP（如天气/IP 展示），展示 UI + 网络结合
- 如果 #23 完成，可追加 `$config.save()` 持久化设置
