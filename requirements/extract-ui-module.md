# 提取 UI 模块 + config JSValue 存储

> 目标：`kwcc.c` 不含 microui，只做 config JSValue 存储。UI 归 `kwcc_ui.c`，JS 归 `kwcc_js.c`。

---

## 背景

当前 `kwcc.c` 混了 microui（`g_mu` + `mu_init`）和 config 存储，文件拆分后仍依赖 microui，毫无意义。

---

## 目标架构：文件职责彻底分离

| 文件 | 职责 | 不含 |
|------|------|------|
| `kwcc_base.h` | config getter 声明（纯 C 类型） | 无 JS/microui 类型 |
| `kwcc.c` | config JSValue 存储（内部实现） | 无 microui |
| `kwcc_ui.c` | `mu_Context g_mu` + `kwcc_init()` + `kwcc_process_js()` + UI 桥接 + input + svg + register_ui | 无 JS lifecycle |
| `kwcc_ui.h` | UI 模块声明 + SVG cache extern + `g_mu` extern | |
| `kwcc_js.c` | `kwcc_create_js()` + `kwcc_destroy_js()` + JS stubs + `kwcc_set_ui_callback` + `js_kwcc_ui` | 无 microui |
| `kwcc_js.h` | JS lifecycle + stubs + UI bridge 声明 | 无 microui |
| `kwcc.h` | 公共 API：`kwcc_init` / `kwcc_free` / `kwcc_process_js` / `kwcc_get_mu` | 无 JS 类型 |
| `kwcc_io.c` | I/O reactor（已有） | |

---

## 改动清单

### 1. `kwcc_base.h` — 纯 C 基础设施

```c
#define KWCC_CONFIG_MAX_MODULES 64
#define KWCC_CONFIG_MAX_KEY_LEN 64

const char *kwcc_config_get(const char *module, const char *key, const char *default_value);
int         kwcc_config_get_int32(const char *module, const char *key, int default_value);
```

- **不含 JSValue/JSGCRef/microui 类型**
- 去掉 struct 定义（移到 kwcc.c 内部）
- 去掉 `kwcc_config_set` 声明（内部使用）

### 2. `kwcc.c` — config JSValue 存储（唯一职责）

```c
/* 内部结构，不暴露给外部 */
typedef struct {
    char      module[KWCC_CONFIG_MAX_KEY_LEN];
    JSValue   options;
    JSGCRef   options_ref;
    int       in_use;
} kwcc_config_module_t;

/* 内部函数 */
void kwcc_config_set_jsctx(JSContext *ctx);        // 设置 JSContext
void kwcc_config_set_object(const char *m, JSValue obj);  // 存 JS Object + JS_AddGCRef

/* 公共函数 */
const char *kwcc_config_get(...);    // JS_GetPropertyStr + JS_ToCString
int         kwcc_config_get_int32(...);  // JS_GetPropertyStr + JS_ToInt32
```

- **无 microui**（不含 `g_mu` / `mu_init`）
- `kwcc_init()` / `kwcc_free()` 为空实现（或移除，由 `kwcc_ui.c` 负责）
- 旋转 buffer 存 config_get 返回值

### 3. `kwcc_ui.c` — 拥有 `mu_Context g_mu` + UI 全职责

新增/迁移：
- `mu_Context g_mu;`（从 kwcc.c 移入）
- `kwcc_ui_init()` — `mu_init(&g_mu)` + text 回调 + close 回调（合并原有两个函数）
- `kwcc_ui_free()` — 清理 UI 资源（当前为空，备用）
- `kwcc_process_js()` — JS 帧处理（调 `g_frame_counter` / `kwcc_begin_frame()` / `mu_begin/end`）
- `kwcc_get_mu()` — 返回 `&g_mu`
- `js_ui_dispatch()` — UI 方法调度
- `kwcc_register_ui()` — UI 对象 + methods_js
- 原有所有 UI 内容不变

**`kwcc.c` 不含 `kwcc_init()` / `kwcc_free()`**，只做 config 存储。

### 4. `kwcc.h` — 公共 API

```c
#include "kwcc_base.h"
void          kwcc_process_js(JSContext *ctx, const char *js_text);
mu_Context   *kwcc_get_mu(void);
```

- 不含 `kwcc_init()` / `kwcc_free()`（已归 kwcc_ui.c）
- 不含 `kwcc_create_js()` / `kwcc_destroy_js()`（归 kwcc_js.h）
- 不含 config struct 声明

### 5. `kwcc_js.c` — JS lifecycle + stubs

新增/迁移：
- `kwcc_create_js()` — `JS_NewContext` + `kwcc_config_set_jsctx(ctx)`
- `kwcc_destroy_js()` — `JS_FreeContext`
- `kwcc_set_ui_callback()` + `js_kwcc_ui()`
- `js_kwcc_config_set()` — 传 JSValue 到 `kwcc_config_set_object()`（不再拆字符串）
- 原有 stubs 不变

### 6. `kwcc_js.h` — JS 声明

```c
JSContext *kwcc_create_js(void);
void       kwcc_destroy_js(JSContext *ctx);
/* stubs + UI bridge 声明 */
```

### 7. `kwcc_ui.h` — UI 声明

```c
extern mu_Context g_mu;
extern svg_cache_t g_svg_cache[SVG_CACHE_SIZE];
extern int         g_frame_counter;
void kwcc_ui_init(void);
void kwcc_ui_free(void);
void kwcc_register_ui(JSContext *ctx);
void kwcc_process_js(JSContext *ctx, const char *js_text);
mu_Context *kwcc_get_mu(void);
```

### 8. JS wrapper 更新 — `kwcc_ui.c` methods_js

```javascript
kwcc_config = function(module, options) {
    kwcc_config_set(module, options);
};
```

### 9. `main.m` 初始化 + 清理顺序

```c
/* init */
js_ctx = kwcc_create_js();     /* 1. JSContext + config JSContext 注册 */
kwcc_ui_init();                /* 2. microui 初始化（不再叫 kwcc_init） */
kwcc_register_ui(js_ctx);      /* 3. UI 对象注册 */

/* cleanup */
kwcc_ui_free();                /* 1. 清理 UI 资源 */
kwcc_destroy_js(js_ctx);       /* 2. 释放 JS 上下文 */
```

---

## 文件依赖关系

```
kwcc_base.h  ← 纯 C，无框架依赖
kwcc.h       ← kwcc_base.h + mquickjs + microui（公共 API）
kwcc_ui.h    ← mquickjs + microui（UI 声明）
kwcc_js.h    ← mquickjs（JS 声明）
kwcc_io.h    ← kwcc_base.h（纯 POSIX）
```

| 源文件 | 依赖头文件 |
|--------|-----------|
| `kwcc.c` | kwcc_base.h + mquickjs.h |
| `kwcc_ui.c` | kwcc_ui.h + kwcc_core.h + kwcc_js.h |
| `kwcc_js.c` | kwcc_js.h + kwcc_base.h + mqjs_stdlib.h |
| `kwcc_io.c` | kwcc_io.h + kwcc_base.h |
| `main.m` | kwcc.h + kwcc_ui.h + kwcc_js.h |

---

## 验证

编译通过：`make clean && make`
