# MEMORY.md — kwcc 项目记忆索引

> 详细文档在 `.claude/memory/` 目录下，按需读取。

## 项目概况
- **kwcc**: 极轻量级自包含 UI 引擎，macOS 10.14+
- 架构: mquickjs -> microui -> NanoVG -> Sokol
- 编译: `clang`, `make`, Objective-C 扩展名 `.m`

## 记忆文件索引

### [mquickjs ES5 语法支持](mquickjs_es5.md)
- 从测试用例 (`tests/*.js`) 和源码分析验证的支持/不支持语法清单
- **关键陷阱**: `{}` 在语句开头被解析为 block 而非 object literal
  - 修复: `var ui = {};` 代替 `ui = {};`
- 可用的全局函数、内置对象、闭包、eval、TypedArray 等

### [编译坑与已解决问题](build_pitfalls.md)
- macOS 10.14 编译注意事项（`.m` 扩展名、sokol 宏名等）
- mquickjs 两阶段构建理解
- **调试方法论**: 使用 lldb 定位崩溃（不要用 printf）
- mquickjs C 函数注册方案（CONFIG_KWCC，已验证通过）
- 常见崩溃与修复（JS_ToCString NULL、mu_slider_ex fmt 参数等）
- MVP 独立测试目录 `tests/mvp/`

### [Microui 核心机制](microui_analysis.md)
- 布局系统：`mu_layout_row` 的 items/widths/-1 含义
- Widget ID 生成（fnv-1a 哈希，指针地址作为 ID 的原因）
- 立即模式状态持久化机制
- **slider 值指针问题**：局部变量地址导致跨帧状态丢失
- 窗口 body 计算、文字对齐、默认样式值

### [Sokol DPI 行为](sokol_analysis.md)
- `high_dpi=true` vs `false` 的完整对比表
- `sapp_width/height`、`sapp_dpi_scale`、鼠标坐标在各模式下的值
- 推荐的坐标系统一方案

### [NanoVG 坐标空间模型](nanovg_analysis.md)
- `nvgBeginFrame` 的 width/height/DPR 参数各自的作用
- DPR 不影响坐标转换，只控制渲染质量
- HiDPI 渲染的两种等价方案

### [nanosvg 源码分析](nanosvg_analysis.md)
- 核心数据结构：NSVGimage → NSVGshape → NSVGpath 链表
- 解析 API：nsvgParseFromFile / nsvgDelete
- 颜色格式：R|(G<<8)|(B<<16) 无 alpha
- 路径点格式：cubic bezier 6 floats per segment
- **SVG 渲染架构**：作为 MU_COMMAND_SVG (type=32) 插入 microui 命令队列
  - 用 `mu_push_command` 而非独立队列，确保 zindex 排序正确
  - SVG 命令跟随所属窗口的 zindex，在 microui 命令流中按序渲染
  - 屏幕坐标 = clip_rect.x + 布局相对偏移
  - nanosvg 实现 (`NANOSVG_IMPLEMENTATION`) 只在 main.m 中定义一次

### [C 层完整架构](c_architecture.md)
- 全局变量、核心函数、ui.* API 完整清单
- 事件系统：kwcc_dispatch_event、kwcc_bind_topic、topic map
- 窗口挡板：kwcc_sync_module、可见性拦截栈
- SVG 缓存：fnv1a、svg_resolve、帧安全淘汰
- 字体系统：loadFontDir 自动选 CJK 字体
- JS wrapper（methods_js）定义
- 已实现/未实现控件对照表

### [UI 设计与实现模式](ui_design_patterns.md)
- 布局系统：layoutRow 参数规则、-1 的含义、状态切换
- 文字测量与对齐：nvgTextBounds 替代硬编码
- 坐标系与 DPI：high_dpi=false 是简单方案
- 状态持久化：Slider 值指针、JS 层状态保护
- 图标渲染：MU_COMMAND_ICON 的正确画法
- Bridge API 开发模式：**ui.* 方法完整清单** + 参数提取模板
- NanoVG 渲染技巧：字体、矩形、描边、裁剪
- 调试检查清单
- JS 文件模块化：load() 函数、模块状态持久化

### [picohttpparser 分析](picohttpparser_analysis.md)
- HTTP 响应解析 API：`phr_parse_response()` 返回值语义（>0 成功/-2 不完整/-1 错误）
- `phr_header` 零拷贝指针结构，需 `JS_NewStringLen` 提取
- 增量解析机制：`last_len` 参数用于 incomplete 检测
- 与 mquickjs 集成：`JS_NewStringLen` 创建 JS 字符串，`JS_NewInt32` 创建状态码
- chunked 解码器：`phr_decode_chunked`（自研 socket 实现时需要）

### [GitHub 发布流程](deploy_workflow.md)
- GitHub Token 提取方法
- 功能分支 → main 的完整发布流程（PR、合并、打标签、Release）
- 版本号自动递增规则（vX.Y.Z）
- 保留远程分支、留在当前分支等注意事项

## 快速命令
```bash
make        # 构建
make run    # 运行
./setup.sh  # 下载依赖
rm -f *.o kwcc  # 清理构建产物
```
