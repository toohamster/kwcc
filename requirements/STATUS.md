# requirements/ 目录方案状态汇总

> 更新于 2026-06-25

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
| 14 | `async-io-design.md` | ⏳ 待实施 | 依赖 bus-split，已解除 | — |
| 14b | `async-io-implementation-plan.md` | ⏳ 待实施 | — | — |
| 15 | `microui-id-override.md` | ⏳ 待论证 | — | — |
| 16 | `mquickjs-cfunc-registration.md` | ⚠️ 参考 | C 函数注册技术参考 | — |

## 代码清理 🧹

| 操作 | 说明 |
|------|------|
| 删除 `src/kwcc_pool.h/c` | 旧版三层 Slab 内存池，已被 kwcc_mempool (L0-L7) 取代，无引用 |

## 下一步可以做

1. **async-io-design.md** — 异步 I/O + Promise（bus 拆分已完成，前置依赖已满足）
2. **microui-id-override.md** — 窗口 ID 覆盖机制（待论证）
