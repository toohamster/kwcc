# C 层完整架构状态

> 基于 UI 模块拆分后的多文件架构（`kwcc_ui.c` + `kwcc_js.c` + `kwcc.c`）

## 文件职责分布

| 文件 | 职责 | 不含 |
|------|------|------|
| `kwcc_base.h` | 纯 C 类型 + config getter 声明 | 无 JS/microui 类型 |
| `kwcc.c` | config JSValue 存储（内部 `__kwcc_config` 对象方案） | 无 microui |
| `kwcc_ui.c` | `g_mu` + `kwcc_ui_init/free` + `kwcc_process_js` + UI 桥接 + input + SVG + 字体 | 无 JS lifecycle |
| `kwcc_ui.h` | UI 模块声明 + SVG cache extern + `g_mu` extern | |
| `kwcc_js.c` | `kwcc_create/destroy_js` + stdlib stubs + kwcc_ui 桥接 | 无 microui |
| `kwcc_js.h` | JS lifecycle + stubs 声明 | 无 microui |
| `kwcc.h` | 入口 umbrella header（#include 各模块头文件） | 无实现代码 |

## 全局变量

### kwcc_ui.c
| 变量 | 类型 | 用途 |
|------|------|------|
| `g_mu` | `mu_Context` | microui 主上下文（**定义**，非 extern） |
| `g_slider_val` | `mu_Real` | 持久化 slider 值（static，避免局部地址陷阱） |
| `g_current_font` | `const char*` | 当前激活字体名称 |
| `g_topic_map[256]` | struct { mu_Id, topic[128] } | 每帧 ID→topic 映射（Zero-Alloc） |
| `g_topic_map_count` | `int` | topic map 计数 |
| `g_sync_table[32]` | `mod_state_t` { key[64], visible } | 模块状态同步表 |
| `g_sync_count` | `int` | sync 计数 |
| `g_current_mod_key` | `const char*` | 当前模块 key（ui.sync 设置） |
| `g_win_intercepted[32]` | `int` | 窗口拦截栈 |
| `g_win_topics[32][128]` | `char` | 窗口 topic 栈 |
| `g_win_top` | `int` | 窗口栈顶 |
| `g_js_ctx` | `JSContext*` | JS 上下文（close 回调用） |
| `g_svg_cache[128]` | `svg_cache_t` | SVG 缓存（extern，共享给 main.m） |
| `g_svg_cache_next` | `int` | SVG 缓存轮转指针 |
| `g_frame_counter` | `int` | 帧计数器 |

### kwcc_js.c
| 变量 | 类型 | 用途 |
|------|------|------|
| `g_ui_callback` | `JSUICallback` | UI 桥接回调指针（kwcc_set_ui_callback 设置） |

### kwcc.c
| 变量 | 类型 | 用途 |
|------|------|------|
| `g_js_ctx` | `JSContext*` | config 存储用的 JSContext |

## C 层函数清单

### 核心生命周期
| 函数 | 所在文件 | 功能 |
|------|----------|------|
| `kwcc_ui_init()` | kwcc_ui.c | `mu_init` + text 回调 + `on_window_close` |
| `kwcc_ui_free()` | kwcc_ui.c | 清理 UI 资源（当前空，备用） |
| `kwcc_create_js()` | kwcc_js.c | 创建 JSContext + 调 `kwcc_config_set_jsctx` |
| `kwcc_destroy_js()` | kwcc_js.c | 释放 JSContext |
| `kwcc_begin_frame()` | kwcc_ui.c | 每帧重置：topic_map_count=0, sync_count=0, win_top=0 |
| `kwcc_process_js()` | kwcc_ui.c | 帧入口：frame++ → begin_frame → mu_begin → JS_Eval → mu_end |
| `kwcc_get_mu()` | kwcc_ui.c | 返回 g_mu 指针 |
| `kwcc_get_font()` | kwcc_ui.c | 返回当前字体名称 |

### 事件系统（kwcc_ui.c）
| 函数 | 功能 |
|------|------|
| `kwcc_bind_topic(id, topic)` | 存入 g_topic_map（id→topic） |
| `kwcc_dispatch_event(ctx, topic, action)` | JS_Eval 调用 `$bus.emit(topic, action, new Object())` |
| `kwcc_on_window_close(ctx, title)` | X 按钮回调：dispatch(title, "close") |

### 窗口挡板（kwcc_ui.c）
| 函数 | 功能 |
|------|------|
| `kwcc_sync_module(key, visible)` | 设置当前模块 key + 更新 visible 状态 |
| `kwcc_get_current_visibility()` | 查 g_sync_table 返回当前模块可见性 |

### SVG 缓存（kwcc_ui.c）
| 函数 | 功能 |
|------|------|
| `fnv1a(s)` | FNV-1a 哈希 |
| `svg_resolve(data, is_inline)` | 哈希匹配 → 解析 → 帧安全淘汰 |
| `kwcc_queue_svg(data, is_inline, x, y, w, h)` | 先 mu_get_clip_rect → 再 svg_resolve → 入队 |

### 字体系统（kwcc_ui.c）
| 函数 | 功能 |
|------|------|
| `is_cjk_hint(name)` | 检测字体名是否包含 CJK 关键词 |
| `kwcc_load_font_dir(dir_path)` | 扫描目录下 .ttf/.otf 字体，自动选 CJK 字体 |

### 输入事件（kwcc_ui.c）
| 函数 | 功能 |
|------|------|
| `kwcc_input_mousemove(x, y)` | → mu_input_mousemove |
| `kwcc_input_mousedown(x, y, btn)` | → mu_input_mousedown |
| `kwcc_input_mouseup(x, y, btn)` | → mu_input_mouseup |
| `kwcc_input_scroll(x, y)` | → mu_input_scroll |
| `kwcc_input_text(text)` | → mu_input_text |

### JS stubs（kwcc_js.c）
| 函数 | 功能 |
|------|------|
| `js_print` | 打印到 stdout + log |
| `js_gc` | 触发 GC |
| `js_load` | 读取并 eval JS 文件 |
| `js_setTimeout` / `js_clearTimeout` | stub |
| `js_date_now` / `js_performance_now` | stub |
| `js_kwcc_ui` | UI 桥接入口 → g_ui_callback |
| `js_kwcc_config_set` | config 设置入口 → kwcc_config_set |

### kwcc_ui.c — js_ui_dispatch + register_ui
`js_ui_dispatch` — 所有 ui.* 方法的 C handler，按 method 名 dispatch
`kwcc_register_ui` — 创建 `ui` 对象 + 注入 JS wrapper + 调 `kwcc_set_ui_callback`

### Config 存储（kwcc.c）
| 函数 | 功能 |
|------|------|
| `kwcc_config_set_jsctx(ctx)` | 设置 JSContext（内部，kwcc_create_js 调用） |
| `kwcc_config_set_object(module, obj)` | 存储 JS object 到 `__kwcc_config` 全局对象 |
| `kwcc_config_set(module, key, value)` | C stub：字符串形式 config set |
| `kwcc_config_get(module, key, default)` | 旋转 buffer 返回字符串值 |
| `kwcc_config_get_int32(module, key, default)` | 返回 int32 值 |

## 已实现的 JS API（完整清单）

```javascript
// 窗口
ui.beginWindow(title, x, y, w, h, opt, topic)
ui.endWindow()

// 状态同步
ui.sync(key, visible)

// 面板
ui.beginPanel(name, opt)
ui.endPanel()

// 交互控件
ui.button(text, topic)        // 返回 bool(点击), topic 非空时 dispatch "click"
ui.slider(text, value, min, max, topic)  // 返回 value, 变化时 dispatch "change"

// 布局
ui.layoutRow(height, w1, w2, w3, w4)
ui.setNext(x, y, w, h)

// 显示
ui.label(text)
ui.display(text)              // 深色背景 + 右对齐白色文本
ui.textCentered(text)
ui.rect(x, y, w, h, r, g, b)

// 字体
ui.loadFont(name, path)
ui.setFont(name)
ui.loadFontDir(dir)

// SVG
ui.svg(path_or_svg, x, y, w, h)

// config
kwcc_config(module, options)  // 遍历 options 的 keys，逐个调 kwcc_config_set
```

## 未实现的常见控件

以下控件在 microui 中有对应，但 kwcc 中没有桥接：

| microui 函数 | 状态 |
|--------------|------|
| `mu_checkbox` | ❌ 未实现 |
| `mu_textbox` | ❌ 未实现 |
| `mu_header` | ❌ 未实现 |
| `mu_begin_treenode` | ❌ 未实现 |
| `mu_icon` | ❌ 未实现（JS 层无入口） |

## 重要设计决策记录

1. **button dispatch action 固定为 "click"**：topic 已经唯一标识按钮，handler 不需要识别 action
2. **slider 值用 static 变量**：避免局部地址陷阱（microui 用 &value 作为 ID）
3. **topic map 每帧清零**：g_topic_map_count = 0 在 begin_frame 中，不保留跨帧数据
4. **deferred 机制已移除**：on_window_close 直接 dispatch，不再先存数组
5. **on_window_close 收到的是 title 不是 topic**：dispatch 的 topic 参数是 title 字符串，目前没有通过 ID 查找正确 topic 的映射（待 v2 完成后修复）
6. **config 存储用全局对象方案**：`__kwcc_config` 挂在 JS global 上，避免 JSGCRef 手动管理

## JS 框架 API（main.js）

### 框架变量（$ 前缀）
| 变量 | 用途 |
|------|------|
| `$modules` | 模块注册表（name → mod 对象） |
| `$moduleKeys` | 模块注册顺序（用于遍历） |
| `$topics` | topic 注册表（name → topic 对象） |
| `$loadedFiles` | 已加载文件记录（loadOnce 内部使用） |
| `$store` | Store 实例（createStore 创建） |
| `$bus` | EventBus 实例（createBus 创建） |

### 注册 API
| 函数 | 用途 | 去重 |
|------|------|------|
| `loadJs(path)` | 加载 JS 文件，已加载则跳过 | ✅ |
| `loadJs(path, once)` | `once=1`(默认)只加载一次；`once=0` 强制加载 | ✅ |
| `registerModule(name, mod)` | 注册模块 state + actions + initEvents | ✅ 已注册跳过 |
| `registerModuleView(name, renderFn)` | 注册模块 view 渲染函数 | ✅ 已注册跳过 |
| `registerTopic(name, topics)` | 注册模块 topic 常量 | ✅ 已注册跳过 |

### 初始化
| 函数 | 用途 |
|------|------|
| `initStore()` | 聚合所有模块 state + actions → 创建 $store |
| `initEvents()` | 遍历模块调用 initEvents() → 注册 $bus handler |
| `onFrame()` | 每帧调用，遍历模块 render，自动 ui.sync |

## mquickjs C API 速查

### JSValue 类型与创建
| 用途 | API | 说明 |
|------|-----|------|
| 创建字符串 | `JS_NewString(ctx, buf)` 或 `JS_NewStringLen(ctx, buf, len)` | |
| 创建整数 | `JS_NewInt32(ctx, v)` / `JS_NewInt64(ctx, v)` / `JS_NewFloat64(ctx, v)` | |
| 创建布尔 | `JS_NewBool(v)` | |
| 创建对象 | `JS_NewObject(ctx)` | 不用 `{}` |
| 创建数组 | `JS_NewArray(ctx, initial_len)` | |
| 未定义值 | `JS_UNDEFINED` | 宏常量，uint64 |
| JSValue 本质 | `uint64_t` (64 位平台) | 不是结构体 |

### JSValue → C 值
| 用途 | API | 说明 |
|------|-----|------|
| 转 C 字符串 | `JS_ToCString(ctx, val, &cbuf)` | 返回 `const char*`，`cbuf` 是 `JSCStringBuf[5]` 栈缓冲 |
| 转 int32 | `JS_ToInt32(ctx, &pres, val)` | 返回 0 成功，-1 异常 |
| 转 uint32 | `JS_ToUint32(ctx, &pres, val)` | |
| 转 double | `JS_ToNumber(ctx, &pres, val)` | |
| 转字符串 | `JS_ToString(ctx, val)` | 返回 JSValue |

### 对象/数组操作
| 用途 | API |
|------|-----|
| 获取属性 | `JS_GetPropertyStr(ctx, obj, "key")` → JSValue |
| 设置属性 | `JS_SetPropertyStr(ctx, obj, "key", val)` |
| 获取数组元素 | `JS_GetPropertyUint32(ctx, arr, idx)` → JSValue |
| 获取数组长度 | `JS_GetPropertyStr(ctx, arr, "length")` + `JS_ToInt32` |

### 类型判断
| 用途 | API |
|------|-----|
| 判断字符串 | `JS_IsString(ctx, val)` |
| 判断数字 | `JS_IsNumber(ctx, val)` |
| 判断 null | `JS_IsNull(val)` |
| 判断 undefined | `JS_IsUndefined(val)` |
| 判断函数 | `JS_IsFunction(ctx, val)` |
| 判断数组 | `JS_GetClassID(ctx, val) == JS_CLASS_ARRAY` |
| 判断错误 | `JS_IsError(ctx, val)` |

### C 函数注册
| 步骤 | 说明 |
|------|------|
| 函数签名 | `JSValue js_func(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)` |
| 注册位置 | `deps/mquickjs/mqjs_stdlib.c` 的 `js_global_object[]` 数组 |
| 宏 | `JS_CFUNC_DEF("name", argc, func)` |
| 不要重复注册 | 不在 `js_c_function_decl[]` 中注册 |
| Makefile | `HOST_CFLAGS` 加 `-DCONFIG_KWCC` |
| 函数实现 | `src/kwcc_js.c` |

### C 函数调用（C→JS）
| 方式 | 说明 |
|------|------|
| `JS_Eval(ctx, code, len, filename, JS_EVAL_REPL)` | 执行 JS 字符串，REPL 模式可创建全局变量 |
| `JS_GetGlobalObject(ctx)` | 获取全局对象 |
| `JS_Call(ctx, call_flags)` | 调用已压栈的函数（需先 `JS_PushArg`） |
| `JS_GetException(ctx)` | 获取异常（调用失败后检查） |

### 关键陷阱
| 陷阱 | 说明 |
|------|------|
| `JS_ToCString` 返回 NULL | 必须 NULL 检查后再使用（如传字符串给 microui 函数） |
| `JSCStringBuf` 大小 | 仅 5 字节 `uint8_t buf[5]`，短字符串内联存储 |
| 不支持 `...rest` | mquickjs 不支持展开参数，JS 侧传固定参数 |
| `{}` 语句开头 | 被解析为 block，JS 侧用 `new Object()` |
| 头文件必须 include | 否则 x86_64 ABI float→double 提升导致参数错误 |
| GC 安全 | 长时间持有的 JSValue 用 `JS_PushGCRef` / `JS_AddGCRef` 保护 |
