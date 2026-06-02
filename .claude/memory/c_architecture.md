# C 层完整架构状态

> 基于 `src/kwcc.c` 完整源码分析（661 行）

## 全局变量

| 变量 | 类型 | 行号 | 用途 |
|------|------|------|------|
| `g_mu` | `mu_Context` | 23 | microui 主上下文 |
| `g_slider_val` | `mu_Real` | 24 | 持久化 slider 值（static，避免局部地址陷阱） |
| `g_current_font` | `const char*` | 25 | 当前激活字体名称 |
| `g_topic_map[256]` | struct { mu_Id, topic[128] } | 31 | 每帧 ID→topic 映射（Zero-Alloc） |
| `g_topic_map_count` | `int` | 32 | topic map 计数 |
| `g_sync_table[32]` | `mod_state_t` { key[64], visible } | 44 | 模块状态同步表 |
| `g_sync_count` | `int` | 45 | sync 计数 |
| `g_current_mod_key` | `const char*` | 48 | 当前模块 key（ui.sync 设置） |
| `g_win_intercepted[32]` | `int` | 51 | 窗口拦截栈 |
| `g_win_topics[32][128]` | `char` | 52 | 窗口 topic 栈 |
| `g_win_top` | `int` | 53 | 窗口栈顶 |
| `g_js_ctx` | `JSContext*` | 56 | JS 上下文（close 回调用） |
| `g_svg_cache[128]` | `svg_cache_t` | 135 | SVG 缓存（extern，共享给 main.m） |
| `g_svg_cache_next` | `int` | 136 | SVG 缓存轮转指针 |
| `g_frame_counter` | `int` | 137 | 帧计数器 |

## C 层函数清单

### 核心生命周期
| 函数 | 行号 | 功能 |
|------|------|------|
| `kwcc_init()` | 552 | 初始化 microui + text 回调 |
| `kwcc_free()` | 558 | 空 |
| `kwcc_create_js()` | 561 | 创建 JSContext + 注册 ui 对象 + methods_js |
| `kwcc_destroy_js()` | 610 | 释放 JSContext |
| `kwcc_begin_frame()` | 95 | 每帧重置：topic_map_count=0, sync_count=0, win_top=0 |
| `kwcc_process_js()` | 616 | 帧入口：frame++ → begin_frame → mu_begin → JS_Eval → mu_end |
| `kwcc_get_mu()` | 634 | 返回 g_mu 指针 |
| `kwcc_get_font()` | 638 | 返回当前字体名称 |

### 事件系统
| 函数 | 行号 | 功能 |
|------|------|------|
| `kwcc_bind_topic(id, topic)` | 101 | 存入 g_topic_map（id→topic） |
| `kwcc_dispatch_event(ctx, topic, action)` | 112 | JS_Eval 调用 `$bus.emit(topic, action, new Object())` |
| `kwcc_on_window_close(ctx, title)` | 89 | X 按钮回调：dispatch(title, "close") |

### 窗口挡板
| 函数 | 行号 | 功能 |
|------|------|------|
| `kwcc_sync_module(key, visible)` | 63 | 设置当前模块 key + 更新 visible 状态 |
| `kwcc_get_current_visibility()` | 79 | 查 g_sync_table 返回当前模块可见性 |

### SVG 缓存
| 函数 | 行号 | 功能 |
|------|------|------|
| `fnv1a(s)` | 139 | FNV-1a 哈希 |
| `svg_resolve(data, is_inline)` | 145 | 哈希匹配 → 解析 → 帧安全淘汰 |
| `kwcc_queue_svg(data, is_inline, x, y, w, h)` | 206 | 先 mu_get_clip_rect → 再 svg_resolve → 入队 |

### 字体系统
| 函数 | 行号 | 功能 |
|------|------|------|
| `is_cjk_hint(name)` | 485 | 检测字体名是否包含 CJK 关键词 |
| `kwcc_load_font_dir(dir_path)` | 505 | 扫描目录下 .ttf/.otf 字体，自动选 CJK 字体 |

### 输入事件
| 函数 | 行号 | 功能 |
|------|------|------|
| `kwcc_input_mousemove(x, y)` | 642 | → mu_input_mousemove |
| `kwcc_input_mousedown(x, y, btn)` | 646 | → mu_input_mousedown |
| `kwcc_input_mouseup(x, y, btn)` | 650 | → mu_input_mouseup |
| `kwcc_input_scroll(x, y)` | 654 | → mu_input_scroll |
| `kwcc_input_text(text)` | 658 | → mu_input_text |

### JS wrapper（methods_js，kwcc.c:581-598）

所有 ui.* 方法通过 `kwcc_ui(method, ...args)` 桥接，JS wrapper 定义在 `methods_js` 字符串中，`kwcc_create_js` 时 `JS_Eval` 执行。

### microui 文本回调
| 函数 | 行号 | 功能 |
|------|------|------|
| `mu_text_width(font, str, len)` | 222 | nvgTextBounds 真实测量（nvgFontFace + nvgFontSize 14） |
| `mu_text_height(font)` | 234 | nvgTextBounds("Hy") 真实测量 |

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
```

## 未实现的常见控件

以下控件在 microui 中有对应，但 kwcc.c 中没有桥接：

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

## mquickjs C API 速查（基于 `mquickjs.h` + `jsapi.c` 验证）

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
| 函数实现 | `src/jsapi.c` 或 `src/mquickjs_stubs.c` |

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
