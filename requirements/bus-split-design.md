# 方案：kwcc_bus 重构 — 分离 UI 桥接与通用事件总线

> 状态：待论证
> 优先级：最高（async-io HTTP 模块前置依赖）
> 创建于 2026-06-23

## 背景

### 当前问题

`kwcc_bus.c`（52 行）混杂了两个不相关的职责：

| 职责 | 实际归属 | 问题 |
|------|---------|------|
| topic map（ID→topic 映射） | UI 层内部 | 放在 bus 里，bus 不应该是 UI 专属 |
| JS dispatch（`kwcc_dispatch_event`） | UI→JS 桥接 | 函数名不遵守命名规范，include 了 mquickjs |

**当前 `kwcc_bus.c` 不是真正的 bus**，它只是 UI 层到 JS 层的事件桥接器。

### 目标

拆成两个独立的东西：

```
1. kwcc_ui_bus.c/h — UI→JS 事件桥接（独立文件）
   → 负责：topic map + JS_Eval("$bus.emit(...)")
   → 遵守命名规范：kwcc_ui_bus_*

2. kwcc_bus.c/h — 通用 C 事件总线（独立基础设施）
   → 像 kwcc_mempool 一样，零外部依赖，纯 C Pub/Sub
   → 负责：C 模块间的事件解耦
   → 不感知 JS，不感知 UI
```

---

## 架构

```
┌──────────────────────────────────────────────────────────┐
│  kwcc_ui_bus.c/h（UI→JS 事件桥接，独立文件）                  │
│  kwcc_ui_bus_bind_topic(widget_id, "ui/calc/btn0")               │
│  kwcc_ui_bus_dispatch_event("ui/calc/btn0", "click")             │
│    → JS_Eval("$bus.emit('ui/calc/btn0', 'click', ...)")            │
├──────────────────────────────────────────────────────────┤
│  kwcc_bus.c/h（纯 C Pub/Sub，独立基础设施）                    │
│  subscriber 链表（topic 组 + 组内哈希回调表）                     │
│  subscribe / unsubscribe / publish                                │
│  零外部依赖，不 include mquickjs                                │
├──────────────────────────────────────────────────────────┤
│  kwcc_js.c（JS 桥接层）                                    │
│  作为 bus consumer：kwcc_bus_subscribe("*", js_cb, ctx)          │
│  收到 bus 事件 → 构建 JSValue → JS_Eval $bus.emit(...)            │
└──────────────────────────────────────────────────────────┘
        ↑ 任何 C 模块都能直接用
        │
┌──────────────────────────────────────────────────────────┐
│  kwcc_http.c（未来，bus consumer）                         │
│  kwcc_bus_publish("http/end/req_1", &resp, ...)                  │
│  topic 由 HTTP 模块内部管理，JS 侧无感知                            │
└──────────────────────────────────────────────────────────┘
```

### 关键变化

| 旧 | 新 |
|----|-----|
| `kwcc_bus.c` 包含 topic map + JS dispatch | topic map 移到 `kwcc_ui_bus.c`，JS dispatch 移到 `kwcc_ui_bus.c` |
| `kwcc_dispatch_event` | `kwcc_ui_bus_dispatch_event`（遵守命名规范） |
| bus include mquickjs | bus 零依赖，JS 回调是 consumer 注册的 |
| 固定数组上限 | 链表实现，topic 组数量无上限 |

---

## 第一部分：UI→JS 桥接（`kwcc_ui_bus.c/h`，新建独立文件）

### 头文件（`src/kwcc_ui_bus.h`）

```c
#ifndef KWCC_UI_BUS_H
#define KWCC_UI_BUS_H

void kwcc_ui_bus_set_js_ctx(void *ctx);
void kwcc_ui_bus_begin_frame(void);
void kwcc_ui_bus_bind_topic(int id, const char *topic);
void kwcc_ui_bus_dispatch_event(const char *topic, const char *action);

#endif
```

### 实现（`src/kwcc_ui_bus.c`）

```c
/* kwcc_ui_bus.c — UI→JS 事件桥接 */
#include "kwcc_ui_bus.h"
#include "mquickjs/mquickjs.h"
#include "llog.h"

#define KWCC_UI_TOPIC_MAP_SIZE 256
static struct { int id; char topic[128]; } g_kwcc_ui_topic_map[KWCC_UI_TOPIC_MAP_SIZE];
static int g_kwcc_ui_topic_count = 0;
static JSContext *g_kwcc_ui_bus_js_ctx = NULL;

void kwcc_ui_bus_set_js_ctx(void *ctx) {
    g_kwcc_ui_bus_js_ctx = (JSContext *)ctx;
}

void kwcc_ui_bus_begin_frame(void) {
    g_kwcc_ui_topic_count = 0;
}

void kwcc_ui_bus_bind_topic(int id, const char *topic) {
    if (g_kwcc_ui_topic_count < KWCC_UI_TOPIC_MAP_SIZE && topic) {
        g_kwcc_ui_topic_map[g_kwcc_ui_topic_count].id = id;
        strncpy(g_kwcc_ui_topic_map[g_kwcc_ui_topic_count].topic, topic, 127);
        g_kwcc_ui_topic_map[g_kwcc_ui_topic_count].topic[127] = '\0';
        g_kwcc_ui_topic_count++;
    }
}

void kwcc_ui_bus_dispatch_event(const char *topic, const char *action) {
    if (!g_kwcc_ui_bus_js_ctx || !topic) return;
    char buf[512];
    /* topic 和 action 转义 */
    snprintf(buf, sizeof(buf), "$bus.emit('%s', '%s', new Object());",
             topic, action ? action : "");
    JS_Eval(g_kwcc_ui_bus_js_ctx, buf, strlen(buf), "<ui_bus_dispatch>", 0);
}
```

### 调用方改动

```c
/* kwcc_ui.c — 按钮点击 handler */
kwcc_ui_bus_dispatch_event(topic, "click");
/* 旧：kwcc_dispatch_event(ctx, topic, "click"); */

/* main.m — 每帧开始 */
kwcc_ui_bus_begin_frame();
/* 旧：kwcc_bus_begin_frame(); */
```

---

## 第二部分：通用 C 事件总线（`kwcc_bus.c/h`，重写）

### 设计原则

- 像 `kwcc_mempool` 一样，是独立的 C 基础设施
- 零外部依赖：不 include mquickjs，不 include microui
- 不感知业务：不知道 UI，不知道 JS，不知道 HTTP
- 纯 Pub/Sub：subscribe → publish → unsubscribe

### 数据结构

subscriber 链表中的每个节点是一个 **topic 组**，组内用固定大小数组管理回调：

```c
/* 每个 topic 组内的回调条目 */
#define KWCC_BUS_GROUP_MAX_CB 16

typedef struct {
    uint64_t        id;         /* sub_id，唯一标识 */
    kwcc_bus_cb_t   cb;
    void           *user_data;
    int             in_use;
} kwcc_bus_cb_entry_t;

/* topic 组节点 */
typedef struct kwcc_bus_group {
    char                   *topic;      /* topic 模式（精确/前缀/*）*/
    kwcc_bus_cb_entry_t     callbacks[KWCC_BUS_GROUP_MAX_CB];
    int                     cb_count;   /* 已使用的回调数 */
    struct kwcc_bus_group  *next;
} kwcc_bus_group_t;
```

### API

```c
/* kwcc_bus.h */
#ifndef KWCC_BUS_H
#define KWCC_BUS_H

#include <stddef.h>

typedef uint64_t kwcc_bus_sub_id_t;
typedef void (*kwcc_bus_cb_t)(const char *topic, const void *data,
                               size_t len, void *user_data);

void            kwcc_bus_init(void);
kwcc_bus_sub_id_t kwcc_bus_subscribe(const char *topic, kwcc_bus_cb_t cb, void *user_data);
void            kwcc_bus_unsubscribe(kwcc_bus_sub_id_t id);
void            kwcc_bus_publish(const char *topic, const void *data, size_t len);

#endif
```

### 核心实现

```c
static kwcc_bus_group_t  *g_kwcc_bus_head = NULL;
static uint64_t           g_kwcc_bus_next_id = 1;

/* 匹配规则：精确 / * 通配 / 前缀 */
static int match(const char *pattern, const char *topic) {
    if (strcmp(pattern, topic) == 0) return 1;
    if (strcmp(pattern, "*") == 0) return 1;
    size_t plen = strlen(pattern);
    if (plen > 0 && pattern[plen-1] == '/' && strncmp(pattern, topic, plen) == 0)
        return 1;
    return 0;
}

/* 订阅：找到或创建 topic 组，添加回调 */
kwcc_bus_sub_id_t kwcc_bus_subscribe(const char *topic, kwcc_bus_cb_t cb, void *user_data) {
    /* 1. 找已有的 topic 组 */
    kwcc_bus_group_t *grp = g_kwcc_bus_head;
    while (grp) {
        if (strcmp(grp->topic, topic) == 0) break;
        grp = grp->next;
    }

    /* 2. 没有就创建新组 */
    if (!grp) {
        grp = calloc(1, sizeof(*grp));
        grp->topic = strdup(topic);
        grp->next = g_kwcc_bus_head;
        g_kwcc_bus_head = grp;
    }

    /* 3. 在组内找空位 */
    for (int i = 0; i < KWCC_BUS_GROUP_MAX_CB; i++) {
        if (!grp->callbacks[i].in_use) {
            grp->callbacks[i].id = g_kwcc_bus_next_id;
            grp->callbacks[i].cb = cb;
            grp->callbacks[i].user_data = user_data;
            grp->callbacks[i].in_use = 1;
            grp->cb_count++;
            return g_kwcc_bus_next_id++;
        }
    }
    return 0;  /* 组内满了 */
}

/* 取消订阅：找到 sub_id，标记 inactive */
void kwcc_bus_unsubscribe(kwcc_bus_sub_id_t id) {
    for (kwcc_bus_group_t *grp = g_kwcc_bus_head; grp; grp = grp->next) {
        for (int i = 0; i < KWCC_BUS_GROUP_MAX_CB; i++) {
            if (grp->callbacks[i].in_use && grp->callbacks[i].id == id) {
                grp->callbacks[i].in_use = 0;
                grp->cb_count--;
                return;
            }
        }
    }
}

/* 发布：遍历 topic 组，匹配则触发组内所有活跃回调 */
void kwcc_bus_publish(const char *topic, const void *data, size_t len) {
    for (kwcc_bus_group_t *grp = g_kwcc_bus_head; grp; grp = grp->next) {
        if (!match(grp->topic, topic)) continue;
        for (int i = 0; i < KWCC_BUS_GROUP_MAX_CB; i++) {
            if (grp->callbacks[i].in_use) {
                grp->callbacks[i].cb(topic, data, len, grp->callbacks[i].user_data);
            }
        }
    }
}
```

### 使用示例

```c
/* HTTP 模块：发布响应事件 */
kwcc_bus_publish("http/end/req_1", &response, sizeof(response));

/* JS 桥接层：订阅所有事件 */
kwcc_bus_subscribe("*", kwcc_js_on_bus_event, ctx);

/* 日志模块：订阅所有 HTTP 事件 */
kwcc_bus_subscribe("http/", kwcc_log_on_event, NULL);
```

---

## Topic 设计

### 两套系统，独立运行

```
UI 桥接（kwcc_ui_bus.c/h）            通用 bus（kwcc_bus.c/h）
─────────────────────────              ──────────────────────
topic map：ID→topic 映射                subscriber 链表（topic 组）
kwcc_ui_bus_dispatch_event(...)        kwcc_bus_publish(topic, data, len)
→ JS_Eval $bus.emit                     → 触发 subscriber 回调
                                        → JS 桥接转发到 $bus
```

两套系统通过 topic 字符串对齐语义，但实现完全独立。

### 命名空间规范

```
{module}/{resource}/{action}

UI 桥接 topic：
  ui/calc/btn0         ← 计算器按钮
  ui/calc/op_plus      ← 运算符按钮
  ui/test/reset        ← 测试重置按钮

bus topic：
  http/end/req_1       ← HTTP 请求完成
  http/progress/req_1  ← HTTP 请求进度
  fs/read_done         ← 文件读取完成
  timer/timeout/clock  ← 定时器超时
```

### 匹配规则

| 模式 | 示例 | 匹配 |
|------|------|------|
| 精确 | `"http/end/req_1"` | 只匹配 `http/end/req_1` |
| `*` 通配 | `"*"` | 匹配所有 topic |
| 前缀 | `"http/"` | 匹配所有以 `http/` 开头的 topic |

---

## 兼容性

### JS 端零改动

`$bus.on("topic", handler)` 完全不变。JS 层感知到的事件格式不变。

非 UI 事件桥接到 JS 时，action 传空字符串：
```c
snprintf(buf, "$bus.emit('%s', '', new Object());", topic);
```

### C 端改动

| 文件 | 改动 |
|------|------|
| `src/kwcc_ui_bus.h/c` | 新建：topic map + `kwcc_ui_bus_dispatch_event` |
| `src/kwcc_bus.h/c` | 重写为 topic 组链表 + 组内回调数组 |
| `src/kwcc_js.c` | 加入 `kwcc_bus_subscribe("*", ...)` + JS 回调 |
| `src/main.m` | `kwcc_bus_begin_frame()` → `kwcc_ui_bus_begin_frame()` |

---

## 实施步骤

| Step | 内容 | 改动文件 |
|------|------|---------|
| 1 | 新建 `kwcc_ui_bus.h/c`，加入 topic map + dispatch_event | `src/kwcc_ui_bus.h`, `src/kwcc_ui_bus.c` |
| 2 | kwcc_bus.h/c 重写为 topic 组链表 | `src/kwcc_bus.h`, `src/kwcc_bus.c` |
| 3 | kwcc_js.c 加入 bus consumer 回调 | `src/kwcc_js.c` |
| 4 | 替换调用点：`kwcc_dispatch_event` → `kwcc_ui_bus_dispatch_event` | `src/kwcc_ui.c`, `src/main.m` |
| 5 | 编译验证 | `make clean && make` |
| 6 | 运行时验证：calc/test/svg 按钮正常响应 | `make run` |
