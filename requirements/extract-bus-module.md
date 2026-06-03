# 提取 EventBus + Topic 到 kwcc_bus 独立模块

## Context

当前 `kwcc_ui.c` 混了 C→JS 事件分发（`kwcc_dispatch_event`、`kwcc_bind_topic`、`g_topic_map`），但它本质上是通用的消息总线桥接服务，和 microui 完全无关。未来 `kwcc_io.c` 等任何 C 层模块都需要触发 JS 事件，不应依赖 `kwcc_ui.h`。

**为什么独立成模块**：
- `kwcc_js.c` 负责 JS 生命周期 + stdlib stubs
- EventBus dispatch 负责 C→JS 消息路由，和 JS 生命周期无关
- `kwcc_bus.h` = 纯消息总线声明，任何 C 模块 include 即可发事件

## 目标文件职责

| 文件 | 职责 | 不含 |
|------|------|------|
| `kwcc_bus.c` | C→JS 消息总线桥接：topic map + dispatch_event + bind_topic + frame 重置 | 无 microui、无 JS lifecycle |
| `kwcc_bus.h` | 消息总线声明 | 无 microui 类型 |

## 改动清单

### 1. `kwcc_bus.h` — 新建

```c
#ifndef KWCC_BUS_H
#define KWCC_BUS_H

#include "mquickjs/mquickjs.h"

void kwcc_dispatch_event(JSContext *ctx, const char *topic, const char *action);
void kwcc_bind_topic(int id, const char *topic);
void kwcc_bus_begin_frame(void);  /* reset topic map per frame */

#endif
```
- 纯声明，不含 microui 类型
- `kwcc_bind_topic(int id, ...)` — `mu_Id` 改为 `int`，通用 ID

### 2. `kwcc_bus.c` — 新建

**从 kwcc_ui.c 移入**：
- `#define TOPIC_MAP_SIZE 256`
- `static struct { int id; char topic[128]; } g_topic_map[TOPIC_MAP_SIZE];`
- `static int g_topic_map_count = 0;`
- `kwcc_bind_topic(int id, const char *topic)` — 存入 topic map
- `kwcc_dispatch_event(JSContext *ctx, const char *topic, const char *action)` — `$bus.emit(...)`
- `kwcc_bus_begin_frame(void)` — 重置 `g_topic_map_count = 0`

**依赖**：`mquickjs.h` + `llog.h`

### 3. `kwcc_ui.c` — 移除 dispatch 代码

- 删除 `TOPIC_MAP_SIZE`、`g_topic_map`、`g_topic_map_count`、`kwcc_bind_topic`、`kwcc_dispatch_event` 的定义
- `kwcc_begin_frame()` 中移除 `g_topic_map_count = 0`（已归 kwcc_bus）
- `js_ui_dispatch` 中 button/slider 的 dispatch 调用改为调外部函数 `kwcc_dispatch_event`
- `kwcc_on_window_close` 保留在 kwcc_ui.c，内部调 `kwcc_dispatch_event(g_js_ctx, title, "close")`
- include `kwcc_bus.h` 获取声明
- `kwcc_register_ui` 中需要调 `kwcc_bind_topic` 的地方改为调外部函数

**UI 调用层兼容（`mu_Id` → `int`）**：
- `mu_Id` 在 microui 中就是 `int`，所以 UI 层传参无需实际转换
- 但声明层已解耦，调用方需显式传 `int` 类型（如 `(int)wid` 保持意图清晰）
- 示例：`mu_Id wid = mu_get_id(&g_mu, title, strlen(title));` → `kwcc_bind_topic((int)wid, topic);`

### 4. `kwcc_ui.h` — 移除相关声明

- 移除 `kwcc_bind_topic` 声明（已归 kwcc_bus.h）

### 5. `kwcc.h` — umbrella header 新增

```c
#include "kwcc_bus.h"
```

## 执行步骤（按顺序）

### Step 1: 新建 `src/kwcc_bus.h`

内容：
```c
#ifndef KWCC_BUS_H
#define KWCC_BUS_H

#include "mquickjs/mquickjs.h"

void kwcc_dispatch_event(JSContext *ctx, const char *topic, const char *action);
void kwcc_bind_topic(int id, const char *topic);
void kwcc_bus_begin_frame(void);  /* reset topic map per frame */

#endif
```

### Step 2: 新建 `src/kwcc_bus.c`

从 `kwcc_ui.c` 移入以下内容：

1. `#define TOPIC_MAP_SIZE 256`
2. `static struct { int id; char topic[128]; } g_topic_map[TOPIC_MAP_SIZE];` — 注意：`mu_Id` 改为 `int`
3. `static int g_topic_map_count = 0;`
4. `kwcc_bind_topic(int id, const char *topic)` 函数（签名中 `mu_Id` 改为 `int`）
5. `kwcc_dispatch_event(JSContext *ctx, const char *topic, const char *action)` 函数（原 `static` → 改为公开）
6. `kwcc_bus_begin_frame(void)` 函数 — 只重置 `g_topic_map_count = 0`
7. includes: `stdio.h`, `string.h`, `mquickjs.h`, `llog.h`, `kwcc_bus.h`

### Step 3: 修改 `src/kwcc_ui.c`

**删除**：
- `#define TOPIC_MAP_SIZE 256` (line 29)
- `g_topic_map` 定义 (line 30-31)
- `g_topic_map_count` 定义 (line 31)
- `kwcc_bind_topic` 函数 (lines 85-92)
- `kwcc_dispatch_event` 函数 (lines 102-120)
- `kwcc_begin_frame` 函数 (lines 94-98) — 注意：其中 `g_sync_count = 0` 和 `g_win_top = 0` 保留在 kwcc_ui.c，仅 `g_topic_map_count = 0` 移除

**新增**：
- `#include "kwcc_bus.h"` （在已有 includes 区域添加）

**修改 kwcc_begin_frame**：
- 改为只包含 `g_sync_count = 0; g_win_top = 0;`
- 改为调用 `kwcc_bus_begin_frame()` 来重置 topic map
- 即：`kwcc_begin_frame` 内部调用 `kwcc_bus_begin_frame()`

**修改 kwcc_on_window_close**：
- 保留在 kwcc_ui.c，但 `kwcc_dispatch_event` 现在是外部函数（不再 static），直接调用即可

**修改 js_ui_dispatch 中 button/slider 的 dispatch**：
- `kwcc_dispatch_event(ctx, topic, "click")` 和 `kwcc_dispatch_event(ctx, topic, "change")` 调用保持不变（函数签名不变，只是从 static 变成了公开）

**修改 kwcc_register_ui 中 beginWindow 的 topic 处理**：
- 当前 beginWindow 分支 (line 304-344) 中没有调用 `kwcc_bind_topic`！topic 只是存到 `g_win_topics` 栈中用于窗口拦截。**实际 `kwcc_bind_topic` 当前没有被任何地方调用**。
- 所以 UI 层的改动只是 `include "kwcc_bus.h"` 即可。

### Step 4: 修改 `src/kwcc_ui.h`

- 无需修改（当前 ui.h 中没有声明 `kwcc_bind_topic` 或 `kwcc_dispatch_event`，这些是内部函数）
- 确认 `kwcc_begin_frame` 也不在公开头文件中（它是内部调用，在 `kwcc_process_js` 内调用）

### Step 5: 修改 `src/kwcc.h`

在 umbrella header 中新增：
```c
#include "kwcc_bus.h"
```

插入到 `kwcc_base.h` 之后（bus 是基础服务，应在 ui/js 之前 include）。

### Step 6: 修改 `Makefile`

- `MQJS_SRCS` 行添加 `src/kwcc_bus.c`
- 新增构建规则：
  ```makefile
  $(OBJ_DIR)/src/kwcc_bus.o: src/kwcc_bus.c src/kwcc_bus.h src/kwcc_base.h $(MQJS_HEADERS) | $(OBJ_DIR)/src
  	$(CC) $(CFLAGS) -c $< -o $@
  ```
- 更新 `kwcc_ui.o` 依赖：添加 `src/kwcc_bus.h`（替换掉不再需要的内部依赖）

## 验证

```bash
make clean && make && make run
```

编译通过，窗口正常显示，UI 控件可交互，button/slider 事件分发正常。

---

## 实施步骤总结

按以下顺序执行：

| 步骤 | 操作 | 文件 |
|------|------|------|
| 1 | 新建头文件 | `src/kwcc_bus.h` |
| 2 | 新建实现文件 | `src/kwcc_bus.c`（从 ui.c 迁移 topic map + dispatch + bind + bus_begin_frame）|
| 3 | 清理旧代码 + 引入新头文件 | `src/kwcc_ui.c` |
| 4 | 更新 umbrella header | `src/kwcc.h` |
| 5 | 更新构建配置 | `Makefile` |
| 6 | 编译验证 | `make clean && make && make run` |

**注意**：
- `kwcc_ui.c` 是唯一需要大幅改动的文件，其余都是新增或微小改动
- 当前 `kwcc_bind_topic` 没有被实际调用（仅声明），所以迁移后不影响运行时行为
- `kwcc_dispatch_event` 在 button/slider handler 中被调用，迁移后签名不变，调用点无需改
- `kwcc_on_window_close` 保留在 kwcc_ui.c，内部调 `kwcc_dispatch_event(g_js_ctx, title, "close")` 不变
- 关键：`kwcc_ui.c` 中 `kwcc_begin_frame()` 需要拆分：保留 `g_sync_count=0` + `g_win_top=0`，新增调用 `kwcc_bus_begin_frame()`

## 后续计划（本期不实施）

### topic map 暂保持死代码状态

当前 `g_topic_map` 和 `kwcc_bind_topic` 是预留能力，实际调用路径是 button/slider 直接传 topic 字符串到 `kwcc_dispatch_event`，不经过 ID 映射。本期只负责代码搬迁，**不激活** topic map。

**原始动机**：解决 microui 控件用 title 字符串生成 ID 的问题（窗口 X 关闭事件中 title ≠ topic 字符串的 bug）。后续实现 `beginWindow` 时自动调 `kwcc_bind_topic(window_id, topic)` → `on_window_close` 通过 ID 查真实 topic 再做 dispatch，即可修复该 bug。

## 关键设计决策

### topic map 的 ID 类型

- **原方案**：`mu_Id`（microui 内部类型）
- **新方案**：`int`（通用整型，任何模块都能用）
- **理由**：topic map 是通用分发服务，未来 `kwcc_io.c` 可能需要绑定 socket ID → topic，不应依赖 microui

### g_topic_map 的 set/get 关系

- `kwcc_bind_topic(id, topic)` — 注册：ID → topic 映射
- 消费方（如 `kwcc_on_window_close`）通过 ID 查找 topic 再 dispatch
- 这种 set/get 模式与 `$bus` 联合：C 层用 bind/dispatch，JS 层用 `$bus.on/emit`

### begin_frame 职责拆分

- `kwcc_ui.c`：重置 `g_win_top = 0`
- `kwcc_bus.c`：重置 `g_topic_map_count = 0`
- `main.m frame()`：按需调用各模块 begin_frame，或在 `kwcc_process_js` 内部统一聚合
