# 设计决策记录：kwcc_bus 重构

> 记录设计讨论过程，解释"为什么这么设计"。
> 方案文档（`requirements/bus-split-design.md`）写结论，这里写原因。

---

## 决策 1：拆分 UI 桥接与通用事件总线

### 问题

当前 `kwcc_bus.c`（52 行）同时承担了两个不相关的职责：
1. topic map（ID→topic 映射）— 纯 UI 内部需求
2. `kwcc_dispatch_event` — UI→JS 的事件桥接

把它叫作"bus"是名不副实的。它只是一个 UI 专用的 C→JS 事件转发器。

### 决策

拆成两个独立的东西：

- **`kwcc_ui_bus.c/h`** — UI→JS 事件桥接，UI 层内部使用
- **`kwcc_bus.c/h`** — 通用 C 事件总线，独立基础设施

### 原因

如果 bus 耦合了 mquickjs（因为 dispatch 需要 JS_Eval），任何依赖 bus 的模块都间接依赖 JS。HTTP 模块、系统监控模块这些纯 C 模块就没法用 bus 做事件解耦了。

拆开后：
- `kwcc_ui_bus.c` 可以 include mquickjs，它本来就是 UI 和 JS 之间的桥
- `kwcc_bus.c` 零外部依赖，像 `kwcc_mempool` 一样是纯 C 基础设施

---

## 决策 2：函数命名遵守 `kwcc_<module>_<action>` 规范

### 问题

原 `kwcc_dispatch_event` 没有模块前缀，不符合项目命名规范。

### 决策

- UI 桥接：`kwcc_ui_bus_dispatch_event`
- 通用 bus：`kwcc_bus_subscribe` / `kwcc_bus_unsubscribe` / `kwcc_bus_publish`

### 原因

命名是架构的体现。`kwcc_ui_bus_dispatch_event` 一眼就知道是 UI bus 模块的函数，`kwcc_dispatch_event` 看不出来属于谁。

---

## 决策 3：UI 桥接抽出为独立文件 `kwcc_ui_bus.c/h`

### 问题

最开始考虑把 UI 桥接代码直接塞进 `kwcc_ui.c`。

### 决策

抽出为独立的 `kwcc_ui_bus.c/h`。

### 原因

UI 桥接有完整的 API（begin_frame / bind_topic / dispatch_event / set_js_ctx），有独立的数据结构（topic map），放在 `kwcc_ui.c` 里会让 UI 文件膨胀。独立文件职责清晰，也方便以后测试。

---

## 决策 4：bus 不需要 NORMAL/LIGHT 模式

### 问题

之前设计了 NORMAL（真订阅，ref_count）和 LIGHT（轻绑定，bindOnce）两种模式。后来发现这过度设计了。

### 决策

去掉 LIGHT 模式，bus 只有一种订阅方式：`subscribe → publish → unsubscribe`。

### 原因

LIGHT 模式是为了解决 UI 每帧重置的问题，但 UI 已经抽到 `kwcc_ui_bus.c` 里了，有自己的 topic map。通用 bus 不需要知道 UI 的特殊需求。

---

## 决策 5：bus 用哈希数组（组概念）管理 subscriber

### 问题

一开始用链表管理 subscriber，每个节点是一个订阅。后来讨论发现更好的方式。

### 决策

subscriber 链表中的每个节点是一个 **topic 组**，组内用哈希表管理该 topic 下的所有回调：

```
subscriber list:
  [topic="*"]      → { hash: { id=1→cb_a, id=5→cb_b } }
  [topic="http/"]  → { hash: { id=2→cb_c, id=3→cb_d } }
```

- `subscribe(topic, cb, user_data)` → 找到或创建 topic 组 → 哈希表新增条目 → 返回 sub_id
- `unsubscribe(sub_id)` → 找到对应 topic 组 → 哈希表删除该条目 → 完成
- `publish(topic)` → 遍历 topic 链表 → 匹配 → 遍历组内哈希表 → 触发所有回调

### 原因

1. **unsubscribe 不修改链表** — 只操作哈希表，publish 遍历时链表结构不变，安全
2. **不需要 ref_count** — 链表是只读的，哈希表的增删不影响遍历
3. **GC 简单** — 组内哈希为空时删除整个组节点，不需要复杂的引用计数
4. **概念清晰** — subscriber 是 topic 组，不是单个订阅

---

## 决策 6：GC 不是 pub 事件，是 publish 内部触发

### 问题

GC 怎么触发？三种方案：
- A. publish 内部检查阈值后触发
- B. 作为 bus 上的特殊 topic（`__bus/gc`）
- C. 外部定时器

### 决策

选方案 A：publish 完成后检查 inactive 数量，超过阈值则触发 GC 压缩。

### 原因

方案 B 有循环问题 — GC 修改 subscriber 结构，而 publish 正在遍历。虽然可以用"复制新数组 + 原子替换"来解决，但不值得。方案 C 需要外部调用方记得调，容易忘。方案 A 对调用方透明，自动管理。

---

## 决策 7：不区分 NORMAL/LIGHT，topic 本身就包含完整语义

### 问题

一度考虑给 `kwcc_bus_publish` 加一个 action 参数来和 UI 事件格式对齐。

### 决策

不加 action 参数。`publish(topic, data, len)` 就够了。

### 原因

UI 事件的 action 有意义（click/change），因为 topic 是控件标识。非 UI 事件的 topic 本身就包含完整语义（`sys/network/connected`、`http/end/req_1`），再加 action 是多余的。为了"格式统一"增加不必要的参数不值得。

---

## 决策 8：bus 不处理消息路由，路由是模块内部的事

### 问题

多个模块订阅同一个 topic 时，如何知道谁该处理这条消息？

### 决策

bus 不处理这个问题。两种解决方式由调用方自行选择：

1. **topic 区分** — 用不同的 topic（`net/request/module_a` vs `net/request/module_b`）
2. **数据内过滤** — 所有订阅者都收到，各自判断数据是否属于自己

### 原因

bus 的职责是"topic 匹配 → 触发回调"。消息路由是业务逻辑，应该在模块内部实现。比如 network 模块内部维护 `request_id → callback` 映射，对外提供 `network_request(url, cb)` 接口，但 bus 只负责把数据传到 network 模块。

---

## 决策 9：JSContext 生命周期在当前项目中不是问题

### 问题

如果 JSContext 被销毁重建，UI 桥接和 JS 桥接的订阅需要重新注册。

### 决策

不处理。当前 JSContext 只在启动时创建一次、退出时销毁一次，没有热重载。

### 原因

`kwcc_create_js()` 在 `main.m` 中只调用一次。如果未来做 JS 热重载，再处理也不迟。

---

## 决策 10：两套 Bus，独立运行

### 问题

UI 桥接和通用 bus 用同一套 topic 命名会不会冲突？

### 决策

两套系统独立运行：
- `kwcc_ui_bus` — UI→JS 桥接，topic map 在 UI 内部
- `kwcc_bus` — 通用 C Pub/Sub，subscriber 链表

它们通过 topic 字符串对齐语义，但实现完全独立。JS 侧的 `$bus` 是第三套系统，由 JS 桥接层转发。

### 原因

UI 桥接是直接 JS_Eval 发到 JS bus，不经过 C bus。非 UI 事件先走 C bus，再由 JS 桥接转发到 JS bus。两条路径不同，不能混为一套。
