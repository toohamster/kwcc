# TESTING_WINDOW_CLOSE — 窗口 X 关闭 + 可见性挡板补全

> 测试类型：C 端复现测试（UI 层，链接 kwcc_ui + microui + nanovg，无 Sokol）
> 测试文件：`tests/test_window_close.c`
> 运行方式：
> ```bash
> # 正常跑
> ./tests/bin/test_window_close
> # lldb 下跑（崩溃取 bt）
> lldb -s /tmp/lldb_script_close.txt -- ./tests/bin/test_window_close
> ```
> 编译命令（模板见 testing_methodology.md，UI 层需链接 kwcc_ui.o + microui + nanovg）：
> ```bash
> clang -Wall -Wextra -I. -Isrc -Ideps -O0 -D_GNU_SOURCE -DCONFIG_KWCC \
>   -o tests/bin/test_window_close tests/test_window_close.c \
>   build/obj/src/kwcc.o build/obj/src/kwcc_js.o build/obj/src/kwcc_js_http.o \
>   build/obj/src/kwcc_ui.o build/obj/src/kwcc_io.o build/obj/src/kwcc_bus.o \
>   build/obj/src/kwcc_mempool.o build/obj/src/kwcc_config.o build/obj/src/kwcc_ui_bus.o \
>   build/obj/src/kwcc_base.o build/obj/src/kwcc_http.o \
>   build/obj/deps/mquickjs/mquickjs.o build/obj/deps/mquickjs/cutils.o \
>   build/obj/deps/mquickjs/dtoa.o build/obj/deps/mquickjs/libm.o \
>   build/obj/deps/nanovg/nanovg.o build/obj/deps/microui/microui.o \
>   build/obj/deps/log/log.o build/obj/deps/picohttpparser/picohttpparser.o \
>   -framework Cocoa -framework OpenGL -framework IOKit -framework QuartzCore
> ```

## 背景

窗口挡板（sync visible → C 层拦截 beginWindow）原实现只拦截 begin/endWindow 一对，
body 中的 widget 落到 microui 空 container/clip/layout 栈 → `mu_get_clip_rect` 的
expect 断言 → abort（用户实机 microui.c:261）。修复 = 方案 A：widget 级拦截补全
（`kwcc_ui_in_intercepted_window()` 检查，11 个 handler 入口吞调用）。

## 测试点

| # | 测试点 | 类型 | 状态 |
|---|--------|------|------|
| 1 | `init_create_js` — 生产 init 序列（mempool→io→http→create_js→ui）完整执行 | 纯 C | ✅ 通过 |
| 2 | `setup_eval_no_exception` — 复刻 test.js/test_view.js 的 setup JS 无异常 | C handler | ✅ 通过 |
| 3 | `frame1_window_renders` — 窗口正常渲染（21 条命令） | C handler | ✅ 通过 |
| 4 | `close_handler_fired_once` — 模拟点击 X（3 帧输入序列）→ close 回调恰好 1 次，dispatch 到达 topic | C handler | ✅ 通过 |
| 5 | `visible_set_zero_after_close` — JS handler 链路（closeWindow action）生效 visible=0 | C handler | ✅ 通过 |
| 6 | `survived_intercept_frames` — 拦截路径 60 帧存活（修复前此处 abort） | C handler | ✅ 通过 |
| 7 | `engine_eval_alive_after_frames` — 60 帧后 JS 引擎仍可用 | C handler | ✅ 通过 |

**L1 结果（2026-11-04，修复后）**：7 passed, 0 failed，exit 0，lldb 无 signal。
拦截帧命令产出 = 0（挡板吞掉全部 widget 的直接证据）。

## 挡板覆盖清单（方案 A 改动点 ↔ 验收项）

| handler | 拦截行为 | 验收 |
|---------|---------|------|
| beginWindow（嵌套） | 外层被拦 → 内层同步拦截 | 测试点 6（间接，测试视图无嵌套） |
| endWindow | 原有检查（不变） | 测试点 6 |
| beginPanel / endPanel | 吞 | ⏳ 待嵌套面板场景覆盖 |
| button | 返回 false | 测试点 6 |
| label / textCentered / display / rect / svg / setNext / layoutRow | 吞 | 测试点 6（label/layoutRow 直接触发过 abort，回归保护） |
| slider | 返回传入值 | ⏳ 待 slider 场景覆盖 |
| loadFont / setFont / loadFontDir | 不拦（全局资源，非窗口域） | — |

## L2 用户验收清单（Gate-5，只有用户能执行）

1. `make run` → 点击 Test Counter 标题栏 X → **窗口消失，程序继续运行**
2. 关闭后继续点 + / Reset → 功能正常
3. 终端无 `Fatal error` 行；kwcc.log 有 `on_window_close: 'Test Counter' -> topic 'test/window'`
4. （回归）calc / svg 等其他模块窗口正常

## 已知未覆盖

- 嵌套窗口 / 面板场景（挡板逻辑已处理，无自动化测试点）
- slider 拦截返回值契约（无自动化测试点）
- 触发过崩溃的原生路径（microui.c:261 expect）无独立回归测试——由测试点 6 间接覆盖
