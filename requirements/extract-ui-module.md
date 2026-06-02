# 提取 UI 模块 + JS 命名统一

> 目标：将 UI 代码从 `kwcc.c` 拆分到 `kwcc_ui.c`，统一文件命名规范，为模块化架构打基础。

---

## 背景

当前 `kwcc.c`（660+ 行）混了三块功能：
1. microui/UI 桥接（`js_ui_dispatch`、svg cache、topic map、窗口挡板、input 事件）
2. config 系统（固定数组存储，`entries[16]` 有数量上限）
3. JS 生命周期（`kwcc_create_js()` 同时创建 JSContext + 注册 UI）

**问题**：
- `kwcc_create_js()` 强依赖 `g_mu`（microui）初始化，导致 JSContext 无法提前创建
- config 存 C 字符串 + 固定数组，JS Object 的动态特性被浪费
- UI 代码无法独立编译，未来加 TUI/无 UI 模式需要重写整个文件

---

## 目标架构

```
核心层（必须）：JSContext + config 存储（kwcc.c/h + kwcc_base.h）
  ├─ 模块：UI   → kwcc_ui.c/h（microui + nanovg + sokol）→ GUI
  ├─ 模块：HTTP → kwcc_http.c/h（curl + picohttpparser）
  └─ 模块：IO   → kwcc_io.c/h（select reactor）
```

**初始化顺序变为：**
```c
kwcc_create_js();       // 1. 创建 JSContext（不依赖 microui）
kwcc_config("io", ...); // 2. 设置各模块配置
kwcc_config("ui", ...);
kwcc_init();            // 3. microui 初始化
kwcc_register_ui();     // 4. UI 方法注册（依赖 g_mu）
kwcc_io_init();         // 5. IO 模块初始化（读 config）
```

---

## 改动清单

### 1. 拆分 `kwcc_create_js()` — `src/kwcc.c`

**当前**：一个函数同时做 JSContext 创建 + ui 对象注册。

**拆为**：
```c
JSContext *kwcc_create_js(void);   // 只创建 JSContext + 内置函数
void         kwcc_register_ui(JSContext *ctx);  // UI 对象 + methods_js
```

**影响**：`main.m` 的 `init()` 需调整调用顺序。

### 2. 提取 UI 模块 — `src/kwcc_ui.c`（新建）、`src/kwcc_ui.h`（新建）

**迁移内容**（从 `kwcc.c` 移到 `kwcc_ui.c`）：
- `js_ui_dispatch()` — UI 方法调度
- `methods_js` 字符串 — JS wrapper 定义
- SVG cache：`g_svg_cache`、`g_svg_cache_next`、`g_frame_counter`、`fnv1a()`、`svg_resolve()`、`kwcc_queue_svg()`
- topic map：`g_topic_map`、`kwcc_bind_topic()`、`kwcc_dispatch_event()`
- 窗口挡板：`g_sync_table`、`g_win_intercepted`、`g_win_topics`、`g_win_top`、`g_current_mod_key`、`kwcc_sync_module()`、`kwcc_get_current_visibility()`、`kwcc_on_window_close()`
- 字体系统：`g_current_font`、`kwcc_load_font_dir()`、`is_cjk_hint()`
- input 事件：`kwcc_input_mousemove()`、`mousedown()`、`mouseup()`、`scroll()`、`text()`
- microui 回调：`mu_text_width()`、`mu_text_height()`

**对外暴露**：
```c
void kwcc_register_ui(JSContext *ctx);
void kwcc_ui_init(void);  /* microui text 回调设置 */
```

### 3. config 存储改为 JSValue — `src/kwcc.c`

**当前**：固定 C 字符串数组，每个模块最多 16 个 key-value。

**改为**：存 JSValue，动态读取。
```c
typedef struct {
    char     module[KWCC_CONFIG_MAX_KEY_LEN];
    JSValue  options;       /* JS Object，直接存储 */
    JSGCRef  options_ref;   /* GC 保护，永久不过期 */
    int      in_use;
} kwcc_config_module_t;

kwcc_config_set("http", options_jsvalue);  // 存整个 Object
kwcc_config_get("http", "bin_path");       // 内部 JS_GetPropertyStr
```

**关键设计**：
- `JS_AddGCRef` 永久保护 config JSValue，**运行期间不过期**
- 整个程序生命周期有效，退出时 `JS_FreeContext` 自动清理
- 不需要手动 `JS_DeleteGCRef` 或 `free`
- 运行时 config 只增不改（或覆盖 value），不存在"过期"概念

**好处**：
- JS Object 有多少 key 存多少，不设上限
- 读的时候 `JS_GetPropertyStr` 直接拿，类型不丢失
- 零运行时内存管理开销
- 需要 `JSContext`，所以 JSContext 必须提前创建

### 4. 更新 `src/kwcc.h`

- 迁移 UI 相关内容到 `kwcc_ui.h`
- 保留 core API + config + SVG cache extern
- include `kwcc_base.h`

### 5. 重命名 `jsapi.h/c` → `kwcc_js.h/c`

统一命名规范，所有 kwcc 相关源文件以 `kwcc_` 开头。

| 旧文件 | 新文件 |
|--------|--------|
| `src/jsapi.h` | `src/kwcc_js.h` |
| `src/jsapi.c` | `src/kwcc_js.c` |

**需同步更新**：
- `src/kwcc.c` 的 `#include "jsapi.h"` → `#include "kwcc_js.h"`
- `src/main.m` 不需要引用此文件（无直接引用）
- `Makefile` 的 `MQJS_SRCS` + build rule
- `deps/mquickjs/mqjs_stdlib.c` 的 `#include "jsapi.h"` → `#include "kwcc_js.h"`
- 所有函数名不变（已符合 `js_*` / `kwcc_*` 命名）

### 6. 更新 `main.m`

```c
/* 旧 */
kwcc_init();
js_ctx = kwcc_create_js();

/* 新 */
js_ctx = kwcc_create_js();     // JSContext 先有
kwcc_io_init();                // IO 初始化
kwcc_register_ui(js_ctx);      // UI 注册
```

---

## 文件依赖关系

```
kwcc_base.h    ← config struct（无框架依赖）
kwcc.h         ← kwcc_base.h + mquickjs + microui（对外 API）
kwcc_ui.h      ← kwcc.h（UI 模块声明）
kwcc_io.h      ← kwcc_base.h（纯 POSIX + config）
kwcc_js.h      ← mquickjs（JS binding 声明）
```

| 源文件 | 依赖头文件 |
|--------|-----------|
| `kwcc.c` | kwcc.h + kwcc_base.h + kwcc_js.h |
| `kwcc_ui.c` | kwcc_ui.h + kwcc.h |
| `kwcc_io.c` | kwcc_io.h + kwcc_base.h |
| `kwcc_js.c` | kwcc_js.h + kwcc.h |

---

## 验证

每个改动完成后 `make` 验证编译通过。
