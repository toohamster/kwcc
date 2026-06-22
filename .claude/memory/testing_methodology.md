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

每次开发完成后必须清理：

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
