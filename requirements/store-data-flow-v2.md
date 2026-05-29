# 单向数据流方案（v2 — 精简版）

## 文档概述

本文档为当前项目 **MicroUI + Sokol + mQuickJS + 自定义事件总线** 的精简版单向数据流方案。

相比 v1 的变化：
- **移除帧任务队列**：作为独立性能优化专项，不在此方案范围内
- **预处理层允许只读 state**：校验/转换经常需要参考当前状态
- **Action 允许派生状态计算**：不局限于纯赋值
- **C 层用 JSValue 替代 malloc payload 结构体**：避免手动内存管理
- **Topic 映射在 C 层**：控件创建时绑定 topic，JS 层无需写映射代码
- **简化 MQTT 通配符**：精确匹配 + `*` 末尾通配，不做完整 MQTT

---

## 一、架构核心思想

### 1. 设计目标

1. **彻底解耦**：视图 / 事件 / 预处理 / 状态完全隔离；
2. **视图唯一入口**：所有 UI 常驻控件、动态弹窗、浮层全部由 `onFrame` 统一渲染；
3. **状态唯一入口**：所有状态变更只能通过 `dispatch -> Action`；
4. **低侵入 MicroUI**：废弃多回调，全局单一通用回调，零控件专属回调；
5. **事件可控**：层级主题 + 末尾通配，C 层绑定 topic，JS 层无映射代码；
6. **统一规范**：弹窗、IO、网络、计算全部有固定标准写法。

### 2. 五层架构（固定分层，禁止跨层）

从上至下：**视图层 → 事件层 → 预处理层 → Action 层 → Store 状态层**

```
用户操作控件
    ↓
MicroUI → C层全局唯一回调
    ↓
C层用JSValue构造 {topic, action, data} → 调用JS统一入口
    ↓
JS EventBus按topic分发（精确匹配 + *末尾通配）
    ↓
预处理层（只读state，数据校验/转换/过滤）
    ↓
dispatch 提交动作
    ↓
Action（赋值 + 派生状态计算）
    ↓
Store.state 更新
    ↓
下一帧 onFrame 根据状态全自动渲染
```

---

## 二、统一事件格式

**所有控件事件统一使用三段式结构：**

```
{
  topic: "面板/控件类型/别名",   // 来源定位
  action: "动作类型",            // click / change / input / submit / focus / blur
  data: { ... }                  // 控件专属数据，自由扩展
}
```

不同控件的 `data`：

| 控件 | data 内容 |
|------|----------|
| button | `{}` |
| checkbox | `{ checked: bool, label: string }` |
| slider | `{ value: float, min: float, max: float }` |
| textbox | `{ value: string, submitted: bool }` |
| select | `{ index: int, value: string }` |
| svg | `{ cache_idx: int, width: int, height: int }` |

### C 层统一构造

```c
static JSValue make_event(const char *topic, const char *action, JSValue data) {
    JSValue ev = JS_NewObject(js_ctx);
    JS_SetPropertyStr(js_ctx, ev, "topic", JS_NewString(js_ctx, topic));
    JS_SetPropertyStr(js_ctx, ev, "action", JS_NewString(js_ctx, action));
    JS_SetPropertyStr(js_ctx, ev, "data", data);
    return ev;
}

// checkbox 调用示例
JSValue data = JS_NewObject(js_ctx);
JS_SetPropertyStr(js_ctx, data, "checked", JS_NewBool(js_ctx, checked));
JS_SetPropertyStr(js_ctx, data, "label", JS_NewString(js_ctx, label));
JS_Call(js_ctx, js_on_event, JS_UNDEFINED, 1,
    &make_event(topic, "change", data));
```

JS EventBus 永远用同一个解构方式处理：

```javascript
function onEvent(ev) {
    $bus.emit(ev.topic, ev.action, ev.data);
}
```

这样以后加新控件，C 层只管构造自己的 `data`，分发逻辑零改动。

---

## 三、五层分层详细职责 & 红线规范

### 1. 视图层（JS onFrame）

**定位：纯渲染层，界面唯一入口**

**允许**
1. 只读读取 `$store.state`；
2. 绘制常驻 UI 控件，创建控件时在 C 层绑定 topic；
3. 根据 state 布尔字段**条件渲染弹窗 / 浮层 / 动态面板**；

**禁止（红线）**
1. 禁止直接修改任何 state；
2. 禁止在视图内订阅任何事件；
3. 禁止编写业务判断、数据计算；
4. 禁止在事件 / Action 内手动创建、销毁 UI；
5. 禁止跨层调用 Action/EventBus。

### 2. 事件层（EventBus）

**定位：事件分发、隔离、节流**

1. 事件统一为 `{topic, action, data}` 三段式；
2. 支持精确匹配 + `*` 末尾通配（如 `ui/main/*` 匹配 `ui/main/btn/click`）；
3. 支持节流（如 slider 拖拽时限制分发频率）；

**禁止**
1. 禁止手写硬编码 topic 字符串，全部使用常量；
2. 禁止滥用全局通配造成事件泛滥。

### 3. 预处理层（订阅回调）

**定位：纯数据处理层，事件与状态中间桥梁**

**允许**
1. 数值限制、单位换算、字符串脱敏 / 截断；
2. 调用纯工具函数；
3. 简单分支判断；
4. **只读**访问 `$store.state` 用于校验/转换；
5. 调用 dispatch 更新状态。

**禁止（红线）**
1. **禁止写** state（只能通过 dispatch 改）；
2. 禁止调用任何 UI 绘制接口；
3. 禁止直接执行 IO、同步文件、密集运算；
4. 禁止创建弹窗、控件、面板；
5. 禁止业务主逻辑堆积。

### 4. Action 层

**定位：全局唯一状态写入层**

**允许**
- 纯赋值操作：普通数据、弹窗显隐标记、弹窗文案、配置项；
- **派生状态计算**：如 toggleAll 需要遍历列表计算每个子项状态。

**禁止（红线，最重要）**
1. 禁止编写复杂业务逻辑（应放在预处理层或独立模块）；
2. 禁止调用 EventBus、emit 事件；
3. 禁止创建 / 销毁弹窗、控件；
4. 禁止执行文件、网络、同步 IO；
5. 禁止直接操作视图。

### 5. Store 状态层

**定位：全局单一数据源**

1. 存放所有业务数据；
2. 存放所有**动态界面状态**（弹窗显隐、Tab 激活、浮层配置）；
3. 支持中间件：日志、快照、持久化、全局拦截。

---

## 四、C 层架构（JSValue 载荷 + 统一全局回调）

### 1. 废弃旧机制

彻底删除所有控件专属回调。

### 2. 全局唯一回调（全项目仅此一个）

```c
static void ui_global_event_cb(void *userData, JSValue data_obj);
```

所有控件统一绑定该回调，通过 `userData` 传递 topic 字符串指针。

### 3. 控件绑定方式（C 层存储 topic）

microui 是立即模式，控件没有持久化对象。C 层维护一个 `id -> topic` 的映射表：

```c
// C 层维护
static struct { mu_Id id; char topic[128]; } g_topic_map[256];
```

控件绘制后通过 `mu_get_id()` 获取刚生成的 ID，存入映射表。

### 4. JSValue 代替 malloc payload

不需要 `malloc` + `strcpy` 的 C 结构体 payload，直接用 JSValue：

```c
// 构造事件
JSValue data = JS_NewObject(ctx);
JS_SetPropertyStr(ctx, data, "checked", JS_NewBool(ctx, checked));
JS_SetPropertyStr(ctx, data, "label", JS_NewString(ctx, label));

JSValue ev = make_event(topic, "change", data);
JS_Call(ctx, js_on_event, JS_UNDEFINED, 1, &ev);
JS_FreeValue(ctx, ev);
JS_FreeValue(ctx, data);
```

优势：
- 零手动内存管理（JS GC 自动回收）
- 新控件加字段只需多加几行 `JS_SetPropertyStr`
- 不需要改 C 结构体定义
- 支持任意长度字符串

### 5. 各控件的 data 构造示例

```c
// button → data = {}
JSValue data = JS_NewObject(ctx);
kw_dispatch_event(topic, "click", data);

// checkbox → data = { checked, label }
JSValue data = JS_NewObject(ctx);
JS_SetPropertyStr(ctx, data, "checked", JS_NewBool(ctx, checked));
JS_SetPropertyStr(ctx, data, "label", JS_NewString(ctx, label));
kw_dispatch_event(topic, "change", data);

// slider → data = { value, min, max }
JSValue data = JS_NewObject(ctx);
JS_SetPropertyStr(ctx, data, "value", JS_NewFloat64(ctx, val));
JS_SetPropertyStr(ctx, data, "min", JS_NewFloat64(ctx, min));
JS_SetPropertyStr(ctx, data, "max", JS_NewFloat64(ctx, max));
kw_dispatch_event(topic, "change", data);

// textbox → data = { value, submitted }
JSValue data = JS_NewObject(ctx);
JS_SetPropertyStr(ctx, data, "value", JS_NewString(ctx, text));
JS_SetPropertyStr(ctx, data, "submitted", JS_NewBool(ctx, submitted));
kw_dispatch_event(topic, "input", data);
```

---

## 五、动态界面 / 弹窗统一规范

### 1. 核心铁律

**弹窗、浮层、提示框、动态面板、Tab 切换，不属于 Action、不属于订阅层**

> 所有动态界面：**状态驱动，视图渲染**

### 2. 开发固定公式

1. State：增加布尔字段（visible）+ 附属数据（标题 / 内容）
2. Action：赋值 visible 字段 + 派生数据，不操作 UI
3. 订阅层：接收事件 → dispatch Action
4. 视图层 onFrame：if (visible) 绘制弹窗

### 3. 完整标准示例

#### State & Action

```javascript
state: {
    dialogVisible: false,
    dialogTitle: "提示",
    dialogMsg: ""
},
actions: {
    openDialog: function(state, msg) {
        state.dialogVisible = true;
        state.dialogMsg = msg;
    },
    closeDialog: function(state) {
        state.dialogVisible = false;
    }
}
```

#### 事件订阅

```javascript
$bus.on(TOPIC.MAIN_OPEN_DIALOG, function(action, data) {
    $store.dispatch("openDialog", "你已完成操作");
});
$bus.on(TOPIC.DIALOG_CLOSE, function(action, data) {
    $store.dispatch("closeDialog");
});
```

#### 视图层（唯一创建弹窗位置）

```javascript
function onFrame() {
    var s = $store.state;
    ui.button("打开弹窗", 20, 20, 100, 30, TOPIC.OPEN_DIALOG);

    // 状态驱动渲染弹窗
    if (s.dialogVisible) {
        ui.panel(80, 80, 320, 200);
        ui.label(s.dialogTitle, 100, 100);
        ui.label(s.dialogMsg, 100, 130);
        ui.button("关闭", 220, 230, 80, 30, TOPIC.CLOSE_DIALOG);
    }
}
```

---

## 六、分层权限总表

|层级|能读 State|能写 State|能创建 UI|能调 EventBus|能执行业务计算|
|---|---|---|---|---|---|
|视图层|✅ 只读|❌|✅ 仅渲染|❌|❌|
|预处理层|✅ 只读|❌|❌|✅ emit|✅ 轻量|
|Action 层|✅ 读写|✅ 赋值+派生|❌|❌|✅ 派生计算|
|Store|/|✅|❌|❌|❌|

---

## 七、完整场景汇总

1. **简单按钮**：C回调 → EventBus 分发 → 预处理 → dispatch → Action 改状态 → 视图刷新
2. **弹窗 / 浮层**：C回调 → EventBus → 预处理 → dispatch → Action 改 visible → 视图条件渲染
3. **滑块 / 输入框**：C回调带 value → EventBus → 预处理校验（可读 state 对比） → dispatch → 更新状态
4. **派生状态**（如全选）：C回调 → EventBus → 预处理判断 → dispatch → Action 遍历计算子项状态

---

## 八、避坑清单

1. 所有弹窗、动态控件**只能在 onFrame 条件渲染**；
2. Action 永远不创建 UI、不做 IO；
3. 禁止手写事件 Topic，全部使用常量；
4. C 层用 JSValue 构造事件数据，不 malloc payload 结构体；
5. topic 映射存在 C 层，JS 层不写映射代码；
6. 禁止在 onFrame 内编写复杂 if/else 业务逻辑；
7. 预处理层只读 state，不能直接写。

---

## 九、JS 项目目录结构

```
runtime/
    bus.js          // EventBus（精确匹配 + *末尾通配 + 节流）
    store.js        // 全局状态 + dispatch + 中间件
constant/
    topic.js        // 所有 topic 常量
utils/
    common.js       // 通用纯函数
action/
    action_main.js  // 模块化 Action
view/
    render_main.js  // onFrame 渲染逻辑
event/
    event_init.js   // 统一注册所有订阅
main.js             // 入口、帧循环
```

---

## 十、未来扩展：帧任务队列（不在本方案范围）

当需要处理网络 I/O、大文件读取等耗时操作时，作为独立性能优化专项引入。

核心思路：
- 预处理层不直接执行耗时逻辑，改为推入队列
- C 层非阻塞 I/O（kqueue/epoll），回调结果入主线程队列
- 每帧 `onFrame` 末尾消费队列，限制单帧执行量
- 网络回调不直接写 state，走 `dispatch` 保持单向流

---

## 十一、落地实施顺序（推荐）

1. **Store + dispatch**：最小核心，`$store.state` + `$store.dispatch()` + Action 系统
2. **EventBus**：精确匹配 + `*` 末尾通配，`$bus.on()` / `$bus.emit()`
3. **C 层统一事件分发**：JSValue 构造 `{topic, action, data}`，全局单回调
4. **迁移计算器示例**：用新方案重写计算器，验证闭环
5. **弹窗状态驱动**：用 state 控制弹窗显隐
6. **未来**：帧任务队列作为独立专项引入
