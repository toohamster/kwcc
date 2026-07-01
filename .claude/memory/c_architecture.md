# C 层完整架构状态

> 基于多文件架构（`kwcc_mempool.c` + `kwcc_config.c` + `kwcc_ui.c` + `kwcc_js.c` + `kwcc_bus.c` + `kwcc_ui_bus.c` + `kwcc_base.c` + `kwcc_io.c` + `kwcc_http.c`）

## 架构模式

项目使用两种 C 模块模式（详见 [c_module_patterns.md](c_module_patterns.md)）：

| 模式 | 适用场景 | 项目实例 |
|------|----------|----------|
| A: 模块前缀 + 静态全局 | 单例、无隔离需求 | `kwcc_bus`, `kwcc_http`, `kwcc_config`, `kwcc_mempool`, `kwcc_io` |
| B: struct + 函数指针 | 隔离/多实例/可替换 | `kwcc_js_ops_t`, `kwcc_js_module_t` |

## 文件职责分布

| 文件 | 职责 |
|------|------|
| `kwcc_core.h` | 核心生命周期声明：`g_frame_counter` + `kwcc_begin_frame`（kwcc_ui.c 直接 include） |
| `kwcc_base.h` | 纯 C 基础设施：内存池编译常量 + topic 清洗/校验声明 |
| `kwcc_base.c` | topic 清洗（`kwcc_base_topic_sanitize`）+ 校验（`kwcc_base_topic_check`） |
| `kwcc_mempool.h/c` | L0-L7 Slab 内存池：alloc/set/get/release/GC/key_map/常量表/TLV 序列化 |
| `kwcc_config.h/c` | Config 层：get_core/get_app 前缀封装，C 业务模块读取接口 |
| `kwcc_ui.c` | `g_kwcc_mu` + UI 桥接 + input + SVG + 字体 + js_ui_dispatch + register_ui |
| `kwcc_ui.h` | UI 模块声明 + SVG cache extern |
| `kwcc_ui_bus.c` | UI→JS 事件桥接：topic map + bind_topic + dispatch_event + begin_frame |
| `kwcc_ui_bus.h` | UI 桥接声明（4 个 API） |
| `kwcc_bus.c` | 通用 C Pub/Sub 事件总线：subscribe/publish/unsubscribe，零 mquickjs 依赖 |
| `kwcc_bus.h` | 事件总线声明 + `KWCC_BUS_WILDCARD` 常量 |
| `kwcc_js.c` | **Facade**：封装 mquickjs，提供 `kwcc_js_ops_t` 操作接口 + 模块注册 + `$notify` 通道 + bus consumer |
| `kwcc_js.h` | JS Facade 类型：`kwcc_js_ops_t` + `kwcc_js_module_t` + `kwcc_js_val_t` + `kwcc_js_cstr_buf_t` + 生命周期 API |
| `kwcc_io.c` | I/O Reactor：select() 非阻塞 FD 管理器 |
| `kwcc_io.h` | I/O 管理器声明 |
| `kwcc_http.c` | HTTP 模块：fork+pipe+curl+picohttpparser，bus 事件分发 |
| `kwcc_http.h` | HTTP 模块 API 声明 |
| `kwcc.h` | 入口 umbrella header（include 各模块头文件） |

## 全局变量

### main.m
| 变量 | 类型 | 用途 |
|------|------|------|
| `g_kwcc_vg` | `NVGcontext*` | NanoVG 渲染上下文（**定义**，非 extern） |
| `g_js_ctx` | `JSContext*` (static) | JS 上下文 |
| `g_js_text` | `const char*` (static) | 缓存的 JS 代码 |
| `g_log_fp` | `FILE*` (static) | 日志文件指针 |
| `kwcc_load_file` | `const char*` (static func) | 读取文件到静态 buffer |
| `kwcc_render_mu_commands` | `void` (static func) | 遍历 microui 命令队列并渲染 |

### kwcc_mempool.c
| 变量 | 类型 | 用途 |
|------|------|------|
| `g_kwcc_mempool_const_table[16]` | `kwcc_mempool_const_t` | 常量表（null/空串/0/1/true/false/-1 等） |
| `g_kwcc_mempool_key_map[32768]` | `kwcc_mempool_keymap_t` | 开地址哈希表，O(1) key→slot 查找 |
| `g_kwcc_mempool_mgr` | `kwcc_mempool_manager_t` | 池管理器（L0-L7 池指针 + 计数 + max_pools） |
| `g_kwcc_mempool_max_pools[8]` | `const int` | 各层最大池数（编译时常量） |
| `g_kwcc_mempool_l7_used` | `uint64_t` | L7 已用字节数 |
| `g_kwcc_mempool_last_gc_time` | `uint32_t` (static) | 上次 GC 时间戳 |

### kwcc_ui.c
| 变量 | 类型 | 用途 |
|------|------|------|
| `g_kwcc_mu` | `mu_Context` | microui 主上下文（**定义**，非 extern） |
| `g_kwcc_ui_slider_val` | `mu_Real` (static) | 持久化 slider 值 |
| `g_kwcc_ui_current_font` | `const char*` (static) | 当前激活字体名称 |
| `g_kwcc_ui_sync_table[32]` | `mod_state_t` (static) | 模块状态同步表 |
| `g_kwcc_ui_sync_count` | `int` (static) | sync 计数 |
| `g_kwcc_ui_current_mod_key` | `const char*` (static) | 当前模块 key |
| `g_kwcc_ui_win_intercepted[32]` | `int` (static) | 窗口拦截栈 |
| `g_kwcc_ui_win_topics[32][128]` | `char` (static) | 窗口 topic 栈 |
| `g_kwcc_ui_win_top` | `int` (static) | 窗口栈顶 |
| `g_kwcc_ui_js_ctx` | `JSContext*` (static) | JS 上下文（close 回调用） |
| `g_kwcc_ui_svg_cache[128]` | `kwcc_ui_svg_cache_t` | SVG 缓存（extern，共享给 main.m） |
| `g_kwcc_ui_svg_cache_next` | `int` | SVG 缓存轮转指针 |
| `g_kwcc_ui_frame_counter` | `int` | 帧计数器 |

### kwcc_ui_bus.c
| 变量 | 类型 | 用途 |
|------|------|------|
| `g_kwcc_ui_topic_map[256]` | struct { int id, topic[128] } | 每帧 ID→topic 映射 |
| `g_kwcc_ui_topic_count` | `int` | topic map 计数 |
| `g_kwcc_ui_bus_js_ctx` | `JSContext*` (static) | JS 上下文 |

### kwcc_bus.c
| 变量 | 类型 | 用途 |
|------|------|------|
| `g_kwcc_bus_head` | `kwcc_bus_group_t*` (static) | topic group 链表头 |
| `g_kwcc_bus_next_id` | `uint64_t` (static) | 下一个 subscriber ID |

### kwcc_io.c
| 变量 | 类型 | 用途 |
|------|------|------|
| `g_kwcc_io_slots[KWCC_IO_MAX_FDS]` | `kwcc_io_slot_t` | FD 槽位表 |
| `g_kwcc_io_max_fds` | `int` (static) | 活跃 FD 数上限（默认 16，可调） |

### kwcc_js.c
| 变量 | 类型 | 用途 |
|------|------|------|
| `g_kwcc_js_ops` | `kwcc_js_ops_t` | **Facade ops 实例**（非 static，测试可访问） |
| `g_kwcc_js_modules` | `kwcc_js_module_t**` (static) | 已注册模块列表（动态增长） |
| `g_kwcc_js_dispatch_modules` | `const char**` (static) | 分发表 module 名数组（动态增长） |
| `g_kwcc_js_dispatch_groups` | `kwcc_js_dispatch_group_t*` (static) | 分发表 module 分组（每模块独立 handler 列表） |
| `s_notify_emit_fn` | `kwcc_js_val_t` (static) | `$notify.emit` 缓存引用（global 可达，GC 安全） |
| `g_ui_callback` | `JSUICallback` | UI 桥接回调指针 |

## C 层函数清单

### 核心生命周期
| 函数 | 所在文件 | 功能 |
|------|----------|------|
| `kwcc_mempool_init()` | kwcc_mempool.c | 初始化常量表 + key_map + L0-L7 各 1 个池 |
| `kwcc_mempool_shutdown()` | kwcc_mempool.c | 释放所有池 + 清零 |
| `kwcc_ui_init()` | kwcc_ui.c | `mu_init` + text 回调 + `on_window_close` |
| `kwcc_ui_free()` | kwcc_ui.c | 清理 UI 资源（当前空，备用） |
| `kwcc_create_js()` | kwcc_js.c | 创建 JSContext + ops_init + register_modules + bus_subscribe |
| `kwcc_destroy_js()` | kwcc_js.c | 释放 JSContext |
| `kwcc_begin_frame()` | kwcc_ui.c | 每帧重置 + 调 kwcc_bus_begin_frame |
| `kwcc_process_js()` | kwcc_ui.c | 帧入口：frame++ → begin_frame → mu_begin → JS_Eval → mu_end |
| `kwcc_get_mu()` | kwcc_ui.c | 返回 &g_kwcc_mu |
| `kwcc_get_font()` | kwcc_ui.c | 返回当前字体名称 |

### Facade + Module（kwcc_js.c）
| 函数 | 功能 |
|------|------|
| `kwcc_js_ops_init(ctx)` | 初始化 `g_kwcc_js_ops`：绑定 18 个函数指针 + 5 个属性 |
| `kwcc_js_inject_notify(ops)` | 注入 `$notify` 对象 + 缓存 `s_notify_emit_fn` |
| `kwcc_js_register_module(ops, mod)` | 调用 mod→load + 读取 mod→apis 注册进分发表，加入模块列表 |
| `kwcc_js_register_modules(ops)` | 注入 `$notify` → 注册 core APIs → 逐个注册模块 |
| `kwcc_js_dispatch_add(module, func, handler)` | 添加 handler 到分发表（module 分组，重复 func 覆盖） |
| `kwcc_js_dispatch_call(module, func, argc, argv)` | 从分发表查找 handler 并调用（ops 签名） |
| `kwcc_js_call_c(ctx, this_val, argc, argv)` | JS 全局函数 `kwcc_js_call_c`，提取 module/func 后调 dispatch_call |
| `kwcc_js_on_bus_event(topic, data, len, user_data)` | 遍历模块 on_bus_event → 默认白名单 → `$bus.emit` |
| `kwcc_js_match_whitelist(whitelist, topic)` | 白名单前缀匹配（逗号分隔） |

### ops impl 函数指针（kwcc_js.c，全部 static）
| 函数 | 对应 ops 指针 |
|------|--------------|
| `kwcc_js_new_object_impl` | `new_object` |
| `kwcc_js_new_int32_impl` | `new_int32` |
| `kwcc_js_new_string_impl` | `new_string` |
| `kwcc_js_new_string_len_impl` | `new_string_len` |
| `kwcc_js_set_str_prop_impl` | `set_str_prop` |
| `kwcc_js_get_str_prop_impl` | `get_str_prop` |
| `kwcc_js_is_function_impl` | `is_function` |
| `kwcc_js_call_cb_impl` | `call_cb`（含 JS_StackCheck+PushArg+Call+异常捕获） |
| `kwcc_js_to_cstring_impl` | `to_cstring`（含短字符串内联 buf 拷贝） |
| `kwcc_js_is_undefined_impl` | `is_undefined` |
| `kwcc_js_is_null_impl` | `is_null` |
| `kwcc_js_is_exception_impl` | `is_exception` |
| `kwcc_js_eval_impl` | `eval` |
| `kwcc_js_get_class_id_impl` | `get_class_id` |
| `kwcc_js_array_length_impl` | `array_length` |
| `kwcc_js_array_get_impl` | `array_get` |
| `kwcc_js_to_int32_impl` | `to_int32` |
| `kwcc_js_notify_js_impl` | `notify_js`（ack_cleanup → call_cb $notify.emit，NULL id 防御） |

### MemPool 层（kwcc_mempool.c）
| 函数 | 功能 |
|------|------|
| `kwcc_mempool_alloc(type, key, size, timeout)` | 按 size 路由到 L0-L6 或 L7 分配 slot，ref_count=1 |
| `kwcc_mempool_alloc_dynamic(key, cap, timeout)` | L7 动态 malloc 分配 |
| `kwcc_mempool_get(key)` | O(1) key_map 查找返回 slot |
| `kwcc_mempool_get_str(key, default)` | 返回字符串，自动补 \0 |
| `kwcc_mempool_set(slot, data, size)` | 写入数据（兜底 const_lookup） |
| `kwcc_mempool_acquire(slot)` | ref_count++ |
| `kwcc_mempool_release(slot)` | ref_count--，降到 0 可被 GC 回收 |
| `kwcc_mempool_invalidate(slot)` | 强制 ref_count=0, timeout=0 |
| `kwcc_mempool_gc()` | 5 秒节流的 GC（ref_count=0 或超时） |
| `kwcc_mempool_gc_force()` | 强制 GC，无节流 |
| `kwcc_mempool_gc_auto()` | 按使用率自动 GC（>80% 触发 force） |
| `kwcc_mempool_get_keys(prefix, out_keys, max)` | 前缀扫描返回 key 列表 |
| `kwcc_mempool_set_max_pools(type, max)` | 调整某层最大池数 |
| `kwcc_mempool_const_lookup(data, len, type)` | 常量表查找 |
| `kwcc_mempool_fnv1a(s)` | FNV-1a 哈希 |
| `kwcc_mempool_tlv_build(cb, user_data, out_len)` | TLV 序列化（回调驱动） |
| `kwcc_mempool_tlv_iter(data, len, cb, user_data)` | TLV 遍历（回调驱动） |
| `kwcc_mempool_tlv_get_path(data, len, path, out_len)` | TLV 路径查询 |
| `kwcc_mempool_tlv_to_json(data, len, out_len)` | TLV → JSON 字符串（含转义） |
| `kwcc_mempool_tlv_free_json(ptr)` | 释放 JSON 字符串 |

### Config 层（kwcc_config.c）
| 函数 | 功能 |
|------|------|
| `kwcc_config_set_app_int(key, val)` | 自动拼 "a." 前缀 → mempool alloc+set |
| `kwcc_config_set_app_string(key, val)` | 同上，字符串 |
| `kwcc_config_set_app_bool(key, val)` | 同上，布尔 |
| `kwcc_config_set_app_tlv(key, data, len)` | 同上，TLV 数据 |
| `kwcc_config_get_app(key, default)` | 自动拼 "a." 前缀 → mempool get → 字符串 |
| `kwcc_config_release_app(key)` | 自动拼 "a." 前缀 → mempool release |
| `kwcc_config_release_app_prefix(key)` | 前缀批量 release |
| `kwcc_config_set_core_tlv(key, data, len)` | 自动拼 "c." 前缀 → mempool |
| `kwcc_config_get_core(key, default)` | 自动拼 "c." 前缀 → mempool get |
| `kwcc_config_get_core_slot(key)` | 返回 slot 指针（用于 TLV 路径查询） |
| `kwcc_config_release_core(key)` | 自动拼 "c." 前缀 → mempool release |
| `kwcc_config_set_max_pools(type, max)` | 转发到 mempool |

### Topic 工具（kwcc_base.c）
| 函数 | 功能 |
|------|------|
| `kwcc_base_topic_sanitize(out, out_size, in)` | 清洗 topic：只保留 A-Z a-z 0-9 / _，末尾 /* 保留 |
| `kwcc_base_topic_check(topic)` | 校验 topic：拒绝空字符串、全是 / 的 topic |

### UI→JS 桥接（kwcc_ui_bus.c）
| 函数 | 功能 |
|------|------|
| `kwcc_ui_bus_set_js_ctx(ctx)` | 设置 JS 上下文 |
| `kwcc_ui_bus_begin_frame()` | 每帧重置 topic map |
| `kwcc_ui_bus_bind_topic(id, topic)` | 存入 ID→topic 映射，入口 sanitize + check |
| `kwcc_ui_bus_dispatch_event(topic, action)` | JS_Eval 调用 `$bus.emit(topic, action, new Object())`，入口 sanitize + check |

### C Pub/Sub 事件总线（kwcc_bus.c）
| 函数 | 功能 |
|------|------|
| `kwcc_bus_init()` | 初始化 bus（清空链表 + 重置 ID） |
| `kwcc_bus_subscribe(topic, cb, user_data)` | 订阅 topic，入口 sanitize + check，返回 sub_id |
| `kwcc_bus_unsubscribe(id)` | 按 sub_id 取消订阅 |
| `kwcc_bus_publish(topic, data, len)` | 发布事件，入口 sanitize + check，匹配触发回调 |
| `kwcc_bus_match_topic(pattern, topic)` (static) | 匹配：精确 / `/*` 通配 / `/` 前缀 |

### I/O Reactor（kwcc_io.c）
| 函数 | 功能 |
|------|------|
| `kwcc_io_init()` | 初始化 FD 槽位表，读取 config 获取 max_fds |
| `kwcc_io_register(fd, cb, user_data)` | 注册 FD + 回调 |
| `kwcc_io_unregister(fd)` | 注销 FD |
| `kwcc_io_poll_once()` | select() 零超时轮询，回调 dispatch |

### 窗口挡板（kwcc_ui.c）
| 函数 | 功能 |
|------|------|
| `kwcc_sync_module(key, visible)` | 设置当前模块 key + 更新 visible 状态 |
| `kwcc_get_current_visibility()` | 查 g_kwcc_ui_sync_table 返回当前模块可见性 |

### SVG 缓存（kwcc_ui.c）
| 函数 | 功能 |
|------|------|
| `kwcc_ui_fnv1a(s)` | FNV-1a 哈希 |
| `kwcc_ui_svg_resolve(data, is_inline)` | 哈希匹配 → 解析 → 帧安全淘汰 |
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

### kwcc_ui.c — js_ui_dispatch + register_ui
`js_ui_dispatch` — 所有 ui.* 方法的 C handler，按 method 名 dispatch
`kwcc_register_ui` — 创建 `ui` 对象 + 注入 JS wrapper + 调 `kwcc_set_ui_callback`

## 命名规范

| 类型 | 格式 | 示例 |
|------|------|------|
| 函数 | `kwcc_<module>_` | `kwcc_mempool_alloc`, `kwcc_ui_init` |
| 全局变量 | `g_kwcc_<module>_` | `g_kwcc_mu`, `g_kwcc_ui_sync_count` |
| Static 变量 | `g_kwcc_<module>_` | `g_kwcc_ui_current_font` |
| 宏 | `KWCC_<MODULE>_` | `KWCC_UI_SVG_CACHE_SIZE`, `KWCC_MEMPOOL_L7` |
| 类型 | `<module>_t` | `kwcc_mempool_slot_t`, `kwcc_ui_svg_cache_t` |

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

以下控件在 microui 中有对应，但 kwcc 中没有桥接：

| microui 函数 | 状态 |
|--------------|------|
| `mu_checkbox` | 未实现 |
| `mu_textbox` | 未实现 |
| `mu_header` | 未实现 |
| `mu_begin_treenode` | 未实现 |
| `mu_icon` | 未实现（JS 层无入口） |

## 重要设计决策记录

1. **button dispatch action 固定为 "click"**：topic 已经唯一标识按钮，handler 不需要识别 action
2. **slider 值用 static 变量**：避免局部地址陷阱（microui 用 &value 作为 ID）
3. **topic map 每帧清零**：g_kwcc_ui_topic_count = 0 在 ui_bus_begin_frame 中，不保留跨帧数据
4. **deferred 机制已移除**：on_window_close 直接 dispatch，不再先存数组
5. **on_window_close 收到的是 title 不是 topic**：dispatch 的 topic 参数是 title 字符串（待 v2 完成后修复）
6. **Config 存储用 mempool 方案**：通过 kwcc_mempool_alloc/set/get/release 管理，config 层自动拼 "a."/"c." 前缀
7. **ref_count 初始值为 1**：alloc 调用者隐式持有引用，必须显式 release 才能被 GC 回收
8. **const 表匹配不占 slab chunk**：匹配到常量后 slot->data 指向常量区，slot->type=CONST
9. **bus 拆分为三层**：kwcc_bus（纯 C Pub/Sub）+ kwcc_ui_bus（UI→JS 桥接）+ kwcc_base（topic 工具）
10. **bus → JS 白名单**：`bus/js_whitelist` config 控制，`*`=全部、`""`=不转发、逗号前缀=前缀匹配
11. **bus → JS action 固定 `notify_c`**：标识事件来自 C bus，区别于 UI 的 `click`/`change`
12. **Facade + Plugin 架构**：`kwcc_js` 作为 Facade 封装 mquickjs，子模块通过 `kwcc_js_ops_t` 操作 JS，不直接调 mquickjs API
13. **`kwcc_js_val_t = JSValue`**：行为层面隔离（可替换性），非类型层面。ops impl 直接用 JSValue，不需要强转
14. **`$notify` C→JS 通知通道**：C 端通过 `ops->notify_js` 通知，不直接调 resolve/reject；JS 端 `$notify.on(type, handler)` 做回调映射
15. **`ack_cleanup` 在 `call_cb` 之前自动调用**：C 端传了就自动处理，不需要记着调 release
16. **模块生命周期**：core 按 `load → (apis 自动注册) → on_bus_event（运行时）→ unload（退出时）` 顺序调用
17. **module-grouped 两级分发**：`kwcc_js_call_c(module, func, ...args)` 替代旧的 `kwcc_js_mquickjs_call`，分发表按 module 分组，同一 module 内按 func 查找 handler

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
| `loadJs(path)` | 加载 JS 文件，已加载则跳过 | Yes |
| `loadJs(path, once)` | `once=1`(默认)只加载一次；`once=0` 强制加载 | Yes |
| `registerModule(name, mod)` | 注册模块 state + actions + initEvents | Yes |
| `registerModuleView(name, renderFn)` | 注册模块 view 渲染函数 | Yes |
| `registerTopic(name, topics)` | 注册模块 topic 常量 | Yes |

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
