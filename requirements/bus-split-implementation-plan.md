# 执行计划：kwcc_bus 重构

> 基于 `requirements/bus-split-design.md` 方案
> 创建于 2026-06-23

---

## 实施总览

按方案分 7 步实施。每步完成后验证编译通过。

| Step | 内容 | 新增/修改文件 | 依赖 |
|------|------|--------------|------|
| 1 | 新建 `kwcc_ui_bus.h/c` | `src/kwcc_ui_bus.h`, `src/kwcc_ui_bus.c` | 无 |
| 2 | 新建 `kwcc_base.h/c` | `src/kwcc_base.h`, `src/kwcc_base.c` | 无 |
| 3 | 重写 `kwcc_bus.h/c` | `src/kwcc_bus.h`, `src/kwcc_bus.c` | Step 2 |
| 4 | bus 纯 C 测试 | `tests/test_bus.c` | Step 3 |
| 5 | kwcc_js.c 加白名单 + bus consumer | `src/kwcc_js.c` | Step 2, 3 |
| 6 | 编译验证 | `Makefile` | Step 1-5 |
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

- 从 `kwcc_bus.c` 搬入 topic map
- 实现 `kwcc_ui_bus_begin_frame` / `kwcc_ui_bus_bind_topic` / `kwcc_ui_bus_dispatch_event`
- 入口加 `kwcc_base_topic_sanitize` + `kwcc_base_topic_check`

**验证**：`make` 编译通过

---

## Step 2: 新建 `kwcc_base.h/c`

**目的**：topic 清洗 + 校验工具函数。

### `src/kwcc_base.h`

```c
void kwcc_base_topic_sanitize(char *out, size_t out_size, const char *in);
int  kwcc_base_topic_check(const char *topic);
```

### `src/kwcc_base.c`

- `kwcc_base_topic_sanitize`：只保留 `A-Z a-z 0-9 / _`，末尾 `/*` 通配符保留
- `kwcc_base_topic_check`：拒绝空字符串、全是 `/` 的 topic

**验证**：`make` 编译通过

---

## Step 3: 重写 `kwcc_bus.h/c`

**目的**：纯 C Pub/Sub，零 mquickjs 依赖。

### `src/kwcc_bus.h`

```c
#define KWCC_BUS_WILDCARD "/*"

typedef uint64_t kwcc_bus_sub_id_t;
typedef void (*kwcc_bus_cb_t)(const char *topic, const void *data, size_t len, void *user_data);

void              kwcc_bus_init(void);
kwcc_bus_sub_id_t kwcc_bus_subscribe(const char *topic, kwcc_bus_cb_t cb, void *user_data);
void              kwcc_bus_unsubscribe(kwcc_bus_sub_id_t id);
void              kwcc_bus_publish(const char *topic, const void *data, size_t len);
```

### `src/kwcc_bus.c`

- 删除 `#include "mquickjs/mquickjs.h"` 和旧 API
- 实现 topic 组链表（`KWCC_BUS_GROUP_MAX_CB = 16`）
- publish/subscribe 入口加 `kwcc_base_topic_sanitize` + `kwcc_base_topic_check`
- 支持 `/*` 通配符匹配所有 topic
- match 函数使用 `KWCC_BUS_WILDCARD` 常量
- match 函数命名为 `kwcc_bus_match_topic`

**验证**：`make` 编译通过

---

## Step 4: bus 纯 C 测试

**目的**：验证 bus 作为独立 C 基础设施的正确性。

### `tests/test_bus.c`

| # | 测试 | 验证 |
|---|------|------|
| 1 | `kwcc_bus_init` 后 publish 不 crash | 空 bus 安全 |
| 2 | subscribe + publish 单次回调 | 基本功能 |
| 3 | unsubscribe 后不再触发 | 取消有效 |
| 4 | unsubscribe 不影响其他 subscriber | 隔离性 |
| 5 | 同一个 topic 多个 subscriber 全触发 | 组内多回调 |
| 6 | topic 精确匹配 | `"a/b"` 匹配 `"a/b"`，不匹配 `"a/b/extra"` |
| 7 | topic 前缀匹配 | `"http/"` 匹配 `"http/end/req_1"` |
| 8 | `/*` 通配匹配 | `"/*"` 匹配所有 topic |
| 9 | publish 不匹配的 topic | 不应触发 |
| 10 | sub_id 唯一性 | 每个 subscribe 返回不同 ID |
| 11 | unsubscribe 不存在的 ID | 不 crash |
| 12 | data passthrough | user_data 正确传递 |
| 13 | topic sanitization `"net*work"` | → `"network"`（`*` 被丢弃） |
| 14 | topic sanitization `"network/*"` | → `"network/*"`（末尾通配符保留） |
| 15 | topic_check 拒绝空字符串 | 返回 0 |
| 16 | topic_check 拒绝全是 `/` | `"///"` 返回 0 |
| 17 | subscribe 无效 topic 返回 0 | 空 topic 不注册 |
| 18 | group 满 16 个返回 0 | 同 topic 超 16 个 subscriber 失败 |

**验证**：`gcc tests/test_bus.c src/kwcc_bus.c src/kwcc_base.c deps/log/log.c -I. -Ideps -D_GNU_SOURCE -o tests/bin/test_bus && tests/bin/test_bus` 全部通过

---

## Step 5: kwcc_js.c 加白名单 + bus consumer

**目的**：JS 桥接层作为 bus consumer 订阅 C 事件，白名单控制转发。

### `src/kwcc_js.c` 改动

```c
/* 白名单匹配（逗号分隔前缀）*/
static int match_whitelist(const char *whitelist, const char *topic);

/* bus 事件回调 */
static void kwcc_js_on_bus_event(const char *topic, const void *data, size_t len, void *user_data) {
    const char *whitelist = kwcc_config_get_core("bus/js_whitelist", "*");
    if (whitelist[0] == '*' && whitelist[1] == '\0') {
        /* * = 全部转发 */
    } else if (whitelist[0] == '\0') {
        return;  /* 空 = 不转发 */
    } else if (!match_whitelist(whitelist, topic)) {
        return;  /* 不在白名单内 */
    }
    /* 转发到 JS，action = "notify_c" */
}
```

### `kwcc_create_js()` 中调用

```c
kwcc_bus_subscribe(KWCC_BUS_WILDCARD, kwcc_js_on_bus_event, ctx);
```

**验证**：`make` 编译通过

---

## Step 6: 编译验证

```bash
make clean && make
```

必须通过，无警告。Makefile 加 `kwcc_base.c`。

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
| topic sanitization 逻辑错误 | 合法 topic 被丢弃 | Step 4 测试覆盖 |
| JSContext 未正确传递 | dispatch 无效 | `kwcc_ui_bus_set_js_ctx` 在 `kwcc_create_js` 后调用 |
| 白名单配置不生效 | 事件不转发或全转发 | 测试默认 `*`、`""`、前缀三种模式 |
