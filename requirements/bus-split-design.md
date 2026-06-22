# 方案：kwcc_bus 重构为通用 Pub/Sub 事件总线

> 状态：待论证
> 优先级：最高（async-io HTTP 模块前置依赖）
> 创建于 2026-06-22

## 背景

### 当前问题

`kwcc_bus.c` 混杂了三层职责：

| 职责 | 归属 | 问题 |
|------|------|------|
| topic map（ID→topic 映射） | microui 专用 | 混在通用模块里 |
| 每帧重置（`kwcc_bus_begin_frame`） | microui 专用 | 通用模块不该知道"帧" |
| JS dispatch（`kwcc_dispatch_event`） | JS 桥接 | include 了 mquickjs，任何依赖 bus 的模块都间接依赖 JS |

### 目标

**kwcc_bus 重构为纯 C Pub/Sub 事件总线**，完全通用：

```
任意 C 模块 → kwcc_bus_emit("topic", data, len)
                              ↓
                        kwcc_bus（纯 C，零依赖）
                              ↓
                触发已注册的 subscriber 回调
                              ↓
                    JS 桥接 → $bus.emit(...)
```

- **不感知 microui** — 没有 topic map，没有 ID，没有每帧重置
- **不感知 JS** — 不 include mquickjs，不操作 JSValue
- **零外部依赖** — 纯 C，任何模块都能用

---

## 新架构

```
┌──────────────────────────────────────────────────────────┐
│  kwcc_ui.c（UI 层，内部维护 topic map）                    │
│  ui_topic_map[ID→topic] · kwcc_ui_bind_topic · kwcc_ui_begin_frame  │
│  按钮点击 → 查 UI 自己的 map → kwcc_bus_emit("test/click", ...)  │
├──────────────────────────────────────────────────────────┤
│  kwcc_bus.c（纯 C Pub/Sub，零依赖）                        │
│  subscriber 列表 → kwcc_bus_subscribe / unsubscribe / emit        │
├──────────────────────────────────────────────────────────┤
│  kwcc_js.c（JS 桥接层，依赖 mquickjs）                     │
│  kwcc_js_on_event("*") → 收到事件 → JS_Eval $bus.emit(...)         │
│  kwcc_bus_dispatch_event_obj → 传 JSValue 对象                      │
└──────────────────────────────────────────────────────────┘
        ↑ 任何模块都能直接用
        │
┌──────────────────────────────────────────────────────────┐
│  kwcc_http.c（未来，纯 C HTTP 模块）                        │
│  kwcc_bus_emit("http/progress", &prog, sizeof(prog))             │
│  kwcc_bus_emit("http/end", &resp, sizeof(resp))                  │
└──────────────────────────────────────────────────────────┘
```

### 关键变化

| 旧 | 新 |
|----|-----|
| `kwcc_bus.c` 包含 topic map + JS dispatch | 只有 Pub/Sub，无 topic map，无 JS |
| `kwcc_bus_begin_frame()` 在 bus 中 | 移到 `kwcc_ui.c` 内部 |
| `kwcc_bind_topic()` 在 bus 中 | 移到 `kwcc_ui.c` 内部 |
| `kwcc_dispatch_event(ctx, topic, action)` | `kwcc_bus_emit(topic, data, len)` + JS 回调 |

---

## API 设计

### kwcc_bus.h（纯 C）

```c
#ifndef KWCC_BUS_H
#define KWCC_BUS_H

#include <stddef.h>

#define KWCC_BUS_MAX_SUBSCRIBERS 32

/* 事件回调：topic 字符串 + 不透明数据 */
typedef void (*kwcc_bus_cb_t)(const char *topic, const void *data,
                               size_t len, void *user_data);

void kwcc_bus_init(void);
void kwcc_bus_subscribe(const char *topic, kwcc_bus_cb_t cb, void *user_data);
void kwcc_bus_unsubscribe(kwcc_bus_cb_t cb, void *user_data);
void kwcc_bus_emit(const char *topic, const void *data, size_t len);

#endif
```

### kwcc_bus.c（纯 C）

```c
#include <string.h>
#include <stdio.h>
#include "kwcc_bus.h"
#include "llog.h"

typedef struct {
    const char   *topic;        /* 订阅的 topic，支持 "*" 通配 */
    kwcc_bus_cb_t cb;
    void         *user_data;
    int           in_use;
} kwcc_bus_sub_t;

static kwcc_bus_sub_t g_kwcc_bus_subs[KWCC_BUS_MAX_SUBSCRIBERS];

void kwcc_bus_init(void) {
    memset(g_kwcc_bus_subs, 0, sizeof(g_kwcc_bus_subs));
}

void kwcc_bus_subscribe(const char *topic, kwcc_bus_cb_t cb, void *user_data) {
    for (int i = 0; i < KWCC_BUS_MAX_SUBSCRIBERS; i++) {
        if (!g_kwcc_bus_subs[i].in_use) {
            g_kwcc_bus_subs[i].topic = topic;
            g_kwcc_bus_subs[i].cb = cb;
            g_kwcc_bus_subs[i].user_data = user_data;
            g_kwcc_bus_subs[i].in_use = 1;
            return;
        }
    }
    log_warn("bus: subscriber table full");
}

void kwcc_bus_unsubscribe(kwcc_bus_cb_t cb, void *user_data) {
    for (int i = 0; i < KWCC_BUS_MAX_SUBSCRIBERS; i++) {
        if (g_kwcc_bus_subs[i].in_use &&
            g_kwcc_bus_subs[i].cb == cb &&
            g_kwcc_bus_subs[i].user_data == user_data) {
            g_kwcc_bus_subs[i].in_use = 0;
            return;
        }
    }
}

void kwcc_bus_emit(const char *topic, const void *data, size_t len) {
    for (int i = 0; i < KWCC_BUS_MAX_SUBSCRIBERS; i++) {
        if (!g_kwcc_bus_subs[i].in_use) continue;
        kwcc_bus_sub_t *sub = &g_kwcc_bus_subs[i];

        /* 精确匹配 或 "*" 通配 */
        if (strcmp(sub->topic, topic) == 0 ||
            strcmp(sub->topic, "*") == 0) {
            sub->cb(topic, data, len, sub->user_data);
        }
    }
}
```

**注意**：没有任何 `#include "mquickjs/..."`，纯 C 零依赖。

### kwcc_ui.c — topic map 收归 UI 层

```c
/* UI 内部 topic map（从 kwcc_bus.c 搬过来）*/
#define KWCC_UI_TOPIC_MAP_SIZE 256
static struct { int id; char topic[128]; } g_kwcc_ui_topic_map[KWCC_UI_TOPIC_MAP_SIZE];
static int g_kwcc_ui_topic_count = 0;

void kwcc_ui_begin_frame(void) {
    g_kwcc_ui_topic_count = 0;
}

void kwcc_ui_bind_topic(int id, const char *topic) {
    if (g_kwcc_ui_topic_count < KWCC_UI_TOPIC_MAP_SIZE && topic) {
        g_kwcc_ui_topic_map[g_kwcc_ui_topic_count].id = id;
        strncpy(g_kwcc_ui_topic_map[g_kwcc_ui_topic_count].topic, topic, 127);
        g_kwcc_ui_topic_map[g_kwcc_ui_topic_count].topic[127] = '\0';
        g_kwcc_ui_topic_count++;
    }
}

/* 按钮/slider 点击 → 查 UI 自己的 map → 调 bus emit */
static void kwcc_ui_dispatch_by_id(int id, const char *action) {
    for (int i = 0; i < g_kwcc_ui_topic_count; i++) {
        if (g_kwcc_ui_topic_map[i].id == id) {
            kwcc_bus_emit(g_kwcc_ui_topic_map[i].topic, action,
                          action ? strlen(action) : 0);
            return;
        }
    }
}
```

**注意**：topic map 现在是 UI 内部实现，不再暴露到 bus.h。

### kwcc_js.c — JS 桥接层

```c
/* JS 事件回调 — 注册到 bus */
static void kwcc_js_on_event(const char *topic, const void *data,
                              size_t len, void *user_data) {
    JSContext *ctx = (JSContext *)user_data;
    if (!ctx || !topic) return;

    /* 构建 JS data 对象 */
    JSValue data_obj = JS_NewObject(ctx);

    /* 根据 topic 解析 data 结构体 */
    if (data && len > 0) {
        /* action 字符串（来自 button/slider）*/
        data_obj = JS_NewStringLen(ctx, (const char *)data, len);
    }

    /* 挂到全局变量 → JS_Eval → 清理 */
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global_obj, "__kwcc_bus_data", data_obj);

    char buf[512];
    /* topic 中的 ' 和 \ 需要转义 */
    char safe_topic[256];
    int tj = 0;
    for (int i = 0; topic[i] && tj < 254; i++) {
        char c = topic[i];
        if (c == '\\' || c == '\'') safe_topic[tj++] = '\\';
        safe_topic[tj++] = c;
    }
    safe_topic[tj] = '\0';

    snprintf(buf, sizeof(buf),
             "$bus.emit('%s', new Object(), __kwcc_bus_data); "
             "delete global.__kwcc_bus_data;", safe_topic);
    JS_Eval(ctx, buf, strlen(buf), "<bus>", JS_EVAL_REPL);
}

void kwcc_js_bus_init(JSContext *ctx) {
    kwcc_bus_subscribe("*", kwcc_js_on_event, ctx);
}
```

### kwcc_bus_dispatch_event_obj（JS 侧专用）

供需要传递结构化 JSValue 对象的场景（如 HTTP 响应）。**在 `kwcc_js.c` 中实现**，因为它操作 JSValue：

```c
/* kwcc_js.c */
void kwcc_bus_dispatch_event_obj(JSContext *ctx, const char *topic, JSValue data_obj) {
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global_obj, "__kwcc_bus_data", data_obj);

    char buf[512];
    char safe_topic[256];
    int tj = 0;
    for (int i = 0; topic[i] && tj < 254; i++) {
        char c = topic[i];
        if (c == '\\' || c == '\'') safe_topic[tj++] = '\\';
        safe_topic[tj++] = c;
    }
    safe_topic[tj] = '\0';

    snprintf(buf, sizeof(buf),
             "$bus.emit('%s', new Object(), __kwcc_bus_data); "
             "delete global.__kwcc_bus_data;", safe_topic);
    JS_Eval(ctx, buf, strlen(buf), "<bus_obj>", JS_EVAL_REPL);
}
```

---

## 调用链对比

### 现有：button 点击

```
旧:
  ui.button("OK", "test/click")
    → kwcc_bind_topic(widget_id, "test/click")        /* bus.c */
    → 点击 → kwcc_dispatch_event(ctx, topic, "click")  /* bus.c, 含 JS_Eval */

新:
  ui.button("OK", "test/click")
    → kwcc_ui_bind_topic(widget_id, "test/click")     /* UI 内部 */
    → 点击 → kwcc_ui_dispatch_by_id(widget_id, "click")/* UI 内部 */
      → kwcc_bus_emit("test/click", "click", 5)       /* bus.c, 纯 C */
        → kwcc_js_on_event → JS_Eval                  /* kwcc_js.c */
```

JS 侧完全不变：`$bus.on("test/click", handler)` 照常触发。

### 未来：HTTP 请求

```
  kwcc_http_on_read() → EOF
    → kwcc_bus_emit("http/end", &resp, sizeof(resp))  /* bus.c, 纯 C */
      → kwcc_js_on_event → 构建 JSValue → JS_Eval     /* kwcc_js.c */
```

HTTP 模块不需要知道 JS、不需要知道 microui。

---

## 兼容性

### JS 端零改动

`$bus.on("topic", handler)` 和 `$bus.emit("topic", action, data)` 完全不变。
现有的 calc/test/svg 模块无需修改。

### C 端改动

| 文件 | 改动 |
|------|------|
| `src/kwcc_bus.h` | 重写：去掉 `kwcc_dispatch_event`，加 `subscribe/unsubscribe/emit` |
| `src/kwcc_bus.c` | 重写：纯 Pub/Sub，移除 mquickjs include，移除 topic map |
| `src/kwcc_ui.c` | 加：topic map + `kwcc_ui_begin_frame` + `kwcc_ui_bind_topic` + `kwcc_ui_dispatch_by_id`；改：button/slider handler 调 `kwcc_ui_dispatch_by_id` |
| `src/kwcc_js.c` | 加：`kwcc_js_on_event` + `kwcc_js_bus_init` + `kwcc_bus_dispatch_event_obj` |
| `src/main.m` | 改：`kwcc_bus_begin_frame()` → `kwcc_ui_begin_frame()` |

---

## 实施步骤

| Step | 内容 | 改动文件 |
|------|------|---------|
| 1 | kwcc_bus.h 重写为新 API | `src/kwcc_bus.h` |
| 2 | kwcc_bus.c 重写为纯 Pub/Sub | `src/kwcc_bus.c` |
| 3 | kwcc_ui.c 加 topic map + dispatch_by_id | `src/kwcc_ui.c` |
| 4 | kwcc_js.c 加 JS 桥接回调 + dispatch_event_obj | `src/kwcc_js.c` |
| 5 | 替换调用点：`kwcc_bus_begin_frame` → `kwcc_ui_begin_frame` | `src/main.m` |
| 6 | 编译验证 | `make clean && make` |
| 7 | 运行时验证：calc/test/svg 按钮正常响应 | `make run` |

### 改动量

约 100-150 行代码调整，无新增外部依赖。

---

## 风险点

| 风险 | 影响 | 缓解 |
|------|------|------|
| subscriber 表只支持 32 个 | 订阅数超限 | 当前 JS 桥接只注册 1 个 `*`，空间充足 |
| `kwcc_bus_emit` 同步调用回调 | 回调耗时阻塞 emit | JS_Eval 在 emit 中同步执行，60fps 下可控 |
| topic 字符串转义遗漏 | JS SyntaxError | topic 只含 `[a-zA-Z0-9_/.]`，`*` 无需转义 |

---

## 未来扩展

重构后的 bus 是通用基础设施，后续可支持：

- **`*` 通配符** — `kwcc_bus_subscribe("http/*", ...)` 收所有 HTTP 事件
- **前缀匹配** — `kwcc_bus_subscribe("http/", ...)` 匹配所有以 `http/` 开头的 topic
- **中间件** — 在 `kwcc_bus_emit` 中加日志、统计、事件拦截
- **多 JSContext** — 注册多个 subscriber 回调，每个指向不同的 JSContext
- **文件 I/O 事件** — `kwcc_bus_emit("fs/read_done", &result, sizeof(result))`
- **定时器事件** — `kwcc_bus_emit("timer/timeout", &timer_id, sizeof(timer_id))`

任何新模块只需要 `kwcc_bus_emit("topic", data, len)` 就能和 JS 通信。
