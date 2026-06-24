# 设计决策记录：kwcc_bus 重构

> 记录设计讨论过程，解释"为什么这么设计"。
> 方案文档（`requirements/bus-split-design.md`）写结论，这里写原因。

---

## 决策 1：拆分 UI 桥接与通用事件总线

### 问题

当前 `kwcc_bus.c`（52 行）同时承担了两个不相关的职责：
1. topic map（ID→topic 映射）— 纯 UI 内部需求
2. `kwcc_dispatch_event` — UI→JS 的事件桥接

### 决策

- **`kwcc_ui_bus.c/h`** — UI→JS 事件桥接
- **`kwcc_bus.c/h`** — 通用 C 事件总线

### 原因

bus 如果耦合了 mquickjs，任何依赖 bus 的模块都间接依赖 JS。拆开后 `kwcc_bus.c` 零外部依赖，像 `kwcc_mempool` 一样是纯 C 基础设施。

---

## 决策 2：函数命名遵守 `kwcc_<module>_<action>` 规范

原 `kwcc_dispatch_event` 没有模块前缀。改为 `kwcc_ui_bus_dispatch_event`（UI）和 `kwcc_bus_publish`（bus）。

---

## 决策 3：UI 桥接抽出为独立文件

UI 桥接有完整 API 和数据结构，放 `kwcc_ui.c` 里会让文件膨胀。独立文件职责清晰。

---

## 决策 4：bus 不需要 NORMAL/LIGHT 模式

之前设计了两种模式，过度设计。UI 的特殊需求已经由 `kwcc_ui_bus.c` 自己的 topic map 处理了，通用 bus 不需要知道。

---

## 决策 5：bus 用 topic 组 + 固定数组管理 subscriber

subscriber 链表中的每个节点是一个 topic 组，组内用固定大小数组（16）管理回调。不用哈希表，因为 16 个回调已经足够。

---

## 决策 6：topic sanitization — 清洗 + 校验两步走

### 问题

topic 字符串可能包含非法字符（引号、反斜杠等），会破坏 `JS_Eval` 的字符串。

### 决策

- `kwcc_base_topic_sanitize` — 只保留 `A-Z a-z 0-9 / _`，末尾 `/*` 通配符保留，其余 `*` 丢弃
- `kwcc_base_topic_check` — 拒绝空字符串、全是 `/` 的 topic

### 原因

sanitize 去非法字符，check 验结构合法性。两个函数分工不同，不能合并。

---

## 决策 7：`/*` 是唯一的通配符

### 问题

subscribe 用什么匹配所有 topic？`*` 还是 `/*`？

### 决策

`/*` 是 bus subscribe 端的通配符。白名单端用 `*` 表示"全部转发"。两边各用各的方式。

### 原因

`/*` 作为通配符，前缀 `/` 明确了它是路径后缀，不会和普通 topic 名冲突。白名单是前缀匹配，`*` 作为"全部"的标记更直观。

---

## 决策 8：JS 桥接白名单从 config 读取，不额外缓存

### 问题

白名单要不要在 JS 桥接里缓存？

### 决策

不缓存。每次 `kwcc_js_on_bus_event` 直接从 config 读。config 层本身有 mempool 缓存。

### 原因

额外的缓存数组（`g_js_bus_whitelist[32][128]`）和 init 函数是多余的。config 读一次就是 O(1)（mempool 查找），没必要再套一层。

---

## 决策 9：bus → JS 的 action 固定为 `"notify_c"`

### 问题

bus 事件转发到 JS 时，action 传什么？

### 决策

固定 `"notify_c"`，标识事件来自 C bus。

### 原因

空字符串没有语义。`notify_c` 让 JS 端 handler 能区分事件来源（UI 按钮是 `click`，C bus 是 `notify_c`）。

---

## 决策 10：白名单和 bus subscribe 的通配规则不同

| | bus subscribe | JS 白名单 |
|---|---|---|
| 全部匹配 | `/*` | `*` |
| 前缀匹配 | `http/` | `http/` |
| sanitize | 保留末尾 `/*`，丢弃其他 `*` | 不涉及 |

### 原因

bus subscribe 的 `/*` 是路径匹配规则。白名单是前缀匹配，`*` 单独出现表示"全部"，不需要路径语义。

---

## 决策 11：去掉冗余变量时保留核心逻辑

### 教训

用户要求去掉 `kwcc_js_init_bus_whitelist` + `g_js_bus_whitelist` 数组时，不应连带删掉逗号分隔的白名单解析逻辑。改动前必须分析完整函数职责。

---

## 决策 12：两套 Bus，独立运行

UI 桥接（直接 JS_Eval）和 bus（subscriber 链表）是两条独立路径，最终都到 `$bus.emit`。topic 命名不需要强制区分来源。
