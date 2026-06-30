# 测试方法论 — C 端优先测试原则

## 核心原则

**C 端的功能尽量在 C 端测试验证，JS 端验证不方便。**

原因：
- C 端测试：直接调用函数 + assert，秒级出结果
- JS 端测试：需要完整编译 + 启动窗口 + 等 JS 执行，调试信息少

## 分层测试策略

### 第一层：纯 C 测试（tests/test_mempool.c, test_config.c）
- **测试对象**：mempool 层、config 层等纯 C 模块
- **方式**：直接调用 C 函数 + assert 验证
- **优点**：编译快（不链接 UI/microui/nanovg）、调试方便、断言清晰
- **示例**：`tests/test_mempool.c`（46 测试）、`tests/test_config.c`（19 测试）

### 第二层：C handler 测试（tests/test_config_js.c）
- **测试对象**：JS C handler（`kwcc_js_config_set_app_*` 等），验证 JSValue → C 值 → C 层 → mempool 的完整链路
- **方式**：创建 JSContext + 构造 JSValue + 调用 C handler + 验证结果
- **关键**：不需要链接 kwcc_ui.o（避免 nanosvg/NVG 依赖），只需 mquickjs core + mempool + config + kwcc_js
- **示例**：`tests/test_config_js.c`

### 第 2.5 层：ops 接口测试（tests/test_js_ops.c）
- **测试对象**：`kwcc_js_ops_t` 的每个函数指针，作为 Facade 层的 ABI 契约测试
- **方式**：创建独立 JSContext → 调用 `kwcc_js_ops_init` → 通过 ops 操作 JS → 验证结果
- **关键**：在子模块迁移前验证 core 本身正确，避免问题交叉
- **测试内容**：8 大类（值创建/属性操作/函数调用/类型判断/C字符串/eval/notify/数组），75 个测试点
- **重要发现**：`get_class_id` 测试需在创建对象的同一作用域内验证，跨作用域读变量会得到 undefined（返回 -1）

### 第三层：集成测试（JS 脚本 + kwcc 运行）
- **测试对象**：JS wrapper → C handler → C 层 → mempool 端到端
- **方式**：在 app/main.js 中 load 测试脚本 + `make run`
- **缺点**：调试困难，只能用 print/log 排查
- **用途**：仅验证完整链路是否正常，不用于定位问题

## 排查问题的正确流程

1. **先在 C handler 测试中复现问题**（test_config_js.c）
2. **如果 C handler 测试通过** → 问题在 JS wrapper
3. **如果 C handler 测试失败** → 用 lldb 或直接加 log 定位 C handler 内部
4. **如果底层 C 函数有问题** → 写纯 C 测试验证（test_mempool.c / test_config.c）
5. **不要一上来就在 JS 层反复调试**

## 本次教训（Phase 4 TLV 路径查询失败）

**错误做法**：
- 反复改 JS wrapper → 编译 → 运行 → 看日志，浪费 2 小时
- 在 JS 测试脚本里加 print 验证，信息不完整

**正确做法**：
- 写 C handler 测试直接调 `kwcc_js_config_set_app_tlv` + `kwcc_js_config_get_app_tlv_path`
- 5 分钟内定位到 `kwcc_js_value_to_tlv` 返回 NULL
- 再用 lldb/log 定位到 `JS_GetClassID` 返回 0（Object 类未注册）
- 改用 `js_object_keys` 内部 API 解决

**总结**：每一层的问题就在这一层验证，不要跨层调试。

## 编译技巧

C handler 测试**不要链接 kwcc_ui.o**，它会引入 nanosvg/NVG/microui 等大量不需要的依赖。

**测试二进制统一输出到 `tests/bin/` 目录**，已在 `.gitignore` 中配置：
```
tests/bin/
tests/*.o
```

不要在 `tests/` 根目录直接输出二进制文件，避免误提交到 git。

## 清理规则

**铁律（必须严格遵守）**：
- **构建时 `src/` 和 `deps/` 目录出现的变更，一律不能重置，必须加到版本控制里**
- `mqjs_stdlib.h` 是构建产物，但必须保留并 git add
- 绝对不要对 `src/` 和 `deps/` 下的任何文件执行 `git checkout --`

每次开发完成后：

1. **构建产物分类处理**：
   - `mqjs_stdlib.h` — 构建产物但需要保留（后续编译依赖），**可以进 git**
   - `build/` 目录 — Makefile 已忽略，不进 git
   - 最终二进制 `kwcc` — `.gitignore` 已配置

2. **测试二进制**：
   - `tests/test_*` 可执行文件 — 调试用的，编译完就删或放 `tests/bin/`
   - **绝对不能进 git**
   - `.gitignore` 已配置：`tests/bin/` + `tests/*.o`

3. **临时测试脚本**：
   - 开发过程中创建的临时 .c/.js 测试文件，评估后决定保留或删除
   - 如果后续还要用就保留，否则删除

4. **每次 commit 前检查**：
   - `git status` 看有没有 .o、可执行文件、不需要的中间文件混入
   - 确认 `.gitignore` 覆盖完整

## 错误案例：命名规范遵守

**错误**：在实现 dump 功能时，C handler 命名为 `kwcc_js_config_dump` / `kwcc_js_dump_stats`，没有体现功能归属。

**原因**：没有先思考函数名应该体现什么逻辑，随意命名。注册表里也用了 `kwcc_js_config_dump` 而不是 `kwcc_js_mempool_dump_stats`。

**正确做法**：
- C handler 在 `kwcc_js.c` 中，命名 `kwcc_js_<功能>_<具体操作>`
- dump 是 mempool 的调试功能 → `kwcc_js_mempool_dump_stats` / `kwcc_js_mempool_dump_all`
- 注册表名字与 C handler 一致：`{ "kwcc_js_mempool_dump_stats", kwcc_js_mempool_dump_stats }`

**教训**：命名必须先想清楚功能归属和逻辑含义，不能随意缩写或泛化。每次新加函数名都要对照命名规范检查。

## 测试记录

每个功能开发完成后，**必须更新对应的 `tests/TESTING_*.md`**：
- 按功能模块分文件：`TESTING_MEMPOOL.md`、`TESTING_NETWORK.md` 等
- 列出方案中定义的所有测试点
- 标注每个测试点的状态（✅ 通过 / ❌ 失败 / ⏳ 待测试）
- 注明测试类型（纯 C / C handler / JS 集成）和测试文件
- 让其他人能清楚看到做了什么、测了什么、还有什么没测
- 新增功能模块时，新建对应的 `TESTING_XXX.md` 文件

需要的 .o 文件清单：
```
mquickjs.o cutils.o dtoa.o libm.o    # mquickjs core
kwcc_mempool.o kwcc_config.o          # C 层
kwcc_js.o kwcc_bus.o kwcc.o           # JS handler
kwcc_io.o kwcc_base.o kwcc_http.o     # I/O + HTTP + base
log.o                                 # 日志
```

编译命令模板：
```bash
gcc -Wall -I. -Ideps -D_GNU_SOURCE -DCONFIG_KWCC \
    -o tests/test_config_js tests/test_config_js.c \
    build/obj/deps/mquickjs/mquickjs.o \
    build/obj/deps/mquickjs/cutils.o \
    build/obj/deps/mquickjs/dtoa.o \
    build/obj/deps/mquickjs/libm.o \
    build/obj/src/kwcc_mempool.o \
    build/obj/src/kwcc_config.o \
    build/obj/src/kwcc_js.o \
    build/obj/src/kwcc_bus.o \
    build/obj/src/kwcc.o \
    build/obj/deps/log/log.o
```

## 断言宏模板

```c
#define TEST(name, cond) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", #name); } \
    else { tests_failed++; printf("  FAIL: %s\n", #name); } \
} while (0)
```

简单、一致、可读性好。不要用复杂的 ASSERT 宏（容易出宏展开问题）。

## 测试编写规范

### 测试点必须自包含

**每个测试点应创建和使用自己的数据，不依赖其他测试点通过 eval 创建的全局变量。**

错误示例：
```c
// test 8: 通过 eval 创建全局变量
ops->eval(ops, "var kwcc_test_arr = [1,2,3];", ...);

// test 5: 假设 kwcc_test_arr 已存在 → 读到 undefined → JS_GetClassID 返回 -1
kwcc_js_val_t arr = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_arr");
int cid = ops->get_class_id(ops, arr);  // -1，不是 1
```

正确做法：
```c
// test 5: 在本测试点内创建自己的数组
kwcc_js_val_t arr = ops->eval(ops, "[1,2,3]", ...);
int cid = ops->get_class_id(ops, arr);  // 1 (JS_CLASS_ARRAY)
```

**原因**：
- 测试点执行顺序可能调整，依赖顺序 = 隐式耦合
- `JS_Eval` 创建的全局变量依赖于 JS_EVAL_REPL 标志和执行时机
- 读取未创建的属性返回 undefined，`JS_GetClassID(undefined)` 返回 -1，不是 0 或 1

### eval 测试值时用 JS_EVAL_RETVAL 获取返回值

```c
// ✅ 正确：eval 返回最后一个表达式的值
kwcc_js_val_t arr = ops->eval(ops, "[1,2,3]", 7, "<test>", JS_EVAL_RETVAL);

// ❌ 错误：JS_EVAL_REPL 不返回值，需要通过 get_str_prop 读取全局变量
ops->eval(ops, "var arr = [1,2,3];", 16, "<test>", JS_EVAL_REPL);
kwcc_js_val_t arr = ops->get_str_prop(ops, ops->global_obj, "arr");  // 多一步间接
```
