# 编译坑与已解决问题

## macOS 10.14 编译注意事项

### Sokol 相关
- **文件扩展名**: `main.c` -> `main.m` (Sokol 需要 Objective-C 编译环境)
- **宏名**: `SOKOL_GLCORE` (不是旧的 `SOKOL_GLCORE33`)
- **字段名**: `.swapchain` (不是 `.swap_chain`)
- **帧率限制**: `maximumFramesPerSecond` 需要 macOS 10.15+ 检查，需用 `#if MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_15` 保护
- **sokol_glcore.h**: 不存在，不需要下载。`SOKOL_GLCORE` 宏控制 GL 后端

### mquickjs 两阶段构建
- **阶段 1**: 编译 `mquickjs_build.c` + `mqjs_stdlib.c` 为 host tool `_build_stdlib`
  - 运行 `_build_stdlib` 生成 `mqjs_stdlib.h`（原子表和 stdlib 定义）
  - 运行 `_build_stdlib -a` 生成 `mquickjs_atom.h`
  - **不参与** 最终二进制链接
- **阶段 2**: 只编译 4 个核心 .c 文件链接到最终二进制
  - `mquickjs.c`, `cutils.c`, `dtoa.c`, `libm.c`
- **缺失文件**: `softfp_template.h` 和 `softfp_template_icvt.h` 必须下载到 `deps/mquickjs/`

### mquickjs stdlib stub 函数
`mqjs_stdlib.h` 引用了以下函数，需自行实现：
```
js_print, js_gc, js_load, js_setTimeout, js_clearTimeout
js_date_now, js_performance_now
```
见 `src/mquickjs_stubs.c`

### 日志库 syslog.h 冲突
rxi/log.c 使用 `LOG_INFO`, `LOG_DEBUG` 等常量，但 macOS 的 `syslog.h` 会定义同名宏。解决方案：
- `src/llog.h` — 先 `#undef` 所有 syslog 宏，再 include `log/log.h`
- 对外 API: `log_trace()`, `log_debug()`, `log_info()`, `log_warn()`, `log_error()`, `log_fatal()`
- 内部用 `_L_TRACE` 等枚举值传递给 `log_log()`

### microui 2.02 回调要求
- `mu_Context` 必须设置 `text_width` 和 `text_height` 回调函数
- 否则会 assertion failure
- **必须**用 `nvgTextBounds` 测量真实字体宽度，不能用 `len * 7` 硬编码
- 详见 `ui_design_patterns.md`「文字测量与对齐」

## 依赖下载清单 (setup.sh)
| 库 | 文件 | 下载方式 |
|----|------|----------|
| sokol | 4 个 .h 头文件 | raw GitHub 单个文件 |
| nanovg | nanovg.c/.h, nanovg_gl.h, fontstash.h, stb_*.h | raw GitHub 单个文件 |
| microui | microui.c, microui.h | raw GitHub 单个文件 |
| mquickjs | .tar.gz 解压，挑选需要的文件 | GitHub archive |
| log | log.c, log.h | GitHub repo |

## 已解决的调试问题

1. **sokol_glcore.h 404** — 该文件不存在，不需要
2. **SOKOL_GLCORE33 未定义** — 改为 `SOKOL_GLCORE`
3. **main.c 编译失败** — 改为 `main.m` (Objective-C)
4. **`.swap_chain` 字段不存在** — 改为 `.swapchain`
5. **`maximumFramesPerSecond` 编译错误** — 加 macOS 版本检查
6. **microui assertion 崩溃** — 添加 `text_width`/`text_height` 回调
7. **`mquickjs_atom.h` not found** — Makefile 依赖顺序问题
8. **`softfp_template.h` not found** — 加入 setup.sh 下载清单
9. **undefined reference to `js_print` 等** — 创建 `mquickjs_stubs.c`
10. **LOG_INFO 宏冲突** — 创建 `llog.h` wrapper
11. **CONFIG_KWCC 函数注册机制** — 见下方「mquickjs C 函数注册」专节
12. **JS_ToCString 返回 NULL 导致 sprintf 崩溃** — 见下方
13. **mu_slider_ex 格式字符串参数错误** — 见下方
14. **js_print stub 只处理字符串类型** — 见下方

## 调试方法论

### 使用 lldb 定位崩溃（推荐）
不要用打印日志来定位段错误，太慢。用 lldb：
```bash
lldb -- ./kwcc
(lldb) run          # 运行直到崩溃
(lldb) bt 30        # 完整调用栈，直接看到崩溃的函数链
(lldb) quit
```
一次 `bt` 就能拿到从 Sokol frame() → bridge_process_js() → JS_Eval() → 崩溃函数的完整调用链。比加几十行 printf 高效得多。

### lldb 脚本自动化
创建脚本文件 `/tmp/lldb_script.txt`：
```
settings set target.process.thread.step-avoid-regexp ""
run
bt 30
quit
```
然后：`lldb -s /tmp/lldb_script.txt -- ./kwcc`

## mquickjs C 函数注册

### CONFIG_KWCC 注册方案（已验证通过）
- **只需**在 `deps/mquickjs/mqjs_stdlib.c` 的 `js_global_object[]` 中加一行：
  ```c
  #ifdef CONFIG_KWCC
      JS_CFUNC_DEF("kwcc_ui", 3, js_kwcc_ui),
  #endif
  ```
- **不要**在 `js_c_function_decl[]` 中重复注册 — 会产生 `.0`、`JS_CFUNC_0` 等编译错误
- `Makefile` 的 `HOST_CFLAGS` 需要加 `-DCONFIG_KWCC`
- 函数实现在 `src/mquickjs_stubs.c`，链接时解析符号
- 详见 `requirements/mquickjs-cfunc-registration.md`

### MVP 独立测试
创建了 `tests/mvp/` 目录，独立验证 C 函数注册机制：
- 不依赖 Sokol/microui/nanovg
- 仅用 mquickjs core + stubs + log
- 验证了完整的 JS → C → JS 调用链路

## 常见崩溃与修复

### JS_ToCString 返回 NULL
`JS_ToCString(ctx, JS_UNDEFINED, &buf)` 返回 **NULL**。如果直接把 NULL 传给 microui 函数（如 `mu_begin_window_ex`、`mu_button`），microui 内部的 `sprintf` 会崩溃。

**修复**：所有 `JS_ToCString` 结果加 NULL 保护：
```c
const char *text = JS_ToCString(ctx, argv[0], &buf);
mu_button_ex(&g_mu, text ? text : "", 0, 0);  // NULL → 空字符串
```

### mu_slider_ex 格式字符串参数
`mu_slider_ex(ctx, &val, low, high, step, fmt, opt)` 的第 6 个参数是 **格式字符串**，不是标签文字。内部用 `sprintf(buf, fmt, v)`。

**错误**：把标签文字 `"Volume"` 当 fmt 传入 → `sprintf(buf, "Volume", v)` 崩溃
**正确**：`mu_slider_ex(&g_mu, &val, low, high, 0.01f, "%g", MU_OPT_ALIGNCENTER)`

### js_print 只处理字符串
`js_print` stub 原来只处理 `JS_IsString()` 的参数，数字等非字符串被静默转为空字符串。

**修复**：移除 `JS_IsString` 检查，直接 `JS_ToCString`（mquickjs 会对非字符串值自动转字符串）：
```c
const char *s = JS_ToCString(ctx, argv[i], &cbuf);
// s 对数字等也会返回字符串形式，如 "42"、"0.5"
```

## 当前未解决的问题

（无 —— 2026-11-04）

**已解决（2026-11-04）：窗口 X 关闭事件**
- 原 bug：`on_window_close` 收到 title 而 JS 订阅 topic，dispatch 不达
- 修复后暴露第二层潜伏缺陷：可见性挡板只拦 begin/endWindow，body widget 落 microui 空栈 → expect abort（用户实机 microui.c:261 实锤）
- 修复：title→topic map + widget 级挡板补全（`kwcc_ui_in_intercepted_window`，11 个 handler 入口吞调用）
- 详细案例：`module_dev_experience.md`「UI 窗口挡板 / X 关闭事故」；测试：`tests/TESTING_WINDOW_CLOSE.md`（L1 7/7 + L2 用户验收通过）

（之前记录的 JS_Eval 崩溃已修复，kwcc 二进制可以正常运行）

## 开发方法论教训

### 先出方案再做事（强制规则）

不要盲目加几行代码就去编译调试。每次改动前：
1. 分析根因（读源码、lldb、日志）
2. 给出完整方案
3. 用户确认后再实施

### 以方案为主，不要因问题打乱计划（强制规则）

- 开发计划必须围绕已确认的方案（如 store-data-flow-v2）进行
- 中间遇到的问题不能打乱原定的方案计划
- 修改不能"说一句立马就去变更"，应该先准备修改方案，评估之后再去做
- 遇到的问题如果是方案外的问题，先讨论方案，确认后再实施
- 不能为了修某个问题就去破坏原来定好的架构设计

### 优先使用 lldb 而非 printf 调试

lldb 是 macOS 原生调试工具，比加日志打断点高效得多：
```bash
lldb --batch \
  -o 'breakpoint set --name func_name' \
  -o 'run' \
  -o 'thread step-inst -c 8' \
  -o 'frame variable' \
  -o 'quit' -- ./kwcc
```

对于无 debug symbols 的二进制，用 `register read` 直接检查 x86_64 寄存器：
- `rdi` = 第一个指针参数
- `rsi`, `rdx`, `rcx` = 后续整型参数
- `xmm0`-`xmm3` = float 参数
- `thread step-inst` 跳过 prologue 后可检查栈帧

### 程序异常终止的排查要点（2026-11-04 X 关闭事故沉淀）

1. **abort/断言输出走 stderr，不进 kwcc.log**——microui 的 `expect` = fprintf(stderr)+abort()。
   日志文件的终止点不是死亡点。排查"程序莫名退出"时，**第一件事问用户要终端完整输出**，
   `Fatal error: <file>:<line>: assertion '...' failed` 那行就是死亡点坐标
2. **"死代码激活"模式**：修复 A 后出现"新故障"，先怀疑 B 是被激活的既有潜伏缺陷，
   不要先怀疑修复本身或急着回退。判断依据：新故障的执行路径在修复前是否从未被走到过
3. **因果链分析法（事实已在手时，优先于构造复现）**：从已确立的事实点（如"dispatch 已成功"）
   沿代码路径逐环走查到崩溃点，每一环要代码行号实锤；无实锤的环节标注为推断，
   等实证（终端输出/lldb bt）修正。完整案例见 module_dev_experience.md「UI 窗口挡板」节
4. **无断言保护的栈访问静默越界**：实际 abort 点 ≠ 最早出错的调用点
   （本案 layoutRow 的 get_layout 先静默越界，label 才引爆 261 行断言）

### C 函数调用 ABI 陷阱

**现象**：函数调用接收正确参数名但值为 0。

**根因**：调用方没有 include 函数声明头文件。C 编译器在 x86_64 下对无原型的函数调用，float 参数会被提升为 double（通过 XMM 寄存器传递），但函数定义期望 float。**修复**：始终 include 正确的头文件。
