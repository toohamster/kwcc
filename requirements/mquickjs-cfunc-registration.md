# mquickjs C 函数注册方案

## 问题分析

### mquickjs 两阶段构建机制

**阶段 1（host tool）：**
- 编译 `mquickjs_build.c` + `mqjs_stdlib.c` 为 host 可执行文件
- 运行后生成 `mqjs_stdlib.h`（C 源码格式）

**阶段 2（target binary）：**
- 编译 `mquickjs.c` + `mqjs_stdlib.h` + 用户代码 → 最终二进制

### `JS_CFUNC_DEF` 的工作原理

```c
JS_CFUNC_DEF("print", 1, js_print)
```

宏定义（`mquickjs_build.h`）：
```c
#define JS_CFUNC_DEF(name, length, func_name) \
    { JS_DEF_CFUNC, name, { .func = { length, "0", "generic", #func_name } } }
```

**关键点：**
- `#func_name` 将 `js_print` **字符串化**为 `"js_print"`
- build-time 阶段只存储**字符串名**，不引用函数指针
- 但生成的 `js_c_function_table[]` 中会输出 `{ .generic = js_print }`
- 这是**真正的 C 函数指针**，链接时需要解析符号

### `JS_CFUNC_DEF` 与 `JS_CFUNC_SPECIAL_DEF` 的区别

| 宏 | 用途 | cproto_name |
|----|------|-------------|
| `JS_CFUNC_DEF(name, len, func)` | 普通全局函数 | `generic` |
| `JS_CFUNC_SPECIAL_DEF(name, len, proto, func)` | 特殊签名函数 | `proto` 必须是合法原型名 |

**合法原型名**：`generic`、`generic_params`、`constructor`、`f_f`、`ff`、`fff` 等（定义在 `mquickjs.h` 的 `JSCFunctionTypeEnum` 中）

**关键区别**：
- `JS_CFUNC_DEF` 通过 `define_props()` → `add_cfunc()` 自动进入 `js_c_function_table[]`
- `JS_CFUNC_SPECIAL_DEF` 是在 `js_c_function_decl[]` 中**手动预注册**，用于不在 `js_global_object[]` 中直接引用的函数（如 `bound`、`rectangle_closure_test`）
- **同一个函数不应该在两个地方都注册**，否则会产生重复条目

## 方案（已验证完整通过）

### 修改的文件（2 处）

#### 1. `deps/mquickjs/mqjs_stdlib.c`

**在 `js_global_object[]` 中添加 CONFIG_KWCC 条件块**：

```c
JS_CFUNC_DEF("print", 1, js_print),
#ifdef CONFIG_KWCC
    JS_CFUNC_DEF("kwcc_ui", 3, js_kwcc_ui),
#endif
```

**不要在 `js_c_function_decl[]` 中加 CONFIG_KWCC 块** — 会生成重复条目。

#### 2. `Makefile`

HOST_CFLAGS 加 `-DCONFIG_KWCC`：

```makefile
HOST_CFLAGS = -Wall -Wextra -I deps/mquickjs -I src -O2 -D_GNU_SOURCE -fno-math-errno -fno-trapping-math -DCONFIG_KWCC
```

### 同步修改的文件

- `src/mquickjs_stubs.c` — `js_print` 修复为处理所有类型（不再只判断 `JS_IsString`）
- `src/bridge.c` — `JS_Eval` 的 `input_len` 使用 `strlen()` 而非 `0`

### 不需要修改的文件

- `src/mquickjs_stubs.h` — `js_kwcc_ui` 声明已存在
- `src/kwcc_stdlib.c` — 不需要，可直接删除

### 错误记录（已避免）

**错误**：在 `js_c_function_decl[]` 中添加了 `JS_CFUNC_SPECIAL_DEF("kwcc_ui", 3, 0, js_kwcc_ui)`：

```c
// 错误 — 第3个参数 0 会导致编译失败
JS_CFUNC_SPECIAL_DEF("kwcc_ui", 3, 0, js_kwcc_ui )
```

**原因**：`0` 被 string 化为 `"0"`，`dump_cfuncs()` 生成：
```c
{ { .0 = js_kwcc_ui }, JS_CFUNC_0, 3, 0 }
```
- `.0` 无效 → "expression is not assignable"
- `JS_CFUNC_0` 未定义 → "undeclared identifier"

**根因**：`JS_CFUNC_DEF` 在 `js_global_object[]` 中已经注册了函数，`js_c_function_decl[]` 只用于不在 `js_global_object[]` 中引用的函数。

## MVP 验证结果（完整通过）

独立的 MVP 测试 (`tests/mvp/`) 仅验证 CONFIG_KWCC 函数注册机制：
- **不依赖** Sokol、microui、nanovg
- 仅使用 mquickjs core + stubs + log
- 测试文件：`tests/mvp/mvp.c`、`tests/mvp/test.js`、`tests/mvp/Makefile`

### 完整调用链路

```
[C] context created, stdlib = 0x10d1370a0
[C] loaded tests/mvp/test.js (705 bytes), executing...
=== mvp test started ===
[C callback] method=button, argc=1
  arg[0] = Hello
kwcc_ui('button','Hello') returned: 42        ← C 函数返回 42，JS 成功获取
[C callback] method=slider, argc=4
  arg[0] = volume
  arg[1] = 0.5
  arg[2] = 0
  arg[3] = 100
kwcc_ui('slider',...) returned: 42
math works: 7
array: 1,2,3,4
typeof kwcc_ui: function
=== mvp test passed ===
[C] JS result: undefined    ← 脚本最后一个表达式是 print()，返回 undefined（正常）
[C] mvp test passed
```

### 验证项汇总

| 测试项 | 结果 |
|--------|------|
| MVP 编译 (`make clean && make -f tests/mvp/Makefile`) | ✓ 通过 |
| 主项目编译 (`make clean && make`) | ✓ 通过，`kwcc` 962KB |
| `kwcc_ui` 在 JS 中注册为函数 | ✓ `typeof kwcc_ui: function` |
| JS → C 单参数传递 | ✓ `method=button, arg[0]=Hello` |
| JS → C 多参数传递 | ✓ `method=slider, argc=4` |
| C 返回值传回 JS | ✓ `returned: 42` |
| 基础 JS 功能 | ✓ math, array |

**结论**：`JS_CFUNC_DEF("kwcc_ui", 3, js_kwcc_ui)` 在 `js_global_object[]` 注册 → host tool 生成正确的 `js_c_function_table[]` → JS 端成功调用 C 函数。

## 已验证的事实

| 测试 | 结果 |
|------|------|
| `var ui = {};` 在 mquickjs 中 | ✓ 支持 |
| `ui.method = function() {}` | ✓ 支持 |
| `JS_Eval(ctx, code, strlen(code), "name", 0)` | ✓ 工作 |
| `JS_Eval(ctx, code, 0, "name", 0)` | ✗ 静默失败 |
| `make clean && make` 完整编译 | ✓ 通过 |
| MVP 独立测试（JS → C → JS 调用链路） | ✓ 通过 |

## 当前未解决的问题

`bridge_process_js` 中 `JS_Eval` 在 Sokol `frame()` 回调中崩溃（standalone 测试不崩溃）。需要测试编译后的 `kwcc` 二进制是否能正常运行。
