# MEMORY.md — kwcc 项目记忆索引

> 详细文档在 `.claude/memory/` 目录下，按需读取。

## 项目概况
- **kwcc**: 极轻量级自包含 UI 引擎，macOS 10.14+
- 架构: mquickjs -> microui -> NanoVG -> Sokol
- 编译: `clang`, `make`, Objective-C 扩展名 `.m`

## ⚠️ 强制规则（每次会话必读）

> 详细规则见 [workflow_rules.md](workflow_rules.md)

1. **不确定的知识先查证再输出**，不要想当然
2. **命名规范写的时候就遵守**，包括讨论阶段
3. **review 只输出问题**，不等同于执行修改
4. **git 提交前必须得到用户确认**，用户说"提交"才 commit，说"推送"才 push
5. **简单方案优于"更优雅"的方案**
6. **以方案为主**，不因问题打乱计划
7. **⚠️ 遇到问题立刻问用户，绝对不能卡着假死** — 这是行为红线，违反即是严重事故
8. **⚠️ 严格按方案实现，绝不随意发挥** — 方案外的设计决策必须先讨论确认
9. **⚠️ 代码回退用 revert commit，禁止 reset --hard + force push** — 保留完整历史，不覆盖远程

## 记忆文件索引

### [mquickjs C API 参考](mquickjs_c_api.md)
- **JSValue 类型系统**：uint64_t 值类型，无引用计数
- **创建 API**：JS_NewObject / JS_NewArray / JS_NewStringLen / JS_NewInt32 / JS_NewBool
- **属性访问**：JS_GetPropertyStr / JS_SetPropertyStr / JS_GetPropertyUint32
- **C 字符串转换**：JS_ToCString（⚠️ 可能返回 NULL，必须检查）
- **GC 管理**：JS_PUSH_VALUE/JS_POP_VALUE（栈式），JS_AddGCRef/JS_DeleteGCRef（列表式）
- **JS 函数调用**：JS_PushArg + JS_Call(ctx, flags)（2 参数版本！）
- **C→JS Dispatch**：C API 构建对象 + 全局变量传递（禁止拼接用户数据到 JS_Eval）
- **不存在的 API**：JS_FreeValue、JS_Duplicate、JS_Call(ctx, func, this, argc, argv)

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

### [测试方法论](testing_methodology.md)
- **C 端优先测试**：C 端功能在 C 端验证，JS 端不方便
- 四层测试策略：纯 C 测试 → C handler 测试 → ops 接口测试 → JS 集成测试
- 排查问题流程：先 C 层 → 再 C handler → 再 ops → 最后 JS wrapper
- 编译技巧：不链接 kwcc_ui.o，避免 nanosvg/NVG 依赖
- Phase 4 TLV 调试教训：不要在 JS 层反复调试
- ops 测试 75 点：值创建/属性操作/函数调用/类型判断/C字符串/eval/notify/数组

### [模块开发经验](module_dev_experience.md)
- 内存池开发 8 条教训：方案确认、C 端测试、命名规范、不发挥、构建产物、分层、代理机制、测试记录
- Bus 重构教训：static 函数命名规范、命名写时就遵守、sanitize bug、不引入"看似更优"的复杂方案
- Facade+Plugin 教训：`kwcc_js_val_t=JSValue`（不是 uint64_t）、隔离是行为层面、bug 误判别怀疑设计、方案外决策必须先讨论

### [C 语言模块开发模式](c_module_patterns.md)
- **模式 A**：模块前缀 + 静态全局 — 单例、零开销、文件级内聚
- **模式 B**：struct + 函数指针（laid 模式）— 隔离/多实例/可替换、类型级内聚
- 选择决策：需要隔离/多实例/可替换 → B，否则 → A
- 项目中的实际应用对照表

### [⚠️ 工作流规则（强制遵守）](workflow_rules.md)

## 开发规则（来自 CLAUDE.md）

> 详细规则见 [workflow_rules.md](workflow_rules.md)

- 出现错误 → 分析根因 → 出方案 → 确定范围 → 等确认 → 再实施
- 以方案为主，不因问题打乱计划
- 不要盲目加几行代码就去编译调试

## 快速命令
```bash
make        # 构建
make run    # 运行
./setup.sh  # 下载依赖
rm -f *.o kwcc  # 清理构建产物
```
