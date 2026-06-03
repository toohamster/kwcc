# 三段式 Slab 内存池实施计划

## Context

内存池方案已在 `requirements/core-memory-pool.md` 中完成详细设计。现在开始编码实施。目标：创建独立 `kwcc_pool.c/h` 模块，注入 `$config` JS API，迁移 `__kwcc_config` 到 App 池。

**重要知识（来自记忆文件）**：
- mquickjs 是 ES5 语法，不支持 `let/const/箭头函数/...展开`，JS 侧必须用 `var` 和 `new Object()`
- mquickjs C API：JSValue 是 `uint64_t` 值类型，无 `JS_FreeValue`，GC 用 `JS_PushGCRef/JS_AddGCRef`
- `JS_ToCString` 可能返回 NULL，必须检查
- `{}` 在语句开头被解析为 block，JS 侧用 `new Object()` 或括号强制
- mquickjs 不支持 `...rest` 展开参数，JS 侧传固定参数
- `kwcc_register_ui` 中通过 `JS_Eval` 注入 JS wrapper 到 `kwcc_ui.c:564`
- C 函数注册通过 `CONFIG_KWCC` + `JS_CFUNC_DEF` 在 `mqjs_stdlib.c` 中
- macOS 10.14 编译：`.m` 扩展名、`-fobjc-arc`、`SOKOL_GLCORE`
- 日志用 `llog.h`，不是直接 `log.h`（syslog.h 宏冲突）

## 关键源文件

| 文件 | 作用 | 本次改动 |
|------|------|---------|
| `src/kwcc_pool.h` | 新建 | 类型定义 + API 声明 |
| `src/kwcc_pool.c` | 新建 | 核心实现 |
| `src/kwcc.h` | umbrella header | 加 `#include "kwcc_pool.h"` |
| `src/kwcc_js.c` | JS lifecycle | 添加 `$config` JS API 注入 + C handler |
| `src/kwcc_ui.c` | UI bridge | 在 `kwcc_register_ui` 中加 `$config` JS wrapper |
| `src/main.m` | Sokol lifecycle | `init()` 加 `kwcc_mem_init_defaults()`，`cleanup()` 加 `kwcc_mem_shutdown()` |
| `src/kwcc_base.h` | base infra | 添加 `KWCC_DEBUG` 宏定义 + 池大小默认宏 |
| `app/main.js` | JS entry | 加 `$config.setAppSize/setUserSize` 调用 |
| `Makefile` | build | 加 `kwcc_pool.c` 编译规则 |
| `deps/mquickjs/mqjs_stdlib.c` | stdlib | 加 `$config` C 函数注册（CONFIG_KWCC） |

## 实施步骤（按顺序执行）

### Step 1: 新建 `src/kwcc_pool.h`

定义所有公开类型和 API：
- `kwcc_mem_pool_t`、`kwcc_slab_t`、`kwcc_slot_t` 结构体
- `kwcc_runtime_spec_t` 配置结构体
- `KWCC_NUM_SIZE_CLASSES` 常量 = 3
- 全局声明：`g_core_pool`、`g_app_pool`、`g_user_pool`
- 公开 API：`kwcc_mem_init`/`kwcc_mem_init_defaults`/`kwcc_mem_shutdown`/`kwcc_pool_configure`
- 槽位操作：`kwcc_pool_alloc`/`acquire`/`release`/`get`/`set`/`invalidate`
- GC：`kwcc_pool_gc`/`gc_force`/`gc_auto`
- 调试（`#ifdef KWCC_DEBUG`）：`kwcc_pool_dump_stats`/`kwcc_pool_dump_all`

### Step 2: 新建 `src/kwcc_pool.c`

实现所有核心逻辑（参照 `requirements/core-memory-pool.md` 中的代码片段）：
- 跨平台内存对齐：`kwcc_pool_aligned_malloc/free`
- Slab 隐式链表：`kwcc_pool_slab_alloc/slab_free/slab_init`
- 初始化：`kwcc_pool_init_internal` → `kwcc_mem_init_defaults` / `kwcc_mem_init` / `kwcc_pool_configure`
- 分配：`kwcc_pool_alloc`（遍历 slabs 找最小适配桶）
- 引用计数：`kwcc_pool_acquire/release`
- 查找：`kwcc_pool_get`（FNV-1a + strcmp）
- 写入：`kwcc_pool_set`
- 主动作废：`kwcc_pool_invalidate`
- GC：`kwcc_pool_gc`/`gc_force`/`gc_auto`/`kwcc_pool_gc_internal`
- 内部释放：`kwcc_pool_free_slot`/`kwcc_pool_force_invalidate`
- 哈希：`kwcc_pool_fnv1a()`
- 调试：`kwcc_pool_dump_stats`/`kwcc_pool_dump_all`（`#ifdef KWCC_DEBUG`）
- 清理：`kwcc_mem_shutdown`

### Step 3: 更新 `src/kwcc.h`

添加 `#include "kwcc_pool.h"` 到 umbrella header（在 `kwcc_base.h` 之后）。

### Step 4: 更新 `src/kwcc_base.h`

在文件末尾添加：
```c
/* ── Memory pool compile-time defaults ── */
#ifndef KWCC_CORE_SIZE
#define KWCC_CORE_SIZE  (32 * 1024)
#endif
#ifndef KWCC_APP_SIZE
#define KWCC_APP_SIZE   (256 * 1024)
#endif
#ifndef KWCC_USER_SIZE
#define KWCC_USER_SIZE  (1 * 1024 * 1024)
#endif
#ifndef KWCC_DEBUG
#define KWCC_DEBUG 0
#endif
```

### Step 5: 更新 `src/kwcc_js.c`

在 `kwcc_create_js()` 中，`kwcc_config_set_jsctx()` 调用之后添加：
- 注册 `$config` 全局 JS 对象（通过 `JS_GetGlobalObject` + `JS_SetPropertyStr`）
- JS wrapper 方法：`setApp/setUser/getApp/getUser/releaseApp/releaseUser/setAppSize/setUserSize/dump/dumpAll`
- 新增 C handler 函数（static）：`js_config_set_app`、`js_config_set_user`、`js_config_get_app`、`js_config_get_user`、`js_config_release_app`、`js_config_release_user`、`js_config_set_app_size`、`js_config_set_user_size`、`js_config_dump`、`js_config_dump_all`

**注意**：mquickjs 不支持 `...rest`，每个 JS 方法必须传固定参数。错误处理：分配失败返回 `undefined` + `console.warn`。

### Step 6: 更新 `src/kwcc_ui.c`（`kwcc_register_ui` 中）

在现有的 `methods_js` 字符串（kwcc_ui.c:564）末尾追加 `$config` 对象的 JS wrapper 代码。格式参照现有的 `ui.button` 等写法。

### Step 7: 更新 `src/main.m`

- `init()`: 在 `kwcc_create_js()` 之前（main.m:176 之前）调用 `kwcc_mem_init_defaults()`
- `cleanup()`: 在 `kwcc_ui_free()` 之后调用 `kwcc_mem_shutdown()`

### Step 8: 更新 `app/main.js`

在文件开头（load runtime 之后，load modules 之前）添加：
```javascript
$config.setAppSize(256 * 1024);
$config.setUserSize(1 * 1024 * 1024);
```

### Step 9: 更新 `Makefile`

- `MQJS_SRCS` 添加 `src/kwcc_pool.c`
- 新增 `$(OBJ_DIR)/src/kwcc_pool.o` 编译规则

### Step 10: 编译验证

```bash
make clean && make && make run
```

## 关键设计决策

- **命名规范**：所有函数 `kwcc_pool_{action}` 三段式，内部函数 `static`
- **ref_count**：`uint16_t`，`alloc()` 时 = 0，`acquire()` 时++，溢出时打 error 忽略
- **GC 80% 阈值**：`kwcc_pool_gc_auto()` 自动检测，超过立即强制 GC
- **JS API 一致性**：`setApp/setUser(key, null)` 统一为释放
- **mquickjs ES5 限制**：JS wrapper 必须用 `var`，不能用 `let/const`，不能用展开参数
- **JS_ToCString NULL 保护**：所有字符串参数必须 NULL 检查

## 验证

- 编译通过（`make clean && make`）
- 窗口正常显示（`make run`）
- `$config.setApp("test", "hello")` / `$config.getApp("test")` 正常工作
- `$config.dump()` 输出池概要
- `$config.setAppSize()` 在 main.js 中可配置 App 池
- 检查 `kwcc.log` 无错误日志
