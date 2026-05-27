# Spec: kwcc - 极轻量级自包含 UI 引擎

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
- **mquickjs (轻量脚本引擎)**: 核心解释器 + 两阶段构建工具
  - 核心 (4 个 .c，链接到最终二进制): `mquickjs.c`, `cutils.c`, `dtoa.c`, `libm.c`
  - 构建工具 (2 个 .c，仅在 make 时运行): `mquickjs_build.c`, `mqjs_stdlib.c`
  - 来源: `https://github.com/bellard/mquickjs`
  - 目录: `deps/mquickjs/`
- **Assets**: 需下载一个开源 `.ttf` 字体（如 Roboto）至 `./assets/`。

---

## 3. 目录结构
```text
/kwcc
├── deps/            # 第三方源码 (由 setup.sh 自动下载)
│   ├── sokol/       #   Sokol 头文件 (单头库)
│   ├── nanovg/      #   NanoVG 核心 + GL 后端
│   ├── microui/     #   microui IMGUI
│   └── mquickjs/    #   mquickjs 解释器核心 + 构建工具
├── src/             # 项目 C 源码
│   ├── main.m       # Sokol 窗口生命周期与渲染主循环 (Objective-C)
│   ├── bridge.c     # mquickjs 与 microui 的 API 绑定
│   └── bridge.h     # Bridge 公共接口
├── app/             # 脚本层
│   └── main.js      # UI 解析逻辑与业务代码
├── assets/          # 字体与静态资源
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
4. **Retina 适配**: 使用 `sapp_width()` / `sapp_height()` 获取窗口像素尺寸。

### 4.2 脚本桥接 API
通过 `kwcc_ui()` 全局函数 + JS 包装注入 `ui` 对象:
- `ui.label(text)`
- `ui.button(text)` → 返回布尔值表示是否被点击
- `ui.beginWindow(title, x, y, w, h)` / `ui.endWindow()`
- `ui.layoutRow(height)`
- `ui.slider(text, value, min, max)` → 返回当前值

### 4.3 microui 渲染管线
microui 的 draw hooks 被替换为命令列表 (`mu_Command`)。在 Sokol frame 回调中遍历该列表，将 `MU_COMMAND_RECT` / `MU_COMMAND_TEXT` 转换为 NanoVG 调用。

---

## 5. 编译规范 (macOS 10.14+)
- **编译器**: `clang`
- **源语言**: `main.m` (Objective-C)，其余为 C
- **宏定义**:
  - `#define SOKOL_GLCORE` (针对 macOS OpenGL 后端)
  - `#define NANOVG_GL3_IMPLEMENTATION`
- **两阶段构建**:
  1. 编译 host tool (`mquickjs_build.c` + `mqjs_stdlib.c`) → 生成 `mqjs_stdlib.h`
  2. 编译主二进制 (mquickjs.c + cutils.c + dtoa.c + libm.c + bridge.c + main.m)
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

### 第四步：极简标签渲染
在 `main.js` 中实现一个解析器，能解析 `<button text="提交" />` 字符串并映射到 `ui.button` 调用。
