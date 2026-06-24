# requirements/ 目录方案状态汇总

> 更新于 2026-06-24

## 已完成 ✅

| 文件 | 状态 | 说明 |
|------|------|------|
| `mempool-design.md` | ✅ 全部完成 | Phase 1-7 全部完成，24/24 测试通过 |
| `bus-split-design.md` | ✅ 全部完成 | 7 步全部完成，19/19 测试通过，kwcc_bus 拆分为纯 C Pub/Sub + UI 桥接 + JS 白名单 |
| `bus-split-implementation-plan.md` | ✅ 全部完成 | 执行计划已全部实施 |
| `extract-bus-module.md` | ✅ 已完成 | bus 模块已提取到 kwcc_bus.c/h |
| `extract-ui-module.md` | ✅ 已完成 | UI/JS/config 模块已拆分 |
| `naming-fix.md` | ✅ 已完成 | 命名规范整改已完成 |
| `store-data-flow-v2.md` | ✅ 已完成 | 单向数据流 v2 已实施 |
| `store-data-flow-v2-plan.md` | ✅ 已完成 | 开发计划已执行完毕 |
| `svg-enhancement-scheme.md` | ✅ 已完成 | SVG 缓存增强已实施 |
| `calculator-scheme.md` | ✅ 已完成 | 计算器示例已完成 |

## 待论证 ⏳

| 文件 | 状态 | 说明 |
|------|------|------|
| `async-io-design.md` | ⏳ 待论证 | 单线程异步 I/O + Promise，bus 拆分已完成，可启动 |
| `microui-id-override.md` | ⏳ 待论证 | microui ID 覆盖机制，优先级在 async-io 之后 |

## 已废弃 ❌

| 文件 | 状态 | 说明 |
|------|------|------|
| `core-memory-pool.md` | ❌ 已废弃 | 已被 mempool-design.md (v7) 取代 |
| `core-memory-pool-plan.md` | ❌ 已废弃 | 已被 mempool-design.md (v7) 取代 |
| `store-data-flow.md` | ❌ 已废弃 | v1 版本，已被 v2 取代 |
| `mquickjs-cfunc-registration.md` | ⚠️ 参考资料 | C 函数注册方案，作为技术参考保留 |

## 代码清理 🧹

| 操作 | 说明 |
|------|------|
| 删除 `src/kwcc_pool.h/c` | 旧版三层 Slab 内存池，已被 kwcc_mempool (L0-L7) 取代，无引用 |

## 下一步可以做

1. **async-io-design.md** — 异步 I/O + Promise（bus 拆分已完成，前置依赖已满足）
2. **microui-id-override.md** — 窗口 ID 覆盖机制（待论证）
