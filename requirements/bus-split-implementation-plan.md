# 执行计划：kwcc_bus 重构

> 基于 `requirements/bus-split-design.md` 方案
> 创建于 2026-06-23

---

## 实施总览

按方案分 6 步实施。每步完成后验证编译通过。

| Step | 内容 | 新增/修改文件 | 依赖 |
|------|------|--------------|------|
| 1 | 新建 `kwcc_ui_bus.h/c` | `src/kwcc_ui_bus.h`, `src/kwcc_ui_bus.c` | 无 |
| 2 | 重写 `kwcc_bus.h/c` | `src/kwcc_bus.h`, `src/kwcc_bus.c` | 无 |
| 3 | bus 纯 C 测试 | `tests/test_bus.c` | Step 2 |
| 4 | kwcc_js.c 加入 bus consumer | `src/kwcc_js.c` | Step 2 |
| 5 | 替换调用点 | `src/kwcc_ui.c`, `src/main.m` | Step 1 |
| 6 | 编译验证 | — | Step 1-5 |
| 7 | 运行时验证 | — | Step 6 |

---

## Step 1: 新建 `kwcc_ui_bus.h/c`

**目的**：将 UI→JS 桥接从 `kwcc_bus.c` 抽离到独立文件。

### `src/kwcc_ui_bus.h`

```c
#ifndef KWCC_UI_BUS_H
#define KWCC_UI_BUS_H

void kwcc_ui_bus_set_js_ctx(void *ctx);
void kwcc_ui_bus_begin_frame(void);
void kwcc_ui_bus_bind_topic(int id, const char *topic);
void kwcc_ui_bus_dispatch_event(const char *topic, const char *action);

#endif
```

### `src/kwcc_ui_bus.c`

- 从 `kwcc_bus.c` 搬入 topic map（`g_kwcc_ui_topic_map`）
- 实现 `kwcc_ui_bus_begin_frame` / `kwcc_ui_bus_bind_topic` / `kwcc_ui_bus_dispatch_event`
- JSContext 通过 `kwcc_ui_bus_set_js_ctx` 设置

**验证**：`make` 编译通过（旧 `kwcc_bus.c` 还在，不影响）

---

## Step 2: 重写 `kwcc_bus.h/c`

**目的**：将 `kwcc_bus.c` 重写为纯 C Pub/Sub，零 mquickjs 依赖。

### `src/kwcc_bus.h`

```c
#ifndef KWCC_BUS_H
#define KWCC_BUS_H

#include <stddef.h>

typedef uint64_t kwcc_bus_sub_id_t;
typedef void (*kwcc_bus_cb_t)(const char *topic, const void *data,
                               size_t len, void *user_data);

void              kwcc_bus_init(void);
kwcc_bus_sub_id_t kwcc_bus_subscribe(const char *topic, kwcc_bus_cb_t cb, void *user_data);
void              kwcc_bus_unsubscribe(kwcc_bus_sub_id_t id);
void              kwcc_bus_publish(const char *topic, const void *data, size_t len);

#endif
```

### `src/kwcc_bus.c`

- 删除 `#include "mquickjs/mquickjs.h"`
- 删除 topic map（已搬到 `kwcc_ui_bus.c`）
- 删除 `kwcc_dispatch_event` / `kwcc_bind_topic` / `kwcc_bus_begin_frame`
- 实现 topic 组链表：
  - `kwcc_bus_group_t` — 每个节点是一个 topic 组
  - `kwcc_bus_cb_entry_t[16]` — 组内回调数组
  - `kwcc_bus_subscribe` — 找/创建组，加回调
  - `kwcc_bus_unsubscribe` — 找到 sub_id，标记 inactive
  - `kwcc_bus_publish` — 遍历组，匹配 topic，触发回调

**验证**：`make` 编译通过

---

## Step 3: bus 纯 C 测试

**目的**：验证 bus 作为独立 C 基础设施的正确性，不依赖 JS/UI。

### `tests/test_bus.c`

测试点：

| # | 测试 | 验证 |
|---|------|------|
| 1 | `kwcc_bus_init` 后 publish 不 crash | 空 bus 安全 |
| 2 | subscribe + publish 单次回调 | 基本功能 |
| 3 | 同一个 topic 多个 subscriber | 组内多回调 |
| 4 | unsubscribe 后不再触发 | 取消有效 |
| 5 | unsubscribe 不影响其他 subscriber | 隔离性 |
| 6 | topic 精确匹配 | `match("a/b", "a/b")` |
| 7 | topic 前缀匹配 | `match("http/", "http/end/req_1")` |
| 8 | topic `*` 通配 | `match("*", "any/topic")` |
| 9 | publish 不匹配的 topic | 不应触发 |
| 10 | 多个 topic 组，publish 只触发匹配的 | 组隔离 |
| 11 | sub_id 唯一性 | 每个 subscribe 返回不同 ID |
| 12 | unsubscribe 不存在的 ID | 不 crash |

```c
/* 示例：test_bus.c */
#include <stdio.h>
#include <assert.h>
#include "kwcc_bus.h"

static int g_cb_count = 0;
static void *g_cb_data = NULL;

static void test_cb(const char *topic, const void *data, size_t len, void *user_data) {
    g_cb_count++;
    g_cb_data = user_data;
}

void test_subscribe_publish() {
    kwcc_bus_init();
    g_cb_count = 0;
    kwcc_bus_sub_id_t id = kwcc_bus_subscribe("test/topic", test_cb, (void*)0x123);
    assert(id > 0);
    kwcc_bus_publish("test/topic", NULL, 0);
    assert(g_cb_count == 1);
    assert(g_cb_data == (void*)0x123);
}

void test_unsubscribe() {
    g_cb_count = 0;
    kwcc_bus_sub_id_t id = kwcc_bus_subscribe("test", test_cb, NULL);
    kwcc_bus_unsubscribe(id);
    kwcc_bus_publish("test", NULL, 0);
    assert(g_cb_count == 0);
}

/* ... 其余测试 */

int main() {
    test_subscribe_publish();
    printf("PASS: subscribe_publish\n");
    test_unsubscribe();
    printf("PASS: unsubscribe\n");
    /* ... */
    return 0;
}
```

**验证**：`gcc tests/test_bus.c src/kwcc_bus.c -I. -o tests/test_bus && tests/test_bus` 全部通过

---

## Step 4: kwcc_js.c 加入 bus consumer

**目的**：JS 桥接层作为 bus consumer 订阅所有 C 事件。

### `src/kwcc_js.c` 改动

```c
/* JS 桥接回调 — 收到 bus 事件后转发到 JS $bus */
static void kwcc_js_on_bus_event(const char *topic, const void *data,
                                  size_t len, void *user_data) {
    JSContext *ctx = (JSContext *)user_data;
    if (!ctx || !topic) return;

    char buf[512];
    snprintf(buf, sizeof(buf), "$bus.emit('%s', '', new Object());", topic);
    JS_Eval(ctx, buf, strlen(buf), "<bus>", JS_EVAL_REPL);
}

/* 在 kwcc_create_js() 中调用 */
void kwcc_js_bus_init(JSContext *ctx) {
    kwcc_bus_subscribe("*", kwcc_js_on_bus_event, ctx);
}
```

**验证**：`make` 编译通过

---

## Step 5: 替换调用点

**目的**：所有旧 API 调用替换为新 API。

### 改动点

| 文件 | 旧调用 | 新调用 |
|------|--------|--------|
| `src/kwcc_ui.c` button handler | `kwcc_dispatch_event(ctx, topic, "click")` | `kwcc_ui_bus_dispatch_event(topic, "click")` |
| `src/kwcc_ui.c` slider handler | `kwcc_dispatch_event(ctx, topic, "change")` | `kwcc_ui_bus_dispatch_event(topic, "change")` |
| `src/kwcc_ui.c` window close | `kwcc_dispatch_event(ctx, title, "close")` | `kwcc_ui_bus_dispatch_event(title, "close")` |
| `src/main.m` frame start | `kwcc_bus_begin_frame()` | `kwcc_ui_bus_begin_frame()` |
| `src/main.m` init | — | `kwcc_ui_bus_set_js_ctx(g_js_ctx)` |
| `src/main.m` init | — | `kwcc_bus_init()` + `kwcc_js_bus_init(g_js_ctx)` |

### `src/kwcc.h` 更新

```c
#include "kwcc_ui_bus.h"   /* 新增 */
#include "kwcc_bus.h"      /* 保留，但 API 变了 */
```

**验证**：`make` 编译通过

---

## Step 6: 编译验证

```bash
make clean && make
```

必须通过，无警告。

---

## Step 7: 运行时验证

```bash
make run
```

验证项：
1. [ ] 窗口正常显示
2. [ ] calc 模块按钮点击正常响应
3. [ ] test 模块按钮点击正常响应
4. [ ] svg 模块窗口关闭/打开正常
5. [ ] kwcc.log 无错误

---

## 风险点

| 风险 | 影响 | 缓解 |
|------|------|------|
| `kwcc_bus.c` 重写遗漏旧函数引用 | 编译错误 | Step 2 完成后仔细检查 grep |
| bus 测试不通过 | 核心功能有 bug | Step 3 先跑 C 测试，不依赖 JS |
| JSContext 未正确传递给 UI bus | dispatch 无效 | Step 5 中 `kwcc_ui_bus_set_js_ctx` 必须在 `kwcc_create_js` 后调用 |
| topic 组链表 match 逻辑错误 | 事件不触发 | Step 3 C 测试覆盖 match 逻辑，Step 7 运行时验证 |
