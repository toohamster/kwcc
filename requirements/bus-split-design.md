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
│  kwcc_bus_subscribe("*", kwcc_js_on_bus_event, ctx)              │
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

Topic 只允许 `A-Z a-z 0-9 / _` 字符。所有入口强制清洗，不合规字符直接丢弃。

```c
/* kwcc_base.h */
void kwcc_base_topic_sanitize(char *out, size_t out_size, const char *in);
int  kwcc_base_topic_check(const char *topic);
```

```c
/* kwcc_base.c */
void kwcc_base_topic_sanitize(char *out, size_t out_size, const char *in) {
    int j = 0;
    for (int i = 0; in[i] && j < out_size - 1; i++) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '_') {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

int kwcc_base_topic_check(const char *topic) {
    if (topic[0] == '\0') return 0;
    int has_content = 0;
    for (int i = 0; topic[i]; i++) {
        if (topic[i] != '/') { has_content = 1; break; }
    }
    return has_content;  /* 全是 / 返回 0 */
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

---

## 第三部分：JS 桥接 — 白名单 + `notify_c`

从 config 读取白名单，每次事件触发时直接匹配逗号分隔列表，不额外缓存。

```c
/* kwcc_js.c */

/* 白名单匹配（逗号分隔的前缀列表）*/
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
| `*` 通配 | `"*"` | 匹配所有 topic |
| 前缀 | `"http/"` | 匹配所有以 `http/` 开头的 topic |

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
