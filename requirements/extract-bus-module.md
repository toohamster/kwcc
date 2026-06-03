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

### 6. `main.m` — 调用链更新

- `frame()` 中在 `kwcc_process_js` 前加 `kwcc_bus_begin_frame()` 调用
- 或在 `kwcc_process_js` 内部统一调用双方 begin_frame

### 7. Makefile

- 新增 `kwcc_bus.o` 到链接列表
- 依赖：`kwcc_bus.c` → `kwcc_bus.h` + `mquickjs.h` + `llog.h`

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
