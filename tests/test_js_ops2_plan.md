# test_js_ops.c 测试计划（修正版）

> 重写 test_js_ops.c，验证 ops-signature + module-grouped dispatch 机制

## 初始化流程

1. `kwcc_mempool_init()` — 初始化内存池
2. `malloc(4MB)` + `JS_NewContext(mem_buf, 4MB, &js_stdlib)` — 创建 JS 上下文
3. `kwcc_js_ops_init(ctx)` — 初始化 ops 函数指针
4. `kwcc_js_register_modules(ops)` — 注入 $notify + 注册 core APIs（mempool_dump_stats/dump_all）

## 文件作用域定义（C 不允许嵌套函数）

- ack_cleanup 测试：static `g_kwcc_test_ack_called` + `g_kwcc_test_ack_id[64]` + `kwcc_test_ack_cleanup(const char *id)` 函数
- dispatch 测试：
  - `test_dispatch_echo(ops, argc, argv)`：set_str_prop(global, "kwcc_dispatch_argc", new_int32(argc))，返回 "dispatch_ok"
  - `test_dispatch_sum(ops, argc, argv)`：返回 `new_int32(to_int32(argv[0]) + to_int32(argv[1]))`
  - `test_dispatch_echo_v2(ops, argc, argv)`：返回 "dispatch_ok_v2"（用于覆盖测试）
  - `test_dispatch_apis[]`：包含 echo/sum/NULL
  - `test_dispatch_mod`：name="testmod", apis=test_dispatch_apis

## 9 个 Section

### [1] new_object + set_str_prop + get_str_prop（7点）

- **1a**: `new_object(ops)` 返回非 exception → `!ops->is_exception(obj)`
- **1b**: set_str_prop(obj, "greeting", new_string("hello"))，get_str_prop(obj, "greeting") 后 to_cstring == "hello"
- **1c**: get_str_prop(obj, "nonexistent") → `ops->is_undefined(result) == true`
- **1d**: 先 set "greeting"="hello"，再 set "greeting"="world"，get 后 to_cstring == "world"
- **1e**: set_str_prop(obj, "count", new_int32(42))，get 后 to_int32 == 42
- **1f**: get_str_prop(ops->global_obj, "JSON") → `!ops->is_undefined(result)`
- **1g**: obj1 设 x=1，obj2 不设 x，验证 obj1.x=1 且 obj2.x=undefined

### [2] new_int32 + to_int32（6点）

- **2a**: `to_int32(new_int32(0)) == 0`
- **2b**: `to_int32(new_int32(-99)) == -99`
- **2c**: `to_int32(new_int32(12345)) == 12345`
- **2d**: `to_int32(new_int32(2147483647)) == 2147483647` (INT32_MAX)
- **2e**: `to_int32(new_int32(-2147483648)) == -2147483648` (INT32_MIN)
- **2f**: `eval("'123'", 5, JS_EVAL_RETVAL)` 返回字符串，`to_int32(result) == 123`

### [3] new_string + new_string_len + to_cstring（7点）

- **3a**: `to_cstring(new_string("abc")) == "abc"` （< 5 字节，内联存储）
- **3b**: `to_cstring(new_string("This is a much longer string..."))` 返回完整字符串
- **3c**: `to_cstring(new_string_len("hello world", 5)) == "hello"`
- **3d**: `to_cstring(new_string("")) == ""`
- **3e**: `to_cstring(new_string_len("anything", 0)) == ""`
- **3f**: `to_cstring(new_int32(42)) == "42"` （JS 强转）
- **3g**: `to_cstring(new_string("X")) == "X"`

### [4] call_cb（6点）

**call_cb 签名是 void，无法直接获取返回值，必须通过全局变量中转验证。**

- **4a**: `eval("(function() { kwcc_cb_called = 1; })", JS_EVAL_RETVAL)` 返回函数，`call_cb(fn, 0, NULL)` 后 `get_str_prop(global, "kwcc_cb_called")` → `to_int32 == 1`
- **4b**: `eval("(function(x) { kwcc_cb_arg = x; })", JS_EVAL_RETVAL)` 返回函数，`call_cb(fn, 1, &argv[0])`（argv[0]=new_string("test_value")）后 `get_str_prop(global, "kwcc_cb_arg")` → `to_cstring == "test_value"`
- **4c**: `eval("(function() { kwcc_cb_ret = 'returned'; })", JS_EVAL_RETVAL)` 返回函数，`call_cb(fn, 0, NULL)` 后 `get_str_prop(global, "kwcc_cb_ret")` → `to_cstring == "returned"`
- **4d**: `eval("(function(){})", JS_EVAL_RETVAL)` 返回函数，`is_function(fn) == 1`
- **4e**: `is_function(new_int32(42)) == 0`
- **4f**: `eval("(function() { throw new Error('test'); })", JS_EVAL_RETVAL)` 返回函数，`call_cb(fn, 0, NULL)` 后验证引擎状态正常（后续 `eval("1+1", JS_EVAL_RETVAL)` 仍能执行且返回 2）

### [5] 类型判断 + get_class_id（7点）

**一致性要求：使用 ops->undefined/ops->null/ops->exception，不使用 JS_UNDEFINED/JS_NULL/JS_EXCEPTION 宏。**

- **5a**: `ops->is_undefined(ops->undefined) == true`
- **5b**: `ops->is_null(ops->null) == true`
- **5c**: `ops->is_exception(ops->exception) == true`
- **5d**: `eval("(function(){})", JS_EVAL_RETVAL)` 返回函数，`is_function(fn) == true`
- **5e**: `eval("[1,2,3]", 7, JS_EVAL_RETVAL)` 返回数组，`get_class_id(arr) == 1` (JS_CLASS_ARRAY)
- **5f**: `get_class_id(new_object(ops)) == 0` (JS_CLASS_OBJECT)
- **5g**: `get_class_id(ops->undefined) == -1`

### [6] eval（5点）

- **6a**: `eval("1 + 2", 5, JS_EVAL_RETVAL)` → `to_int32(result) == 3`
- **6b**: `eval("var kwcc_eval_global = 'hello';", 31, JS_EVAL_REPL)`，`get_str_prop(global, "kwcc_eval_global")` → `to_cstring == "hello"`
- **6c**: `eval("undefined_var;", 14, JS_EVAL_RETVAL)` → `is_exception(result) == true`
- **6d**: `eval("", 0, JS_EVAL_RETVAL)` → `!is_exception(result)`
- **6e**: `eval("'test_string'", 13, JS_EVAL_RETVAL)` → `to_cstring(result) == "test_string"`

### [7] notify_js C 端测试（5点）

**测试目标**：验证 ops->notify_js 的 C 端行为，不依赖 JS 端 $notify.on 注册。

- **7a**: `notify_js("testtype", "event1", "req_001", data, NULL)` → 不 crash（无 handler 注册时安全返回）
- **7b**: `notify_js("testtype", "event2", "req_002", data, kwcc_test_ack_cleanup)` → `g_kwcc_test_ack_called == 1` 且 `g_kwcc_test_ack_id == "req_002"`（ack_cleanup 在 call_cb 之前被调用）
- **7c**: `notify_js("unknowntype", "event3", "req_003", data, NULL)` → 不 crash（未注册 type 安全处理）
- **7d**: 连续调用 `notify_js` 两次，`g_kwcc_test_ack_called == 2`（多次调用安全）
- **7e**: `notify_js("testtype", "event4", NULL, data, kwcc_test_ack_cleanup)` → `g_kwcc_test_ack_called == 1`（NULL id 时 ack_cleanup 安全处理）

### [8] array_length + array_get（6点）

- **8a**: `eval("[10, 20, 30]", 13, JS_EVAL_RETVAL)` 返回数组，`array_length == 3`，`array_get(0/1/2)` → `to_int32 == 10/20/30`
- **8b**: `eval("[]", 2, JS_EVAL_RETVAL)` 返回空数组，`array_length == 0`，`array_get(0)` → `is_undefined == true`
- **8c**: `eval("['foo', 'bar']", 15, JS_EVAL_RETVAL)` 返回字符串数组，`array_length == 2`，`array_get(0)` → `to_cstring == "foo"`
- **8d**: `array_get(arr, 99)` → `is_undefined == true`
- **8e**: `eval("[1, 'two', true]", 17, JS_EVAL_RETVAL)` 返回混合数组，`array_get(0)` → `to_int32 == 1`，`array_get(1)` → `to_cstring == "two"`
- **8f**: `get_class_id(arr) == 1` (JS_CLASS_ARRAY)

### [9] dispatch: kwcc_js_call_c + kwcc_js_dispatch_call（9点）

**前置条件：`kwcc_js_register_module(ops, &test_dispatch_mod)` 注册 testmod。**

**JS 层 dispatch（通过 eval 调 kwcc_js_call_c）：**

- **9a**: `eval("kwcc_js_call_c('testmod', 'echo', 'hello');", JS_EVAL_REPL)`，handler 设 `kwcc_dispatch_argc=1`，`kwcc_dispatch_arg0='hello'`
- **9b**: `eval("var kwcc_dispatch_result = kwcc_js_call_c('testmod', 'echo', 'test');", JS_EVAL_REPL)`，`get_str_prop(global, "kwcc_dispatch_result")` → `to_cstring == "dispatch_ok"`
- **9c**: `eval("kwcc_js_call_c('testmod', 'echo', 'a', 'b', 'c');", JS_EVAL_REPL)`，`kwcc_dispatch_argc == 3`
- **9d**: `eval("var kwcc_unknown_mod = kwcc_js_call_c('nonexistent', 'echo');", JS_EVAL_REPL)`，`get_str_prop(global, "kwcc_unknown_mod")` → `is_undefined == true`
- **9e**: `eval("var kwcc_unknown_fn = kwcc_js_call_c('testmod', 'nonexistent');", JS_EVAL_REPL)`，`get_str_prop(global, "kwcc_unknown_fn")` → `is_undefined == true`
- **9f**: `eval("kwcc_js_call_c('core', 'mempool_dump_stats');", JS_EVAL_REPL)` → `!is_exception(result)`

**C 端 dispatch（直接调 kwcc_js_dispatch_call）：**

- **9g**: `kwcc_js_dispatch_call("testmod", "sum", 2, sum_args)`（sum_args[0]=new_int32(10), sum_args[1]=new_int32(20)）→ `to_int32(result) == 30`
- **9h**: `kwcc_js_dispatch_call("testmod", "echo", 1, echo_args)`（echo_args[0]=new_string("direct_call")）→ `to_cstring(result) == "dispatch_ok"`

**重复注册行为：**

- **9i**: `kwcc_js_dispatch_add("testmod", "echo", test_dispatch_echo_v2)`，`kwcc_js_dispatch_call("testmod", "echo", 1, dup_args)` → `to_cstring(result) == "dispatch_ok_v2"`

## 已确认项

- [x] kwcc_js_dispatch_add 重复注册同名 module+func → **覆盖旧 handler**
- [x] kwcc_js_dispatch_call 暴露为非 static → **已在 kwcc_js.h 声明**
- [x] kwcc_js_dispatch_add 暴露为非 static → **已在 kwcc_js.h 声明**
- [x] call_cb 异常捕获 → **不变**（记录日志 + 清除异常）
- [x] call_cb 是 void → **通过全局变量中转验证返回值**
- [x] 类型判断用 ops->undefined/null/exception → **保持一致性，不用宏**

## 总计 60 个测试点

## 编译命令

```bash
gcc -Wall -I. -Ideps -D_GNU_SOURCE -DCONFIG_KWCC \
    -o tests/bin/test_js_ops tests/test_js_ops.c \
    build/obj/deps/mquickjs/mquickjs.o \
    build/obj/deps/mquickjs/cutils.o \
    build/obj/deps/mquickjs/dtoa.o \
    build/obj/deps/mquickjs/libm.o \
    build/obj/src/kwcc_mempool.o \
    build/obj/src/kwcc_config.o \
    build/obj/src/kwcc_js.o \
    build/obj/src/kwcc_bus.o \
    build/obj/src/kwcc_base.o \
    build/obj/src/kwcc_io.o \
    build/obj/src/kwcc_http.o \
    build/obj/deps/log/log.o \
    build/obj/deps/picohttpparser/picohttpparser.o
```
