# requirements/ 目录方案状态汇总

> 更新于 2026-07-05

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
| 14b | `async-io-implementation-plan.md` | ✅ 全部完成 | Layer 1-4 完成，Step 4-7 由 #17/#18/#20 完成 | — |
| 15 | `microui-id-override.md` | ⏳ 待论证 | — | — |
| 16 | `mquickjs-cfunc-registration.md` | ⚠️ 参考 | C 函数注册技术参考 | — |
| 17 | `js-bridge-architecture.md` | ✅ 已完成 | Facade + Plugin 架构，ops 测试 74/74 | 74/74 |
| 18 | `js-http-implementation-plan.md` | ✅ 已完成 | HTTP Plugin 模块，Step 1-7 完成 | — |
| 20 | `js-module-dispatch-plan.md` | ✅ 已完成 | module-grouped 两级分发，ops 测试 74/74 | 74/74 |
| 21 | `js-bridge-dispatch-http-progress.md` | ✅ 已完成 | #17/#18/#20 进度汇总 | — |
| 19 | `kwcc-base-defer-cleanup.md` | ✅ 已完成 | 独立基础设施，无前置依赖 | 14/14 |

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
```

## 代码清理 🧹

| 操作 | 说明 |
|------|------|
| 删除 `src/kwcc_pool.h/c` | 旧版三层 Slab 内存池，已被 kwcc_mempool (L0-L7) 取代，无引用 |

## 下一步可以做

1. **microui-id-override (#15)** — 窗口 ID 覆盖机制（待论证）
