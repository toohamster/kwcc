# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Cross-session Memory

Before starting work, read `.claude/memory/MEMORY.md` for a summary of key project knowledge.
Detailed documents in `.claude/memory/` cover:
- **mquickjs ES5 syntax support** — see `.claude/memory/mquickjs_es5.md`
- **Build pitfalls & resolved issues** — see `.claude/memory/build_pitfalls.md`

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
2. Stage 2 compiles the main binary using only 4 mquickjs core .c files: `mquickjs.c`, `cutils.c`, `dtoa.c`, `libm.c`

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
│   └── log/        #   rxi/log.c logging library
├── src/
│   ├── main.m          # Sokol lifecycle, NanoVG rendering, input routing
│   ├── kwcc.c          # UI engine core: JS ↔ microui ↔ NanoVG bridge
│   ├── kwcc.h          # kwcc public API
│   ├── jsapi.c         # JS runtime support (stdlib stubs + kwcc_ui)
│   ├── jsapi.h         # Stub function declarations
│   └── llog.h          # Logging wrapper (wraps rxi/log.h, handles syslog.h conflicts)
├── app/
│   ├── main.js         # JS entry point (via load() switches examples)
│   └── examples/       # Example projects
│       └── calculator/
│           ├── main.js      # Calculator UI layout
│           └── calc_logic.js # Calculator business logic
├── assets/         # Static resources (Roboto font)
├── setup.sh        # Dependency download script
├── Makefile        # Two-stage build configuration
├── spec.md         # Project specification
└── output.log      # Runtime log file
```

## Architecture

### Render Pipeline (60fps)
1. `frame()` calls `kwcc_process_js()` -> executes `app/main.js` in mquickjs
2. JS code calls `ui.button()`, `ui.label()`, etc. which trigger microui logic
3. `mu_next_command()` iterates the microui command queue
4. Commands (`MU_COMMAND_RECT`, `MU_COMMAND_TEXT`, etc.) are rendered via NanoVG calls

### Script Bridge API
The `ui` object is injected via `kwcc_ui()` global function + JS wrapper in `kwcc_create_js()`:
- `ui.beginWindow(title, x, y, w, h)` / `ui.endWindow()`
- `ui.button(text)` -> returns bool (clicked)
- `ui.label(text)`
- `ui.slider(text, value, min, max)` -> returns current value
- `ui.layoutRow(height)`

Global functions available in JS: `print()`, `console.log()`, `kwcc_ui()`, `gc()`, `load()`

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

1. **JS not executing** — `kwcc_create_js()` fails to create the `ui` object via `JS_Eval` with `SyntaxError: expecting ';'`. The fix is to use mquickjs-compatible syntax (see mquickjs ES5 Support section above). `app/main.js` then fails with `ReferenceError: variable 'ui' is not defined` as a consequence.

2. **Logging** — `log_add_fp()` is called in `init()` to write to `output.log`. File is opened with `"w"` mode (truncated each run).

## Input Handling

Mouse events are mapped from Sokol events in `input()`:
- `SAPP_EVENTTYPE_MOUSE_MOVE` -> `kwcc_input_mousemove()`
- `SAPP_EVENTTYPE_MOUSE_DOWN/UP` -> `kwcc_input_mousedown/mouseup()`
- `SAPP_EVENTTYPE_MOUSE_SCROLL` -> `kwcc_input_scroll()`
- `SAPP_EVENTTYPE_CHAR` -> `kwcc_input_text()`
