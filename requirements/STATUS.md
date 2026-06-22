# requirements/ 目录方案状态汇总

> 更新于 2026-06-22

## 已完成 ✅

| 文件 | 状态 | 说明 |
|------|------|------|
| `mempool-design.md` | ✅ 全部完成 | Phase 1-7 全部完成，24/24 测试通过 |
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
| `bus-split-design.md` | ⏳ 待论证（优先级最高） | kwcc_bus 拆分为纯 C 事件总线 + JS 桥接层 |
| `async-io-design.md` | ⏳ 待论证（依赖 bus 拆分） | 单线程异步 I/O + Promise，已合并为统一方案 |
| `async-io-promise.md` | 📁 保留参考 | 旧方案，内容已合并到 async-io-design.md |
| `async-io-implementation-plan.md` | 📁 保留参考 | 旧实施计划，内容已合并到 async-io-design.md |
| `microui-id-override.md` | ⏳ 待论证 | microui ID 覆盖机制，优先级在 async-io 之后 |

## 已废弃 ❌

| 文件 | 状态 | 说明 |
|------|------|------|
| `core-memory-pool.md` | ❌ 已废弃 | 已被 mempool-design.md (v7) 取代 |
| `core-memory-pool-plan.md` | ❌ 已废弃 | 已被 mempool-design.md (v7) 取代 |
| `store-data-flow.md` | ❌ 已废弃 | v1 版本，已被 v2 取代 |
| `mquickjs-cfunc-registration.md` | ⚠️ 参考资料 | C 函数注册方案，作为技术参考保留 |

## 下一步可以做

1. **bus-split-design.md** — kwcc_bus 拆分（优先级最高，async-io 前置依赖）
2. **async-io-design.md** — 异步 I/O + Promise（依赖 bus 拆分完成后实施）
3. **microui-id-override.md** — 窗口 ID 覆盖机制（待论证）
