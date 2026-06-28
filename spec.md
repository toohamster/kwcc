# Spec: kwcc - 极轻量级自包含 UI 引擎

> **KWCC (Kite Window Canvas Core)** — A tiny, self-contained desktop UI engine.
>
> - **Kite**（风筝）：轻盈、灵动、低负载，象征引擎极轻量、体积小巧、运行高效
> - **Window**：面向桌面窗口场景
> - **Canvas**：基于 NanoVG 画布实现界面渲染
> - **Core**：自包含的底层 UI 核心引擎，零外部依赖
>
> 整体定位：一款极轻量、自包含、无外部依赖的桌面 UI 引擎。

## 1. 项目愿景
构建一个 100% 源码依赖、无外部库安装需求的极轻量桌面 UI 引擎。
- **核心理念**: 开发者只需拥有编译器即可运行，无需 `brew install`。
- **架构**: 使用脚本逻辑 (mquickjs) 驱动 UI 布局 (microui)，通过高性能向量库 (NanoVG) 在系统窗口 (Sokol) 中渲染。

---

## 2. 环境与依赖 (Self-contained)
所有依赖必须以源码形式存放在 `./deps` 目录中，按库名分目录组织，由脚本自动化下载。

### 2.1 依赖清单与来源
- **Sokol (窗口与环境)**: `sokol_app.h`, `sokol_gfx.h`, `sokol_glue.h`, `sokol_log.h`
  - 来源: `https://github.com/floooh/sokol`
  - 目录: `deps/sokol/`
- **NanoVG (向量绘图)**: `nanovg.c`, `nanovg.h`, `nanovg_gl.h`, `fontstash.h`, `stb_image.h`, `stb_truetype.h`
  - 来源: `https://github.com/memononen/nanovg`
  - 目录: `deps/nanovg/`
- **microui (UI 布局)**: `microui.c`, `microui.h`
  - 来源: `https://github.com/rxi/microui`
  - 目录: `deps/microui/`
- **nanosvg (SVG 解析)**: `nanosvg.h`
  - 来源: `https://github.com/memononen/nanosvg`
  - 目录: `deps/nanosvg/`
- **log (日志库)**: `log.c`, `log.h`
  - 来源: `https://github.com/rxi/log.c`
  - 目录: `deps/log/`
- **picohttpparser (HTTP 解析器)**: `picohttpparser.c`, `picohttpparser.h`
  - 来源: `https://github.com/h2o/picohttpparser`
  - 目录: `deps/picohttpparser/`
- **mquickjs (轻量脚本引擎)**: 核心解释器 + 两阶段构建工具
  - 核心 (4 个 .c，链接到最终二进制): `mquickjs.c`, `cutils.c`, `dtoa.c`, `libm.c`
  - 构建工具 (2 个 .c，仅在 make 时运行): `mquickjs_build.c`, `mqjs_stdlib.c`
  - 来源: `https://github.com/bellard/mquickjs`
  - 目录: `deps/mquickjs/`
- **Assets**: 开源 `.ttf` 字体（Roboto）存放于 `./assets/`。

---

## 3. 目录结构
```text
/kwcc
├── deps/            # 第三方源码 (由 setup.sh 自动下载)
│   ├── sokol/       #   Sokol 头文件 (单头库)
│   ├── nanovg/      #   NanoVG 核心 + GL 后端
│   ├── nanosvg/     #   nanosvg SVG 解析器 (单头库)
│   ├── microui/     #   microui IMGUI
│   ├── log/         #   rxi/log.c 日志库
│   ├── picohttpparser/ # picohttpparser HTTP 响应解析器
│   └── mquickjs/    #   mquickjs 解释器核心 + 构建工具
├── src/             # 项目 C 源码
│   ├── main.m       # Sokol 窗口生命周期与渲染主循环 (Objective-C)
│   ├── kwcc_base.h  # 纯 C 基础设施（内存池常量 + topic 清洗/校验声明）
│   ├── kwcc_base.c  # topic 清洗 + 校验实现
│   ├── kwcc_mempool.h/c # Slab 内存池：L0-L7 分层池、key_map、常量表、GC、TLV 序列化
│   ├── kwcc_config.h/c  # Config 层：App/Core 域存取接口，自动拼 "a."/"c." 前缀
│   ├── kwcc_ui.c/h  # UI 模块（g_mu、microui 桥接、input、SVG、字体、register_ui）
│   ├── kwcc_js.c/h  # JS lifecycle + ops/module 类型定义 + bus consumer + 代理表 + JS 白名单
│   ├── kwcc_js_http.c/h # HTTP JS bridge plugin（回调注册表 + bus 事件路由）[待实施]
│   ├── kwcc_ui_bus.c/h # UI→JS 事件桥接（topic map + bind_topic + dispatch_event + begin_frame）
│   ├── kwcc_bus.c/h # 通用 C Pub/Sub 事件总线（subscribe/publish/unsubscribe，零 mquickjs 依赖）
│   ├── kwcc_io.c/h  # I/O Reactor（select() 非阻塞 FD 管理器）
│   ├── kwcc_http.c/h # HTTP Process Engine（fork+pipe+curl+picohttpparser+bus dispatch）
│   ├── kwcc.h       # 入口 umbrella header（聚合所有模块头文件）
│   └── llog.h       # 日志包装器 (解决 macOS syslog.h 宏冲突)
├── app/             # 脚本层
│   ├── main.js      # 模块入口（loadJs 加载示例模块）
│   ├── runtime/
│   │   ├── store.js #   全局状态 + 双参数 dispatch + 中间件
│   │   ├── bus.js   #   EventBus（精确匹配 + *末尾通配 + onGroup/offGroup）
│   │   ├── promise.js # MiniPromise（ES5 兼容，通用异步基础设施）[待实施]
│   │   └── http.js  #   $http.fetch（Promise 风格 HTTP 请求）[待实施]
│   └── modules/examples/  # 示例模块集合
│       ├── test/
│       │   ├── test.js      # test 模块（state + actions + events）
│       │   └── test_view.js # test 模块视图
│       ├── calc/
│       │   ├── calc.js      # 计算器模块（state + actions + events）
│       │   └── calc_view.js # 计算器模块视图
│       └── svg/
│           ├── svg.js       # SVG 模块
│           ├── svg_view.js  # SVG 模块视图
│           ├── star.svg     # 蓝色五角星
│           └── test.svg     # 测试图形
├── assets/          # 字体与静态资源（Roboto.ttf 等）
├── .claude/memory/  # 开发经验与记忆文件
├── setup.sh         # 依赖下载脚本
└── Makefile         # 两阶段编译配置 (macOS)
```

---

## 4. 核心架构逻辑

### 4.1 立即模式渲染 (IMGUI)
1. **Sokol `init`**: 初始化 NanoVG、加载字体、启动 mquickjs 虚拟机、加载 `app/main.js`。
2. **Sokol `event`**: 捕获鼠标/键盘事件并映射至 `mu_Context`。
3. **Sokol `frame` (每秒60次)**:
   - 调用 mquickjs 执行 JS 脚本。
   - JS 通过 `ui.button()` 等调用触发 microui 逻辑。
   - 遍历 microui 指令队列 (`mu_next_command`)，调用 NanoVG 函数绘图。
4. **坐标系**: `high_dpi=false`（默认），所有组件统一在逻辑像素坐标系中工作，无需 DPI 转换。

### 4.2 脚本桥接 API
通过 `kwcc_ui()` 全局函数 + JS 包装注入 `ui` 对象:
- `ui.label(text)`
- `ui.button(text, topic)` → 返回布尔值，topic 非空时点击自动 dispatch 事件
- `ui.beginWindow(title, x, y, w, h, opt, topic)` / `ui.endWindow()` — topic 参数用于 X 关闭事件
- `ui.beginPanel(name, opt)` / `ui.endPanel()` — 无标题栏的面板容器
- `ui.sync(key, visible)` — 同步模块状态到 C 层，控制窗口可见性挡板
- `ui.layoutRow(height, w1, w2, ...)` — `-1` 表示充满剩余空间
- `ui.slider(text, value, min, max, topic)` → 返回当前值，变化时自动 dispatch
- `ui.setNext(x, y, w, h)` — 绝对定位下一个控件
- `ui.rect(x, y, w, h, r, g, b)` — 绘制自定义颜色矩形
- `ui.display(text)` — 计算器显示区（深色背景 + 右对齐白色文字）
- `ui.textCentered(text)` — 水平居中文字
- `ui.svg(path_or_svg, x, y, w, h)` — 渲染 SVG。参数以 `<` 开头时作为内联 SVG 字符串解析，否则作为文件路径
- `ui.loadFont(name, path)` — 加载单个字体文件
- `ui.setFont(name)` — 设置当前字体
- `ui.loadFontDir(dir)` — 加载目录下所有字体（自动识别 CJK 字体）

**框架全局函数（$ 前缀为框架内置变量）**:
- `$store` — Store 实例，`$store.dispatch(module, actionName, payload)`
- `$bus` — EventBus 实例，`$bus.on(topic, handler)` / `$bus.emit(topic, action, data)`
- `$topics` — 全局 topic 注册表，`$topics.calc.digit_7`
- `$modules` — 模块注册表
- `registerModule(name, mod)` — 注册模块（state + actions + initEvents）
- `registerModuleView(name, renderFn)` — 注册模块视图渲染函数
- `registerTopic(name, topics)` — 注册模块 topic 常量到 `$topics[name]`
- `loadJs(path, once)` — 加载 JS 文件，`once=1`(默认)只加载一次，`once=0` 强制加载

**mquickjs 内置函数**:
- `load(filename)` — 读取并执行 JS 文件（底层加载函数）
- `print()`, `console.log()` — 日志输出
- `gc()` — 垃圾回收

### 4.3 microui 渲染管线
microui 的 draw hooks 被替换为命令列表 (`mu_Command`)。在 Sokol frame 回调中遍历该列表，将 `MU_COMMAND_RECT` / `MU_COMMAND_TEXT` / `MU_COMMAND_ICON` 转换为 NanoVG 调用。

**SVG 集成**：SVG 通过 `MU_COMMAND_SVG`（type=32，保留 1-31 给官方未来扩展）作为自定义命令插入 microui 队列。`kwcc_queue_svg` 使用 `mu_push_command` 将 SVG 命令放入 microui 命令列表，在 `render_mu_commands()` 遍历中按 zindex 顺序渲染，确保 SVG 跟随所属窗口层级浮动。

### 4.4 JS 模块注册体系与单向数据流

采用 **store-data-flow-v2** 架构：单向数据流 + 模块注册 + topic 事件总线。

**模块注册**：每个模块通过 `registerModule()` 注册 state、actions、事件订阅。视图通过 `registerModuleView()` 注册渲染函数，业务与视图分离。Topic 常量通过 `registerTopic()` 注册到 `$topics`。

**加载顺序**：
```javascript
loadJs("app/modules/examples/calc/calc.js");      // 注册 topic + module
loadJs("app/modules/examples/calc/calc_view.js"); // 注册视图
```

**每帧渲染**：`onFrame()` 遍历所有已注册模块，自动调用 `ui.sync(key, visible)` + `render(state)`。

**事件流**：用户操作 → microui → C 层全局回调 → `kwcc_ui_bus.c` → `$bus.emit(topic, action, data)` → JS handler → `$store.dispatch(module, action, payload)` → state 更新 → 下一帧 `onFrame` 刷新。C bus 事件（如 I/O、HTTP）通过 `kwcc_bus_publish` → JS consumer（白名单过滤 + 模块按前缀分发）→ 对应模块 `on_bus_event` 处理 → 回调 resolve/reject → `$store.dispatch` 更新 state。

**状态持久化**：`$loadedFiles` 记录 JS 文件加载次数，防止重复加载。`registerModule/View/Topic` 内部去重，防止重复注册。

---

## 5. 编译规范 (macOS 10.14+)
- **编译器**: `clang`
- **源语言**: `main.m` (Objective-C)，其余为 C
- **宏定义**:
  - `#define SOKOL_GLCORE` (针对 macOS OpenGL 后端)
  - `#define NANOVG_GL3_IMPLEMENTATION`
- **两阶段构建**:
  1. 编译 host tool (`mquickjs_build.c` + `mqjs_stdlib.c`) → 生成 `mqjs_stdlib.h`
  2. 编译主二进制 (mquickjs.c + cutils.c + dtoa.c + libm.c + main.m + kwcc_mempool.c + kwcc_config.c + kwcc_ui.c + kwcc_js.c + kwcc_js_http.c + kwcc_bus.c + kwcc_ui_bus.c + kwcc_base.c + kwcc_io.c + kwcc_http.c)
- **链接框架**:
  - `-framework Cocoa -framework OpenGL -framework IOKit -framework QuartzCore`
- **静态集成**: 仅将核心 4 个 .c 文件编译进最终的可执行文件。

---

## 6. Claude Code 执行路线图

### 第一步：自动化环境搭建 ✅
编写 `setup.sh` 使用 `curl` 下载所有依赖源码到 `./deps`。创建 `Makefile` 确保能静态编译通过。

### 第二步：窗口与渲染冒烟测试 ✅
在 `main.m` 中使用 Sokol 初始化窗口，并用 NanoVG 在屏幕中心画一个圆。验证 macOS 10.14 下的 OpenGL 链接是否正常。

### 第三步：集成 microui 与 mquickjs ✅
实现 `bridge.c`。在 JS 中写一个简单的循环，点击按钮时控制台能打印 "Hello"。

### 第四步：完整计算器示例 ✅
实现了一个完整的计算器 demo。

### 第五步：库源码分析与记忆系统 ✅
对 microui、sokol、nanovg 三个库进行了源码级分析，经验保存到 `.claude/memory/` 目录。

### 第六步：GitHub 发布流程 ✅
项目已部署到 GitHub，主分支为 `main`。

### 第七步：集成 nanosvg ✅
集成 nanosvg 实现 SVG 文件渲染。

### 第八步：SVG 缓存增强 ✅
128 槽 FNV-1a 哈希缓存 + 帧安全淘汰机制。

### 第九步：store-data-flow-v2 单向数据流架构 ✅
单向数据流 + 模块注册 + topic 事件总线。

### 第十步：提取 EventBus 到 kwcc_bus 独立模块 ✅
C→JS 消息总线桥接独立模块。

### 第十一步：Slab 内存池系统 ✅
Phase 1-7 全部完成：L0-L7 分层池、key_map、常量表、GC、TLV 序列化、Config 层、$config JS API、代理机制、dump 功能。24/24 测试通过。

### 第十二步：Bus 拆分重构 ✅
kwcc_bus 拆分为三层：kwcc_bus（纯 C Pub/Sub）+ kwcc_ui_bus（UI→JS 桥接）+ kwcc_base（topic 清洗/校验）。JS 白名单 + `KWCC_BUS_WILDCARD` 常量 + `kwcc_bus_match_topic`。19/19 测试通过。

### 第十三步：异步 I/O + HTTP Process Engine 🔄
4 层架构：Layer 1（I/O Reactor）+ Layer 2（kwcc_http.c fork+pipe+curl）+ Layer 3（JS 桥接，待实施）+ Layer 4（JS API，待实施）。Layer 1-2 已完成，6/6 C 测试通过。Layer 3-4 因架构调整拆分至 #18。

### 第十四步：kwcc_js Facade + Plugin 架构 ⏳
参考 Linux 内核 core + module 模式：`kwcc_js` 封装 mquickjs 为 Facade（`kwcc_js_ops_t` 属性+函数指针），扩展模块遵循 `kwcc_js_module_t` 规范注册进 core，不直接碰 mquickjs。详见 `requirements/js-bridge-architecture.md`。

---

## 7. 测试体系

- **纯 C 测试**：`tests/test_mempool.c`（46 测试）、`tests/test_config.c`（19 测试）、`tests/test_bus.c`（19 测试）
- **C handler 测试**：`tests/test_config_js.c`（16 测试）
- **JS 集成测试**：`tests/test_config_js.js`（14 测试）
- **测试记录**：`tests/TESTING_MEMPOOL.md` — 24 个测试点全部通过

## 8. 开发规范（强制规则）

1. **出现错误 → 分析根因 → 出方案 → 确定范围 → 等确认 → 再实施**
2. **以方案为主，不因问题打乱计划**
3. **先出方案再做事**：不要盲目加几行代码就去编译调试
4. **非必要不动 C 层**：C 层改动优先评估影响范围
5. **JS 代码遵守 mquickjs ES5 语法**：无 let/const/箭头函数，`{}` 语句开头陷阱
6. **所有 topic 通过 `registerTopic` 注册**：禁止手写硬编码 topic 字符串
