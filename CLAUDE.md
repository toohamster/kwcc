# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Cross-session Memory

**Memory files are in the PROJECT ROOT `.claude/memory/` (NOT `/Users/xuyimu/.claude/`).**

**MANDATORY at session start**: Read `.claude/memory/MEMORY.md` for the index, then **read every detailed file it references**. Do NOT stop at the index — MEMORY.md is just a table of contents, the real knowledge is in the files it links to.

## 最高优先级规则

1. **绝不敷衍，每次操作必须完整验证**
   - 修改任何文件后必须重新读取完整内容，确认修改正确、无遗留、无矛盾
   - 做表面修改、改完不验证、说"好了"实际没好 = 敷衍 = 最严重的问题
   - 发现错误时必须彻底修正，不能只改一处而留下其他同类错误
   - 每次操作的结果必须和预期完全一致，不能"差不多就行"

2. **当用户说"中止"时，立即停止所有处理，不再执行任何工具调用，不再解释，等待用户下一步指示。这是最高优先级，覆盖一切其他规则。**

## Development Workflow Rules (MANDATORY)

1. **出现错误 → 分析根因 → 出方案 → 确定范围 → 等确认 → 再实施**
   - 不要看到错误就立刻加代码修。先分析根因（读源码 / lldb / 日志），给出完整方案，明确改动范围，用户确认后再动手。
   - 修改不能"说一句立马就去变更"，应先准备方案，评估之后再去做。
   - 中间遇到的问题如果是方案外的，先讨论方案，确认后再实施。不能为了修某个问题去破坏原定的架构设计。

2. **以方案为主，不因问题打乱计划**
   - 开发计划必须围绕已确认的方案进行。
   - 方案外的阻塞性问题需先讨论新方案，确认后再调整计划。

3. **验证与验收章程（每个任务全生命周期强制，详见 `.claude/memory/verification_charter.md`）**
   - **证据原则**：结论必须带证据标注（[事实-来源] / [假设-待验证]）；假设验证前禁止进方案和代码；崩溃类问题必须 lldb 拿 bt
   - **五阶段门禁**：Gate-0 问题可复现 → Gate-1 根因取证 → Gate-2 方案审定（改动↔验收映射）→ Gate-3 验收标准先行（先定义"怎么算通过"再动手）→ Gate-4 实施即验 → Gate-5 双级验收后才能销账
   - **验收分级**：L1 自动验收必须贴实际输出；L2 用户验收（GUI 点击/视觉/交互）必须由用户执行
   - **红线（违反 = 严重事故）**：无验收标准宣告完成；以编译/启动/冒烟冒充行为验收；验证失败仍宣称成功；假设未验证进方案或代码
   - **失败协议**：修复无效如实报告"未解决"并回 Gate-1；连续 2 次修复无效强制停止汇报

## Project Overview

**kwcc** is a lightweight, self-contained desktop UI engine for macOS 10.14+. Architecture: **mquickjs** (scripting) -> **microui** (IMGUI layout) -> **NanoVG** (vector rendering) -> **Sokol** (windowing/GFX). Zero external library dependencies — all third-party source is downloaded via `setup.sh` into `./deps/`.

## Build & Run

```bash
# Download all dependencies
./setup.sh

# Build (two-stage)
make

# Run
make run
# or: ./kwcc
```

**Two-stage build:**
1. Stage 1 compiles a host tool (`mquickjs_build.c` + `mqjs_stdlib.c`) -> generates `mqjs_stdlib.h` and `mquickjs_atom.h` at build time
2. Stage 2 compiles the main binary using only 4 mquickjs core .c files + project .c files: `mquickjs.c`, `cutils.c`, `dtoa.c`, `libm.c` + `main.m` + `kwcc.c` + `kwcc_ui.c` + `kwcc_js.c` + `kwcc_io.c` + `kwcc_bus.c`

**Key compiler flags:** `clang`, `-Wall -Wextra -fobjc-arc -O0 -D_GNU_SOURCE -fno-math-errno -fno-trapping-math`. macOS requires `main.m` (Objective-C) for Sokol.

**Link frameworks:** `-framework Cocoa -framework OpenGL -framework IOKit -framework QuartzCore`

## Directory Structure

```
/kwcc
├── deps/           # Third-party source (downloaded by setup.sh)
│   ├── sokol/      #   Window & graphics (single-header libs)
│   ├── nanovg/     #   Vector rendering
│   ├── microui/    #   IMGUI layout engine
│   ├── mquickjs/   #   JS engine + build tool (two-stage)
│   ├── log/        #   rxi/log.c logging library
│   └── nanosvg/    #   rxi/nanosvg SVG parser (single-header)
├── src/
│   ├── main.m          # Sokol lifecycle, NanoVG rendering, input routing
│   ├── kwcc_base.h     # Pure C infrastructure (config getter declarations)
│   ├── kwcc.c          # config JSValue storage (no microui)
│   ├── kwcc_ui.c       # UI module: g_mu + kwcc_ui_init + kwcc_process_js + UI bridge + input + SVG + register_ui
│   ├── kwcc_ui.h       # UI module declarations + SVG cache extern
│   ├── kwcc_js.c       # JS lifecycle: kwcc_create/destroy_js + stdlib stubs + kwcc_ui bridge
│   ├── kwcc_js.h       # JS lifecycle + stubs declarations
│   ├── kwcc_bus.c      # C→JS message bus: topic map + dispatch_event + bind_topic
│   ├── kwcc_bus.h      # Message bus declarations (no microui types)
│   ├── kwcc.h          # Umbrella header (includes all module headers)
│   └── llog.h          # Logging wrapper (wraps rxi/log.h, handles syslog.h conflicts)
├── app/
│   ├── main.js         # Module entry (loadJs loads example modules)
│   ├── runtime/
│   │   ├── store.js    #   createStore + dual-param dispatch + middleware
│   │   └── bus.js      #   EventBus (exact match + * wildcard + onGroup/offGroup)
│   └── modules/examples/  # Example modules
│       ├── test/       #   test module (state + actions + events + view)
│       ├── calc/       #   calculator module
│       └── svg/        #   SVG rendering module
├── assets/         # Static resources (Roboto font)
├── setup.sh        # Dependency download script
├── Makefile        # Two-stage build configuration
├── spec.md         # Project specification
└── kwcc.log      # Runtime log file
```

## Architecture

### Render Pipeline (60fps)
1. `frame()` calls `kwcc_process_js()` -> executes `app/main.js` in mquickjs
2. JS code calls `ui.button()`, `ui.label()`, etc. which trigger microui logic
3. `mu_next_command()` iterates the microui command queue
4. Commands (`MU_COMMAND_RECT`, `MU_COMMAND_TEXT`, etc.) are rendered via NanoVG calls

### Script Bridge API
The `ui` object is injected via `kwcc_ui()` global function + JS wrapper in `kwcc_create_js()`:
- `ui.beginWindow(title, x, y, w, h, opt, topic)` / `ui.endWindow()`
- `ui.sync(key, visible)` — sync module state to C layer (visibility barrier)
- `ui.button(text, topic)` → returns bool (clicked), auto-dispatches when topic provided
- `ui.slider(text, value, min, max, topic)` → returns current value, auto-dispatches on change
- `ui.label(text)`
- `ui.layoutRow(height)`
- `ui.svg(path_or_svg, x, y, w, h)` — render SVG from file path or inline string (`data[0] == '<'`)
- `ui.display(text)` — calculator display area (dark bg + right-aligned white text)
- `ui.loadFont(name, path)` / `ui.setFont(name)` / `ui.loadFontDir(dir)` — font management

**Framework globals**: `$store`, `$bus`, `$topics`, `$modules`, `registerModule()`, `registerModuleView()`, `registerTopic()`, `loadJs(path, once)`

### C→JS Message Bus

`kwcc_bus.c/h` provides a C→JS message bus bridge for any module:
- `kwcc_dispatch_event(ctx, topic, action)` — emits `$bus.emit(topic, action, new Object())` to JS
- `kwcc_bind_topic(id, topic)` — registers ID→topic mapping in the per-frame topic map
- `kwcc_bus_begin_frame()` — resets topic map at start of each frame

### Logging
Use `llog.h` — provides `log_trace()`, `log_debug()`, `log_info()`, `log_warn()`, `log_error()`, `log_fatal()`. Wraps rxi/log.h and handles macOS syslog.h macro conflicts (`LOG_INFO` etc.). Logs to both `output.log` and stdout.

## mquickjs ES5 Support

mquickjs is a stripped-down QuickJS with mostly ES5 support. Key findings from source analysis:

**Supported:**
- Variables: `var x = value;` (only `var` creates new variables)
- Functions: `function name() {}`, function expressions, closures
- Control flow: `if/else`, `while`, `do/while`, `for`, `for..in`, `switch`, `try/catch/finally`, `throw`, `return`
- Objects: `{ key: value }` literals (in expression context), `new Object()`
- Arrays: `[]`, `.push()`, `.pop()`, `.join()`, `.forEach()`, `.map()`, `.filter()`, `.reduce()`, etc.
- Strings: Full ES5+ string methods (`slice`, `split`, `replace`, etc.)
- Numbers, booleans, math, JSON, RegExp
- `eval()`, `typeof`, `instanceof`, `delete`
- Strict equality: `===`, `!==`
- Operators: `+`, `-`, `*`, `/`, `%`, `++`, `--`, `+=`, etc.

**NOT Supported (or unconfirmed):**
- `let` / `const` (tokenized but not fully implemented)
- Arrow functions `=>` (no `TOK_ARROW` in tokenizer)
- Template literals / backticks
- Spread operator `...`
- `class` / `extends` (tokenized but not fully implemented)
- `import` / `export`
- `for..of` loops (throws "unsupported type")
- ES6 object literal enhancements (shorthand, computed keys)

**Critical gotcha:** In mquickjs, `{}` at the start of a statement may be parsed as a **block statement** rather than an object literal. Use `(ui = {})` or `var ui = new Object();` instead of `ui = {};` to avoid `SyntaxError: expecting ';'`.

**JS_EVAL flags:** `JS_EVAL_REPL` is used for eval mode (REPL behavior). `JS_EVAL_RETVAL` returns the last expression value.

## Known Issues

（无 — 窗口 X 关闭已于 2026-11-04 修复：title→topic map + 可见性挡板 widget 级补全（方案 A），L1 7/7 + L2 用户验收通过。完整案例与教训见 `.claude/memory/module_dev_experience.md`「UI 窗口挡板 / X 关闭事故」。）

## SVG Caching

SVG rendering uses a 128-slot FNV-1a hash cache with frame-safe eviction. Cache types and externs are exposed via `kwcc.h` (`svg_cache_t`, `g_svg_cache`, `g_svg_cache_next`, `g_frame_counter`). `main.m` reads images directly from the cache during rendering. Inline SVG detection: `data[0] == '<'` → `nsvgParse`, otherwise `nsvgParseFromFile`.

## Input Handling

Mouse events are mapped from Sokol events in `input()`:
- `SAPP_EVENTTYPE_MOUSE_MOVE` -> `kwcc_input_mousemove()`
- `SAPP_EVENTTYPE_MOUSE_DOWN/UP` -> `kwcc_input_mousedown/mouseup()`
- `SAPP_EVENTTYPE_MOUSE_SCROLL` -> `kwcc_input_scroll()`
- `SAPP_EVENTTYPE_CHAR` -> `kwcc_input_text()`
