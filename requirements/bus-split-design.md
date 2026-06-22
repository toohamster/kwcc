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

**kwcc_bus 重构为纯 C Pub/Sub 事件总线**，完全通用，零业务耦合：

```
bus 是纯通道，不感知任何模块
  ├─ NORMAL 订阅：链表 + ref_count + 生命周期管理
  └─ LIGHT 绑定：只标记 topic 有发射点，bindOnce 语义

各模块是 bus 的 consumer：
  ├─ JS 桥接层：subscribe("*", js_callback, ctx)
  ├─ UI 模块：light_bind(topic, widget_id) + emit
  └─ 未来模块：按需 subscribe 或 emit
```

- **不感知 microui** — topic map 移到 UI 层
- **不感知 JS** — 不 include mquickjs，不操作 JSValue，不知道任何回调
- **零外部依赖** — 纯 C，任何模块都能用
- **两种模式** — NORMAL（真订阅）+ LIGHT（轻绑定），调用方按需选择

---

## 新架构

```
┌──────────────────────────────────────────────────────────┐
│  kwcc_ui.c（UI 模块，bus consumer）                        │
│  内部维护 widget_id → topic 映射                           │
│  点击 → kwcc_bus_light_bind(topic, widget_id)                     │
│       → kwcc_bus_emit(topic, action, len)                         │
├──────────────────────────────────────────────────────────┤
│  kwcc_bus.c（纯 C Pub/Sub，零依赖）                        │
│  NORMAL subscriber 链表（topic + cb + ref_count）          │
│  LIGHT bind map（topic → bind_data）                       │
│  subscribe / unsubscribe / light_bind / light_unbind / emit       │
├──────────────────────────────────────────────────────────┤
│  kwcc_js.c（JS 桥接层，bus consumer）                      │
│  kwcc_bus_subscribe("*", js_on_bus_event, ctx)                   │
│  收到事件 → 构建 JSValue → JS_Eval $bus.emit(...)                │
│  js_bus_dispatch_obj（内部函数，传 JSValue 对象）                    │
└──────────────────────────────────────────────────────────┘
        ↑ 任何模块都能直接用
        │
┌──────────────────────────────────────────────────────────┐
│  kwcc_http.c（未来，bus consumer）                         │
│  kwcc_bus_subscribe("http/end/*", http_on_end, ...)              │
│  kwcc_bus_emit("http/end/req_1", &resp, ...)                     │
│  topic 由 HTTP 模块内部管理，JS 侧无感知                            │
└──────────────────────────────────────────────────────────┘
```

### 关键变化

| 旧 | 新 |
|----|-----|
| `kwcc_bus.c` 包含 topic map + JS dispatch | 纯 Pub/Sub，两种模式 |
| `kwcc_bus_begin_frame()` 在 bus 中 | 移到 `kwcc_ui.c` 内部 |
| `kwcc_bind_topic()` 在 bus 中 | 移到 `kwcc_ui.c` 内部 |
| 固定数组，32 个上限 | NORMAL 链表无上限 + LIGHT map |
| `kwcc_dispatch_event(ctx, topic, action)` | `kwcc_bus_emit(topic, data, len)` |
| bus include mquickjs | bus 零依赖，JS 回调是 consumer 注册的 |

---

## Topic 设计

### 两套 Bus，独立运行

```
C bus（kwcc_bus.c）                    JS bus（app/runtime/bus.js）
─────────────────                      ─────────────────────
C 模块用（UI/HTTP/FS/Timer）            JS 模块用（$store/业务逻辑）
NORMAL + LIGHT 两种模式                 只有 NORMAL 模式
kwcc_bus_emit → 触发 subscriber        $bus.emit → 触发 listeners
                                        ↑
                              桥接层转发：kwcc_js_on_bus_event
                              C bus emit → JS_Eval("$bus.emit(...)")
```

两套 bus 没有共享内存，没有共享 topic 注册表。topic 就是两边各自用的字符串，通过桥接层连通。

### Topic 属性

每个 topic 隐含三个属性：

| 属性 | C bus | JS bus |
|------|-------|--------|
| **origin**（事件来源） | C（隐式） | JS（隐式） |
| **mode**（订阅方式） | NORMAL 或 LIGHT | 只有 NORMAL |
| **name**（topic 字符串） | `"ui/calc/btn0"` | `"calc/dispatch"` |

origin 和 mode 不需要额外存储，由 bus 实例和 API 调用方式隐含决定。

### 命名空间规范

```
{module}/{resource}/{action}

module:    模块名（ui, http, fs, timer, store）
resource:  具体资源/组件（calc/btn0, end/req_1, timeout/clock）
action:    可选，具体动作（click, change, progress, error）
```

**C bus topic 示例**：
```
ui/calc/btn0         ← UI 计算器按钮（LIGHT）
ui/calc/op_plus      ← UI 运算符按钮（LIGHT）
ui/test/reset        ← UI 测试重置按钮（LIGHT）
http/end/req_1       ← HTTP 请求完成（NORMAL）
http/progress/req_1  ← HTTP 请求进度（NORMAL）
http/error/req_1     ← HTTP 请求失败（NORMAL）
```

**JS bus topic 示例**：
```
calc/dispatch        ← 计算器 store dispatch（NORMAL）
test/window/close    ← 测试窗口关闭（NORMAL）
```

### 匹配规则

| 模式 | 示例 | 匹配 |
|------|------|------|
| **精确** | `"ui/calc/btn0"` | 只匹配 `ui/calc/btn0` |
| **`*` 通配** | `"*"` | 匹配所有 topic |
| **前缀** | `"http/end/"` | 匹配 `http/end/req_1`, `http/end/req_2` 等 |

**注意**：`*` 通配只在 JS 桥接层收所有 C 事件、调试日志等场景使用，业务模块应使用前缀或精确匹配。

### UI topic 和 JS 订阅的关系

UI 声明 topic 只是标记"发射点"，不注册回调：

```c
ui.button("OK", "ui/calc/btn0")   ← 只是声明：点击时发射这个 topic
```

真正让 topic 有意义的是 **JS 侧的订阅**：

```javascript
$bus.on("ui/calc/btn0", function(action, data) {
    $store.dispatch("calc", "digit", { value: 0 });
});
```

**流程**：
1. UI 声明 topic → 只是标记发射点
2. 按钮点击 → C 发射 `kwcc_bus_emit("ui/calc/btn0", "click", 5)`
3. JS 桥接收 → `$bus.emit("ui/calc/btn0", "click", new Object())`
4. JS bus 匹配 → 找到 `$bus.on("ui/calc/btn0", ...)` → 触发 handler

**如果 JS 侧没有订阅**：UI 发射了没人收，事件消失，不影响任何事。

JS 侧**不需要预先声明或注册 topic**，只需要在自己关心的 topic 上 `$bus.on`。topic 字符串两边用同一套命名即可。

### NORMAL 模式 — 真订阅

用于需要生命周期管理的场景（HTTP、FS、Timer 等）。

```
模块 → kwcc_bus_subscribe("http/end/*", callback, user_data)
  → 链表新增节点，topic + cb + user_data，ref_count = 1
  → 返回 sub_id

bus emit → 遍历链表 → 匹配 topic → 调 callback
  → ref_count++ → callback → ref_count--

模块 → kwcc_bus_unsubscribe(sub_id)
  → active = 0，ref_count--
  → 当 active==0 && ref_count==0 时释放节点
```

特点：
- ref_count 保证迭代安全
- unsubscribe 有完整生命周期
- topic 支持精确匹配、`*` 通配、前缀匹配

### LIGHT 模式 — 轻绑定

用于每帧重置、bindOnce 语义的场景（UI widget 等）。

```
UI 每帧绘制 → kwcc_bus_light_bind("ui/calc/btn0", (void*)widget_id)
  → 如果 topic 已存在 → 覆盖 bind_data（bindOnce）
  → 如果 topic 不存在 → 新占位

UI 点击 → kwcc_bus_emit("ui/calc/btn0", "click", 5)
  → 查 LIGHT map → 找到精确匹配的 topic
  → 直接触发（不走 subscriber 链表）
```

特点：
- **bindOnce**：同一个 topic 多次 bind 只是覆盖，不重复占位
- 无 ref_count，无生命周期管理
- emit 时精确匹配 topic，直接触发
- 每帧可重新 bind，widget ID 变化不影响

### 两种模式的关系

LIGHT 和 NORMAL 是**独立的两条路径**：

```
kwcc_bus_emit("topic", data, len):
  1. 查 LIGHT map → 有精确匹配 → 直接触发 LIGHT 回调
  2. 遍历 NORMAL subscriber 链表 → 匹配 topic → 调 callback
```

两条路径互不干扰：
- LIGHT 只需要知道"这个 topic 有绑定"
- NORMAL 需要完整的 subscriber + callback
- 一个 topic 可以同时有 LIGHT bind 和 NORMAL subscribe

---

## API 设计

### kwcc_bus.h

```c
#ifndef KWCC_BUS_H
#define KWCC_BUS_H

#include <stddef.h>

/* ── 不透明订阅句柄 ── */
typedef void* kwcc_bus_sub_id_t;

/* ── 事件回调签名 ── */
typedef void (*kwcc_bus_cb_t)(const char *topic, const void *data,
                               size_t len, void *user_data);

/* ── 初始化 ── */
void kwcc_bus_init(void);

/* ── NORMAL 模式：真订阅 ── */
kwcc_bus_sub_id_t kwcc_bus_subscribe(const char *topic, kwcc_bus_cb_t cb, void *user_data);
void              kwcc_bus_unsubscribe(kwcc_bus_sub_id_t id);

/* ── LIGHT 模式：轻绑定（bindOnce）─ */
void kwcc_bus_light_bind(const char *topic, void *bind_data);
void kwcc_bus_light_unbind(const char *topic);

/* ── 发射事件 ── */
void kwcc_bus_emit(const char *topic, const void *data, size_t len);

#endif
```

### 数据结构

```c
/* NORMAL 订阅链表节点 */
typedef struct kwcc_bus_sub {
    uint64_t              id;         /* 唯一 ID，递增 */
    char                 *topic;      /* 订阅的 topic，订阅方拥有所有权 */
    kwcc_bus_cb_t         cb;
    void                 *user_data;
    int                   ref_count;  /* 引用计数 */
    int                   active;     /* 是否有效 */
    struct kwcc_bus_sub  *next;
} kwcc_bus_sub_t;

/* LIGHT 轻绑定 map 节点 */
typedef struct {
    const char *topic;      /* 精确 topic 字符串 */
    void       *bind_data;  /* 轻绑定数据 */
    int         in_use;
} kwcc_bus_light_t;
```

### kwcc_bus.c 实现

```c
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "kwcc_bus.h"
#include "llog.h"

/* ── NORMAL subscriber 链表 ── */
static kwcc_bus_sub_t  *g_kwcc_bus_head = NULL;
static uint64_t         g_kwcc_bus_next_id = 1;

/* ── LIGHT 轻绑定 map ── */
#define KWCC_BUS_LIGHT_MAX 256
static kwcc_bus_light_t  g_kwcc_bus_light[KWCC_BUS_LIGHT_MAX];

/* ── 初始化 ── */
void kwcc_bus_init(void) {
    g_kwcc_bus_head = NULL;
    g_kwcc_bus_next_id = 1;
    memset(g_kwcc_bus_light, 0, sizeof(g_kwcc_bus_light));
}

/* ── NORMAL: 真订阅 ── */
kwcc_bus_sub_id_t kwcc_bus_subscribe(const char *topic, kwcc_bus_cb_t cb, void *user_data) {
    kwcc_bus_sub_t *sub = (kwcc_bus_sub_t *)calloc(1, sizeof(kwcc_bus_sub_t));
    if (!sub) return NULL;
    sub->id = g_kwcc_bus_next_id++;
    sub->topic = strdup(topic);
    sub->cb = cb;
    sub->user_data = user_data;
    sub->ref_count = 1;
    sub->active = 1;
    sub->next = g_kwcc_bus_head;
    g_kwcc_bus_head = sub;
    return (kwcc_bus_sub_id_t)(uintptr_t)sub->id;
}

void kwcc_bus_unsubscribe(kwcc_bus_sub_id_t id) {
    uint64_t target_id = (uint64_t)(uintptr_t)id;
    for (kwcc_bus_sub_t *sub = g_kwcc_bus_head; sub; sub = sub->next) {
        if (sub->id == target_id) {
            sub->active = 0;
            sub->ref_count--;
            return;
        }
    }
}

/* ── LIGHT: 轻绑定（bindOnce）─ */
void kwcc_bus_light_bind(const char *topic, void *bind_data) {
    if (!topic) return;

    /* 1. 找已有的，覆盖（bindOnce 语义）*/
    for (int i = 0; i < KWCC_BUS_LIGHT_MAX; i++) {
        if (g_kwcc_bus_light[i].in_use &&
            strcmp(g_kwcc_bus_light[i].topic, topic) == 0) {
            g_kwcc_bus_light[i].bind_data = bind_data;
            return;
        }
    }
    /* 2. 找空位，新增 */
    for (int i = 0; i < KWCC_BUS_LIGHT_MAX; i++) {
        if (!g_kwcc_bus_light[i].in_use) {
            g_kwcc_bus_light[i].topic = topic;
            g_kwcc_bus_light[i].bind_data = bind_data;
            g_kwcc_bus_light[i].in_use = 1;
            return;
        }
    }
    log_warn("bus: light bind table full (max=%d)", KWCC_BUS_LIGHT_MAX);
}

void kwcc_bus_light_unbind(const char *topic) {
    if (!topic) return;
    for (int i = 0; i < KWCC_BUS_LIGHT_MAX; i++) {
        if (g_kwcc_bus_light[i].in_use &&
            strcmp(g_kwcc_bus_light[i].topic, topic) == 0) {
            g_kwcc_bus_light[i].in_use = 0;
            return;
        }
    }
}

/* ── 清理 dead NORMAL subscriber ── */
static void kwcc_bus_remove_dead(void) {
    kwcc_bus_sub_t **pp = &g_kwcc_bus_head;
    while (*pp) {
        kwcc_bus_sub_t *sub = *pp;
        if (!sub->active && sub->ref_count <= 0) {
            *pp = sub->next;
            free(sub->topic);
            free(sub);
        } else {
            pp = &sub->next;
        }
    }
}

/* ── 匹配规则：精确 / * 通配 / 前缀 ── */
static bool kwcc_bus_match(const char *pattern, const char *topic) {
    if (strcmp(pattern, topic) == 0) return true;
    if (strcmp(pattern, "*") == 0) return true;
    /* 前缀匹配：pattern 以 / 结尾，topic 以 pattern 开头 */
    size_t plen = strlen(pattern);
    if (plen > 0 && pattern[plen - 1] == '/' &&
        strncmp(pattern, topic, plen) == 0) {
        return true;
    }
    return false;
}

/* ── 发射事件 ── */
void kwcc_bus_emit(const char *topic, const void *data, size_t len) {
    if (!topic) return;

    /* 1. LIGHT 路径：精确匹配，直接触发 */
    for (int i = 0; i < KWCC_BUS_LIGHT_MAX; i++) {
        if (g_kwcc_bus_light[i].in_use &&
            strcmp(g_kwcc_bus_light[i].topic, topic) == 0) {
            /* LIGHT 直接触发，无 ref_count */
            if (g_kwcc_bus_light[i].bind_data) {
                /* bind_data 可以包含回调信息，或者直接作为事件标记 */
            }
            /* LIGHT 标记存在，继续走 NORMAL 路径触发实际回调 */
            break;
        }
    }

    /* 2. NORMAL 路径：遍历 subscriber 链表 */
    for (kwcc_bus_sub_t *sub = g_kwcc_bus_head; sub; sub = sub->next) {
        if (!sub->active) continue;
        if (!kwcc_bus_match(sub->topic, topic)) continue;

        sub->ref_count++;
        sub->cb(topic, data, len, sub->user_data);
        sub->ref_count--;
    }

    /* 3. 清理 dead 节点 */
    kwcc_bus_remove_dead();
}
```

**迭代安全保证**：
- NORMAL: `emit` 调回调前 `ref_count++`，回调返回后 `ref_count--`
- NORMAL: `unsubscribe` 只标记 `active = 0` 并 `ref_count--`
- NORMAL: 节点只在 `!active && ref_count <= 0` 时才被 `remove_dead` 真正释放
- 即使回调里调 `unsubscribe` 也不会崩溃，回调出错不会阻断后续 subscriber

### LIGHT 模式的工作方式

LIGHT bind 不注册回调，它只标记"这个 topic 有发射点"。实际的回调触发走 NORMAL 路径：

```
UI 层每帧：
  kwcc_bus_light_bind("ui/calc/btn0", (void*)(uintptr_t)widget_id)
  → LIGHT map 标记 "ui/calc/btn0" 有绑定

JS 桥接层初始化时：
  kwcc_bus_subscribe("*", kwcc_js_on_bus_event, ctx)
  → NORMAL subscriber 收所有事件

UI 点击时：
  kwcc_bus_emit("ui/calc/btn0", "click", 5)
  → LIGHT 路径：找到 "ui/calc/btn0" 有绑定（确认是有效事件）
  → NORMAL 路径：遍历 subscriber → "*" 匹配 → 调 kwcc_js_on_bus_event
```

LIGHT 的作用是：
1. 提供快速查找（O(1) 精确匹配 vs O(n) 链表遍历）
2. 标记有效 topic，避免发射无意义的空事件
3. 每帧可重新 bind，bindOnce 语义不重复占位

---

## 各模块作为 bus consumer 的使用方式

### UI 模块（kwcc_ui.c）

```c
/* UI 内部 topic map — 每帧重置 */
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

        /* 轻绑定到 bus — bindOnce */
        kwcc_bus_light_bind(topic, (void *)(uintptr_t)id);
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

### JS 桥接层（kwcc_js.c）

```c
/* JS 事件回调 — bus 的一个 consumer */
static void kwcc_js_on_bus_event(const char *topic, const void *data,
                                  size_t len, void *user_data) {
    JSContext *ctx = (JSContext *)user_data;
    if (!ctx || !topic) return;

    JSValue data_obj;
    if (data && len > 0) {
        data_obj = JS_NewStringLen(ctx, (const char *)data, len);
    } else {
        data_obj = JS_NewObject(ctx);
    }

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
    JS_Eval(ctx, buf, strlen(buf), "<bus>", JS_EVAL_REPL);
}

/* 内部函数：直接传递已构建的 JSValue 对象 */
static void js_bus_dispatch_obj(JSContext *ctx, const char *topic, JSValue data_obj) {
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

void kwcc_js_bus_init(JSContext *ctx) {
    /* JS 桥接层作为 bus consumer，收所有事件 */
    kwcc_bus_subscribe("*", kwcc_js_on_bus_event, ctx);
}
```

**注意**：`js_bus_dispatch_obj` 是 `kwcc_js.c` 内部静态函数，不暴露到头文件。`kwcc_js_on_bus_event` 是 JS 桥接层自己的回调，不是 bus 的一部分。

### 未来 HTTP 模块（kwcc_http.c）

```c
/* HTTP 模块作为 bus consumer，订阅自己关心的事件 */
static void http_on_end(const char *topic, const void *data, size_t len, void *user_data) {
    /* 从 topic 提取 req_id，处理响应 */
}

void kwcc_http_init(void) {
    /* HTTP 模块订阅自己的 topic */
    kwcc_bus_subscribe("http/end/*", http_on_end, NULL);
}

/* HTTP 响应完成 */
void kwcc_http_on_response(kwcc_http_req_t *req) {
    char topic[64];
    snprintf(topic, sizeof(topic), "http/end/%s", req->req_id);
    kwcc_bus_emit(topic, &req->response, sizeof(req->response));
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
    → kwcc_bus_light_bind("test/click", widget_id)    /* bus LIGHT */
    → 点击 → kwcc_ui_dispatch_by_id(widget_id, "click")/* UI 内部 */
      → kwcc_bus_emit("test/click", "click", 5)       /* bus.c, 纯 C */
        → kwcc_js_on_bus_event → JS_Eval              /* kwcc_js.c, consumer */
```

JS 侧完全不变：`$bus.on("test/click", handler)` 照常触发。

### 未来：HTTP 请求

```
  kwcc_http_on_read() → EOF
    → kwcc_bus_emit("http/end/req_1", &resp, sizeof(resp))  /* bus.c, 纯 C */
      → kwcc_js_on_bus_event → 构建 JSValue → JS_Eval       /* kwcc_js.c, consumer */
```

HTTP 模块不需要知道 JS、不需要知道 microui。topic 由 HTTP 模块内部管理，JS 侧无感知。

---

## 兼容性

### JS 端零改动

`$bus.on("topic", handler)` 和 `$bus.emit("topic", action, data)` 完全不变。
现有的 calc/test/svg 模块无需修改。

### C 端改动

| 文件 | 改动 |
|------|------|
| `src/kwcc_bus.h` | 重写：去掉 `kwcc_dispatch_event`，加 `subscribe/unsubscribe/light_bind/light_unbind/emit` |
| `src/kwcc_bus.c` | 重写：NORMAL 链表 + LIGHT map + ref_count，移除 mquickjs include，移除 topic map |
| `src/kwcc_ui.c` | 加：topic map + `kwcc_ui_begin_frame` + `kwcc_ui_bind_topic` + `kwcc_ui_dispatch_by_id` |
| `src/kwcc_js.c` | 加：`kwcc_js_on_bus_event` + `kwcc_js_bus_init` + `js_bus_dispatch_obj`（内部） |
| `src/main.m` | 改：`kwcc_bus_begin_frame()` → `kwcc_ui_begin_frame()` |

---

## 实施步骤

| Step | 内容 | 改动文件 |
|------|------|---------|
| 1 | kwcc_bus.h 重写为新 API | `src/kwcc_bus.h` |
| 2 | kwcc_bus.c 重写为 NORMAL 链表 + LIGHT map | `src/kwcc_bus.c` |
| 3 | kwcc_ui.c 加 topic map + dispatch_by_id + light_bind | `src/kwcc_ui.c` |
| 4 | kwcc_js.c 加 `kwcc_js_on_bus_event` + `kwcc_js_bus_init` + `js_bus_dispatch_obj`（内部） | `src/kwcc_js.c` |
| 5 | 替换调用点：`kwcc_bus_begin_frame` → `kwcc_ui_begin_frame` | `src/main.m` |
| 6 | 编译验证 | `make clean && make` |
| 7 | 运行时验证：calc/test/svg 按钮正常响应 | `make run` |

### 改动量

约 150-200 行代码调整，无新增外部依赖。

---

## 风险点

| 风险 | 影响 | 缓解 |
|------|------|------|
| `kwcc_bus_emit` 同步调用回调 | 回调耗时阻塞 emit | JS_Eval 在 emit 中同步执行，60fps 下可控 |
| topic 字符串转义遗漏 | JS SyntaxError | topic 只含 `[a-zA-Z0-9_/.]`，`*` 无需转义 |
| NORMAL 链表节点未正确释放 | 内存泄漏 | `remove_dead` 在每次 emit 后清理，ref_count 保证安全 |
| LIGHT map 满（256） | 新轻绑定失败 | UI widget 数远小于 256，每帧重置自动释放 |

---

## 未来扩展

重构后的 bus 是通用基础设施，后续可支持：

- **`*` 通配符** — `kwcc_bus_subscribe("*", ...)` 收所有事件
- **前缀匹配** — `kwcc_bus_subscribe("http/", ...)` 匹配所有以 `http/` 开头的 topic
- **中间件** — 在 `kwcc_bus_emit` 中加日志、统计、事件拦截
- **多 JSContext** — 注册多个 subscriber 回调，每个指向不同的 JSContext
- **文件 I/O 事件** — `kwcc_bus_emit("fs/read_done", &result, sizeof(result))`
- **定时器事件** — `kwcc_bus_emit("timer/timeout", &timer_id, sizeof(timer_id))`

任何新模块只需要 `kwcc_bus_emit("topic", data, len)` 就能和 JS 通信。
