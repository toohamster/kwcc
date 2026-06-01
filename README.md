# kwcc

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
│   ├── kwcc.c       # JS ↔ microui ↔ NanoVG 桥接
│   └── kwcc.h       # 公共 API
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

- [spec.md](spec.md) — 完整项目规范
- [requirements/store-data-flow-v2.md](requirements/store-data-flow-v2.md) — 单向数据流方案
- [requirements/store-data-flow-v2-plan.md](requirements/store-data-flow-v2-plan.md) — 实施计划

## 许可证

MIT
