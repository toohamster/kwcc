# kwcc

> **KWCC (Kite Window Canvas Core)** — A tiny, self-contained desktop UI engine.
>
> Kite（风筝）取轻盈之意 · Window 桌面窗口 · Canvas 画布渲染 · Core 自包含核心引擎

极轻量级自包含桌面 UI 引擎，macOS 10.14+。

**零外部依赖** — 所有第三方源码通过 `setup.sh` 下载到 `deps/` 目录，开发者只需有编译器即可运行。

## 架构

```
mquickjs (脚本逻辑) → microui (IMGUI 布局) → NanoVG (向量渲染) → Sokol (窗口/GFX)
```

## 快速开始

```bash
# 下载依赖
./setup.sh

# 编译运行
make && ./kwcc
```

两阶段构建：
1. 编译 host tool 生成 `mqjs_stdlib.h`
2. 编译主二进制（仅 4 个 mquickjs 核心 .c 文件）

## 特性

- **单向数据流**：`$store.state` + `$bus` 事件总线 + 模块注册体系
- **C Pub/Sub 事件总线**：纯 C 层 publish/subscribe，UI 桥接 + JS 白名单分离
- **SVG 渲染**：支持文件路径和内联 SVG 字符串，128 槽帧安全缓存
- **多窗口**：microui 原生多窗口同时渲染
- **中文支持**：自动识别 CJK 字体，PingFang SC 默认字体
- **ES5 脚本层**：mquickjs 引擎，轻量快速

## 示例模块

所有示例位于 `app/modules/examples/`：

| 模块 | 说明 |
|------|------|
| `test/` | 最小计数器，验证 store + event bus 闭环 |
| `calc/` | 完整计算器，四则运算 + 状态驱动 UI |
| `svg/` | SVG 渲染，内联字符串 + 文件路径两种方式 |

## 项目结构

```
├── deps/            # 第三方源码 (setup.sh 自动下载)
├── src/
│   ├── main.m       # Sokol 生命周期 + NanoVG 渲染
│   ├── kwcc_base.h/c # 纯 C 基础设施（内存池常量 + topic 清洗/校验）
│   ├── kwcc_mempool.h/c # L0-L7 Slab 内存池 + key_map + 常量表 + GC + TLV
│   ├── kwcc_config.h/c  # Config 层：App/Core 域存取接口
│   ├── kwcc_ui.c/h  # UI 模块（g_mu、microui 桥接、input、SVG、字体）
│   ├── kwcc_js.c/h  # JS lifecycle + bus consumer + JS 白名单
│   ├── kwcc_ui_bus.c/h # UI→JS 事件桥接（topic map + dispatch_event）
│   ├── kwcc_bus.c/h # 通用 C Pub/Sub 事件总线（subscribe/publish/unsubscribe）
│   ├── kwcc.h       # 入口 umbrella header（聚合各模块头文件）
│   └── kwcc_io.h    # I/O 模块声明
├── app/
│   ├── main.js      # 模块入口
│   ├── runtime/     # store.js + bus.js
│   └── modules/examples/  # 示例模块
└── assets/          # 字体资源
```

## 开发规范

- **JS 语法**: ES5 兼容（无 let/const/箭头函数/模板字符串）
- **C 层**: 非必要不修改
- **调试**: macOS 使用 `lldb` 定位崩溃，不使用 printf

## 文档

**已完成**：
- [spec.md](spec.md) — 完整项目规范
- [requirements/mempool-design.md](requirements/mempool-design.md) — Slab 内存池方案
- [requirements/bus-split-design.md](requirements/bus-split-design.md) — Bus 拆分方案
- [requirements/bus-split-implementation-plan.md](requirements/bus-split-implementation-plan.md) — Bus 拆分实施计划
- [requirements/store-data-flow-v2.md](requirements/store-data-flow-v2.md) — 单向数据流方案
- [requirements/store-data-flow-v2-plan.md](requirements/store-data-flow-v2-plan.md) — 单向数据流实施计划
- [requirements/svg-enhancement-scheme.md](requirements/svg-enhancement-scheme.md) — SVG 缓存增强方案
- [requirements/calculator-scheme.md](requirements/calculator-scheme.md) — 计算器示例方案
- [requirements/extract-bus-module.md](requirements/extract-bus-module.md) — Bus 模块提取方案
- [requirements/extract-ui-module.md](requirements/extract-ui-module.md) — UI 模块提取方案
- [requirements/naming-fix.md](requirements/naming-fix.md) — 命名规范整改方案

**待实施**：
- [requirements/async-io-design.md](requirements/async-io-design.md) — 异步 I/O + Promise 方案
- [requirements/microui-id-override.md](requirements/microui-id-override.md) — microui ID 覆盖机制

## 许可证

MIT
