# 方案：kwcc_bus 重构 — 分离 UI 桥接与通用事件总线

> 状态：待论证
> 优先级：最高（async-io HTTP 模块前置依赖）
> 创建于 2026-06-23

## 背景

`kwcc_bus.c`（52 行）混杂了 UI topic map 和 JS dispatch，不是真正的 bus。

### 目标

- **kwcc_ui_bus.c/h** — UI→JS 事件桥接（独立文件）
- **kwcc_bus.c/h** — 通用 C 事件总线（纯 C Pub/Sub，零外部依赖）
- **kwcc_base.h/c** — 基础工具（topic 清洗 + 校验）

---

## 架构

```
┌──────────────────────────────────────────────────────────┐
│  kwcc_ui_bus.c/h（UI→JS 事件桥接，独立文件）                  │
│  kwcc_ui_bus_bind_topic(widget_id, "calc/btn0")                  │
│  kwcc_ui_bus_dispatch_event("calc/btn0", "click")                │
│    → JS_Eval("$bus.emit('calc/btn0', 'click', ...)")               │
├──────────────────────────────────────────────────────────┤
│  kwcc_bus.c/h（纯 C Pub/Sub，独立基础设施）                    │
│  subscriber 链表（topic 组 + 组内回调数组）                       │
│  subscribe / unsubscribe / publish                                │
│  零外部依赖，不 include mquickjs                                │
├──────────────────────────────────────────────────────────┤
│  kwcc_js.c（JS 桥接层，bus consumer）                         │
│  kwcc_bus_subscribe("/*", kwcc_js_on_bus_event, ctx)             │
│  白名单检查 → JS_Eval $bus.emit(topic, "notify_c", ...)          │
└──────────────────────────────────────────────────────────┘
        ↑ 任何 C 模块都能直接用
        │
┌──────────────────────────────────────────────────────────┐
│  kwcc_http.c（未来，bus consumer）                         │
│  kwcc_bus_publish("http/end/req_1", &resp, ...)                  │
└──────────────────────────────────────────────────────────┘
```

---

## 基础工具：kwcc_base.h/c

Topic 只允许 `A-Z a-z 0-9 / _` 字符。subscribe 支持 `/*` 通配符（匹配所有 topic）。

```c
/* kwcc_base.h */
void kwcc_base_topic_sanitize(char *out, size_t out_size, const char *in);
int  kwcc_base_topic_check(const char *topic);
```

```c
/* kwcc_base.c */

/* 清洗：末尾是 /* 时保留，其余 * 全部丢弃 */
void kwcc_base_topic_sanitize(char *out, size_t out_size, const char *in) {
    size_t len = strlen(in);
    int keep_star = (len >= 2 && in[len-2] == '/' && in[len-1] == '*');

    int j = 0;
    size_t limit = keep_star ? len - 2 : len;
    for (size_t i = 0; i < limit && j < (int)out_size - 3; i++) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '_') {
            out[j++] = c;
        }
    }
    if (keep_star) {
        out[j++] = '/';
        out[j++] = '*';
    }
    out[j] = '\0';
}

/* 校验：拒绝空字符串、全是 / 的主题 */
int kwcc_base_topic_check(const char *topic) {
    if (topic[0] == '\0') return 0;
    if (strcmp(topic, "/*") == 0) return 1;  /* 通配符合法 */

    size_t len = strlen(topic);
    if (len >= 2 && topic[len-2] == '/' && topic[len-1] == '*') {
        /* 去掉 /* 检查前面部分 */
        len -= 2;
    }
    for (size_t i = 0; i < len; i++) {
        if (topic[i] != '/') return 1;
    }
    return 0;  /* 全是 / */
}
```

**四个入口都加清洗 + 校验**：

| 入口 | 文件 | 处理 |
|------|------|------|
| bus 发布 | `kwcc_bus_publish` | sanitize → check → 空/全斜杠则 warn + return |
| bus 订阅 | `kwcc_bus_subscribe` | 同上 |
| UI 发布 | `kwcc_ui_bus_dispatch_event` | 同上 |
| UI 绑定 | `kwcc_ui_bus_bind_topic` | 同上 |

---

## 第一部分：UI→JS 桥接（`kwcc_ui_bus.c/h`）

已实现（Step 1 已完成）。包含 topic map + `kwcc_ui_bus_dispatch_event`。

---

## 第二部分：通用 C 事件总线（`kwcc_bus.c/h`）

### 数据结构

```c
#define KWCC_BUS_GROUP_MAX_CB 16

typedef struct {
    uint64_t        id;
    kwcc_bus_cb_t   cb;
    void           *user_data;
    int             in_use;
} kwcc_bus_cb_entry_t;

typedef struct kwcc_bus_group {
    char                  *topic;
    kwcc_bus_cb_entry_t    callbacks[KWCC_BUS_GROUP_MAX_CB];
    int                    cb_count;
    struct kwcc_bus_group *next;
} kwcc_bus_group_t;
```

### API

```c
#define KWCC_BUS_WILDCARD "/*"

typedef uint64_t kwcc_bus_sub_id_t;
typedef void (*kwcc_bus_cb_t)(const char *topic, const void *data, size_t len, void *user_data);

void              kwcc_bus_init(void);
kwcc_bus_sub_id_t kwcc_bus_subscribe(const char *topic, kwcc_bus_cb_t cb, void *user_data);
void              kwcc_bus_unsubscribe(kwcc_bus_sub_id_t id);
void              kwcc_bus_publish(const char *topic, const void *data, size_t len);
```

### publish/subscribe 入口加 topic 清洗

```c
void kwcc_bus_publish(const char *topic, const void *data, size_t len) {
    char safe[256];
    kwcc_base_topic_sanitize(safe, sizeof(safe), topic);
    if (!kwcc_base_topic_check(safe)) {
        log_warn("bus: publish skipped, invalid topic: '%s'", topic);
        return;
    }
    ...  /* 后续用 safe 匹配 */
}
```

subscribe 同理。

### 通配符常量

`kwcc_bus.h` 定义 `#define KWCC_BUS_WILDCARD "/*"`，`kwcc_bus.c` 的 match 函数使用该常量。

`kwcc_base.c` 的 `topic_check` 中 `strcmp(topic, "/*")` 保留硬编码 — 这是校验规则（`/*` 作为合法 topic），不是通配符语义。

---

## 第三部分：JS 桥接 — 白名单 + `notify_c`

从 config 读取白名单，每次事件触发时直接匹配逗号分隔列表，不额外缓存。

白名单规则：
- `*` → 全部转发
- `""` → 不转发
- 逗号分隔前缀列表 → 前缀匹配

```c
/* kwcc_js.c */

static int match_whitelist(const char *whitelist, const char *topic) {
    char buf[2048];
    strncpy(buf, whitelist, sizeof(buf) - 1);
    char *tok = strtok(buf, ",");
    while (tok) {
        size_t len = strlen(tok);
        if (len > 0 && tok[len - 1] == '/') {
            if (strncmp(topic, tok, len) == 0) return 1;
        } else if (strcmp(topic, tok) == 0) {
            return 1;
        }
        tok = strtok(NULL, ",");
    }
    return 0;
}

static void kwcc_js_on_bus_event(const char *topic, const void *data, size_t len, void *user_data) {
    const char *whitelist = kwcc_config_get_core("bus/js_whitelist", "*");
    if (whitelist[0] == '*' && whitelist[1] == '\0') {
        /* * = 全部转发 */
    } else if (whitelist[0] == '\0') {
        return;  /* 空 = 不转发 */
    } else if (!match_whitelist(whitelist, topic)) {
        return;  /* 不在白名单内 */
    }

    JSContext *ctx = (JSContext *)user_data;
    char buf[512];
    char safe[256];
    kwcc_base_topic_sanitize(safe, sizeof(safe), topic);
    snprintf(buf, sizeof(buf), "$bus.emit('%s', 'notify_c', new Object());", safe);
    JS_Eval(ctx, buf, strlen(buf), "<bus>", JS_EVAL_REPL);
}
```

### JS 侧配置

```javascript
/* 默认（不配置）：全部转发 */

/* 生产模式：只转发需要的 */
$config.coreSetTlv("bus", { js_whitelist: "ui/,http/end/,http/error/" });

/* 静默模式：不转发任何 C bus 事件 */
$config.coreSetTlv("bus", { js_whitelist: "" });
```

---

## Topic 命名

### 规范

topic 只允许 `A-Z a-z 0-9 / _`，`/` 作为层级分隔。

### 示例

```
UI 话题：
  calc/btn0         ← 计算器按钮
  calc/op_plus      ← 运算符按钮
  test/reset        ← 测试重置

bus 话题：
  http/end/req_1    ← HTTP 请求完成
  http/progress/req_1  ← HTTP 请求进度
  fs/read_done      ← 文件读取完成
  timer/timeout/clock  ← 定时器超时
```

### 匹配规则

| 模式 | 示例 | 匹配 |
|------|------|------|
| 精确 | `"http/end/req_1"` | 只匹配 `http/end/req_1` |
| `/*` 通配 | `"/*"` | 匹配所有 topic |
| 前缀 | `"http/"` | 匹配所有以 `http/` 开头的 topic |

由 `kwcc_bus_match_topic(pattern, topic)` 实现，pattern 来自 subscribe 的 topic，topic 来自 publish 的事件。

---

## C 端改动

| 文件 | 改动 |
|------|------|
| `src/kwcc_base.h/c` | 新建：`kwcc_base_topic_sanitize` + `kwcc_base_topic_check` |
| `src/kwcc_ui_bus.h/c` | 已存在（Step 1 完成），加 topic 清洗 |
| `src/kwcc_bus.h/c` | 重写为 topic 组链表，加 topic 清洗 |
| `src/kwcc_js.c` | 加白名单 + `kwcc_js_on_bus_event` + config 读取 |
| `src/kwcc.h` | 加 `#include "kwcc_ui_bus.h"`（已完成） |
| `src/main.m` | 加 `kwcc_ui_bus_set_js_ctx(g_js_ctx)`（已完成） |

---

## 实施步骤

| Step | 内容 | 改动文件 |
|------|------|---------|
| 1 | 新建 `kwcc_ui_bus.h/c` | ✅ 已完成 |
| 2 | 新建 `kwcc_base.h/c` | `src/kwcc_base.h`, `src/kwcc_base.c` |
| 3 | 重写 `kwcc_bus.h/c` | `src/kwcc_bus.h`, `src/kwcc_bus.c` |
| 4 | bus 纯 C 测试 | `tests/test_bus.c` |
| 5 | kwcc_js.c 加白名单 + bus consumer | `src/kwcc_js.c` |
| 6 | 编译验证 | `make clean && make` |
| 7 | 运行时验证 | `make run` |
