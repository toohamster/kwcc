# 单向数据流方案（v1）

## 文档概述

本文档为当前项目 **MicroUI \+ Sokol \+ mQuickJS \+ 自定义事件总线** 。
整合全部讨论内容：分层规范、MQTT 层级事件、C 层统一通用回调、无侵入 UI 绑定、单向数据流、弹窗动态界面规范、**帧任务队列异步机制**、编码红线、完整链路、落地流程。

## 一、架构核心思想

### 1\. 设计目标

1. **彻底解耦**：视图 / 事件 / 业务预处理 / 状态完全隔离；

2. **视图唯一入口**：所有 UI 常驻控件、动态弹窗、浮层全部由 `onFrame` 统一渲染；

3. **状态唯一入口**：所有状态变更只能通过 `dispatch \-\&gt; Action`；

4. **低侵入 MicroUI**：废弃多回调，全局单一通用回调，零控件专属回调；

5. **事件可控**：MQTT 层级主题 \+ 通配符，解决同控件区分、局部事件隔离；

6. **主线程零卡顿**：通过帧任务队列分片执行所有耗时任务，保护帧率；

7. **统一规范**：弹窗、IO、网络、计算全部有固定标准写法。

### 2\. 五层架构（固定分层，禁止跨层）

从上至下：**视图层 → 事件层 → 预处理层 → Action 层 → Store 状态层**
附加底层支撑：**帧任务队列**（全局公共基础设施）

### 3\. 标准完整数据流（终极闭环）

#### 3\.1 普通交互（按钮 / 滑块 / 输入框）

```Plain Text
用户操作控件
    ↓
MicroUI触发交互 → C层全局唯一通用回调
    ↓
C层解析载荷 → 通用派发函数 → MQTT层级主题emit
    ↓
JS EventBus分发事件（支持节流/通配符/分组）
    ↓
预处理层（数据校验/转换/过滤）
    ↓
dispatch 提交动作
    ↓
Action（仅赋值状态）
    ↓
Store.state 更新
    ↓
onFrame 视图层根据状态全自动渲染
```

#### 3\.2 耗时任务数据流（IO / 文件 / 网络 / 复杂计算）

```Plain Text
用户操作触发事件
    ↓
预处理层 禁止直接执行耗时逻辑
    ↓
推入【帧任务队列】
    ↓
每帧尾部分片执行（限制单次任务数，不阻塞主线程）
    ↓
任务执行完毕
    ↓
dispatch 更新结果至Store
    ↓
视图层刷新UI
```

## 二、五层分层详细职责 \&amp; 红线规范

### 1\. 视图层（JS onFrame）

**定位：纯渲染层，界面唯一入口**
**允许**

1. 只读读取 `$store\.state`；

2. 绘制常驻 UI 控件；

3. 根据 state 布尔字段**条件渲染弹窗 / 浮层 / 动态面板**；

4. 创建控件时绑定 MQTT Topic 前缀。

**禁止（红线）**

1. 禁止直接修改任何 state；

2. 禁止在视图内订阅任何事件；

3. 禁止编写业务判断、数据计算；

4. 禁止在事件 / Action 内手动创建、销毁 UI；

5. 禁止跨层调用 Action/EventBus。

### 2\. 事件层（MQTT EventBus）

**定位：事件分发、隔离、节流、生命周期管理**

1. 事件格式：`ui/面板/控件类型/别名/动作` / `system/xxx`

2. 支持 `\+` 单层通配、`\#` 多层通配（仅末尾）

3. 支持事件节流、分组解绑（解决动态组件内存泄漏）

**禁止**

1. 禁止手写硬编码事件字符串，全部使用常量；

2. 禁止滥用全局通配 `ui/\#` 造成事件泛滥。

### 3\. 预处理层（订阅回调）

**定位：纯数据处理层，事件与状态中间桥梁**
**允许**

1. 数值限制、单位换算、字符串脱敏 / 截断；

2. 调用纯工具函数；

3. 简单分支判断；

4. 调用 dispatch 更新状态；

5. 将耗时任务推入帧任务队列。

**禁止（红线）**

1. 禁止直接读写 state；

2. 禁止调用任何 UI 绘制接口；

3. 禁止直接执行 IO、同步文件、密集运算；

4. 禁止创建弹窗、控件、面板；

5. 禁止业务主逻辑堆积。

### 4\. Action 层

**定位：全局唯一状态写入层**
**允许**
仅做**纯赋值操作**：普通数据、弹窗显隐标记、弹窗文案、配置项。

**禁止（红线，最重要）**

1. 禁止编写任何业务计算逻辑；

2. 禁止调用 EventBus、emit 事件；

3. 禁止创建 / 销毁弹窗、控件；

4. 禁止执行文件、网络、同步 IO；

5. 禁止直接操作视图。

### 5\. Store 状态层

**定位：全局单一数据源**

1. 存放所有业务数据；

2. 存放所有**动态界面状态**（弹窗显隐、Tab 激活、浮层配置）；

3. 支持中间件：日志、快照、持久化、全局拦截。

## 三、新增：帧任务队列（架构底层基础设施）

### 1\. 解决的问题

1. 防止大计算、文件 IO、网络请求阻塞帧循环造成卡顿；

2. 统一收敛所有耗时逻辑，全局一处管控；

3. 固定每帧执行上限，保证帧率稳定。

### 2\. 核心设计规则

1. 所有**耗时任务**统一放入队列，绝不同步执行；

2. 在 Sokol 帧循环**末尾**统一执行；

3. 限制单帧最大执行任务数量，防止占满单帧时间片；

4. 队列 FIFO 执行，支持普通优先级。

### 3\. 完整可直接使用代码

```javascript
// frame_task.js 全局帧任务队列
const FrameTask = {
    queue: [],
    // 添加异步任务
    add(fn) {
        if(typeof fn === "function"){
            this.queue.push(fn);
        }
    },
    // 每帧执行入口，由主onFrame末尾调用
    run(){
        const limit = 2; // 每帧最多执行2个耗时任务（可配置）
        let count = 0;
        while(this.queue.length > 0< limit){
            const task = this.queue.shift();
            try{
                task();
            }catch(e){
                console.error("帧任务执行异常：",e);
            }
            count++;
        }
    }
};
globalThis.FrameTask = FrameTask;
```

### 4\. 调用位置

在全局帧循环最后一行调用：

```javascript
function onFrame(){
    // 1. 视图渲染所有UI
    renderUI();
    // 2. 执行分片耗时任务
    FrameTask.run();
}
```

### 5\. 使用范例

```javascript
// 订阅层：耗时IO全部推入队列
$bus.on(TOPIC.BTN_LOAD_FILE,()=>{
    FrameTask.add(()=>{
        const data = readBigFile("data.json");
        $store.dispatch("setFileData",data);
    });
});
```

## 四、动态界面 / 弹窗统一规范（解决你之前疑问）

### 1\. 核心铁律

**弹窗、浮层、提示框、动态面板、Tab 切换，不属于 Action、不属于订阅层**

> 所有动态界面：**状态驱动，视图渲染**
> 
> 

### 2\. 开发固定公式

1. State：增加布尔字段（visible）\+ 附属数据（标题 / 内容）

2. Action：仅修改 visible 字段，不操作 UI

3. 订阅层：接收事件 → dispatch Action

4. 视图层 onFrame：if \(visible\) 绘制弹窗

### 3\. 完整标准示例

#### 3\.1 State \&amp; Action

```javascript
state:{
    dialogVisible: false,
    dialogTitle: "提示",
    dialogMsg: ""
},
actions:{
    openDialog(state,msg){
        state.dialogVisible = true;
        state.dialogMsg = msg;
    },
    closeDialog(state){
        state.dialogVisible = false;
    }
}
```

#### 3\.2 事件订阅

```javascript
$bus.on(TOPIC.MAIN_OPEN_DIALOG,()=>{
    $store.dispatch("openDialog","你已完成操作");
});
$bus.on(TOPIC.DIALOG_CLOSE,()=>{
    $store.dispatch("closeDialog");
});
```

#### 3\.3 视图层（唯一创建弹窗位置）

```javascript
function onFrame(){
    const s = $store.state;
    ui.button("打开弹窗",20,20,100,30,"ui/main/btn/open/click");

    // 状态驱动渲染弹窗
    if(s.dialogVisible){
        ui.panel(80,80,320,200);
        ui.label(s.dialogTitle,100,100);
        ui.label(s.dialogMsg,100,130);
        ui.button("关闭",220,230,80,30,"ui/dialog/btn/close/click");
    }
}
```

### 4\. 为什么 Action 不能直接创建弹窗？

1. 破坏**视图唯一渲染入口**；

2. UI 分散创建，帧状态不同步，极易出现残影、卡死、闪退；

3. 打破单向数据流，变成双向混乱；

4. 无法做热重载、脏标记、界面快照。

## 五、C 层终极架构（零侵入、单一全局回调）

### 1\. 废弃旧机制

彻底删除所有控件专属回调：`kw\_ui\_on\_button\_click`、`kw\_ui\_on\_input\_text`。

### 2\. 统一载荷结构体

```c
typedef enum {
    EVENT_ARG_NONE,
    EVENT_ARG_FLOAT,
    EVENT_ARG_STRING
} EventArgType;

typedef struct {
    char topic_prefix[256];
    char action[32];
    EventArgType arg_type;
} ControlEventPayload;
```

### 3\. 全局唯一回调（全项目仅此一个）

所有控件统一绑定该回调，无新增回调成本。

```c
static void ui_global_event_cb(void* userData, float fVal, const char* sVal);
```

### 4\. 三套通用派发函数

```c
void kw_dispatch_event_no_arg(const char* prefix, const char* suffix);
void kw_dispatch_event_float(const char* prefix, const char* suffix, float val);
void kw_dispatch_event_string(const char* prefix, const char* suffix, const char* text);
```

### 5\. 控件创建流程

1. 创建原生 MicroUI 控件；

2. malloc 分配载荷，绑定 topic/action/ 类型；

3. 绑定全局唯一回调；

4. 存入控件映射表；

5. 控件销毁自动释放 payload 内存。

## 六、JS 项目目录标准结构

```Plain Text
runtime/
    bus.js          // MQTT事件总线
    store.js        // 全局状态+中间件
    frame_task.js   // 帧任务队列
constant/
    topic.js        // 所有主题常量
    event.js        // 系统事件常量
utils/
    ui_utils.js     // UI纯工具函数
    common.js       // 通用纯函数
action/             // 模块化Action
    action_main.js
    action_popup.js
view/
    render_main.js
event/
    event_init.js   // 统一注册所有订阅
main.js             // 入口、帧循环
```

## 七、分层权限总表（终极红线）

|层级|能否改 State|能否创建 UI|能否执行业务计算|能否执行 IO / 耗时|
|---|---|---|---|---|
|视图层|❌|✅\(仅渲染\)|❌|❌|
|预处理层|❌|❌|✅\(轻量\)|❌\(推入队列\)|
|Action 层|✅\(仅赋值\)|❌|❌|❌|
|Store|/|❌|❌|❌|
|帧任务队列|❌|❌|✅\(重度\)|✅|

## 八、完整场景汇总（所有业务全覆盖）

1. **简单按钮**：事件→预处理→dispatch→Action 改普通状态→视图刷新

2. **弹窗 / 浮层**：事件→预处理→dispatch→Action 改 visible→视图条件渲染

3. **滑块 / 输入框**：MQTT 主题事件→预处理校验→dispatch→更新数据状态

4. **文件 / 网络 IO**：事件→推入帧任务→异步执行→dispatch 回填结果→视图刷新

5. **复杂计算**：事件→推入帧任务→分片运算→更新状态

## 九、新增避坑清单（补充帧队列 \+ 弹窗）

1. 所有弹窗、动态控件**只能在 onFrame 条件渲染**；

2. Action 永远不创建 UI、不做 IO、不做业务逻辑；

3. 任何耗时 \&gt; 5ms 的逻辑必须推入 FrameTask；

4. 禁止手写事件 Topic，全部使用常量；

5. 动态弹窗组件必须使用 onGroup/offGroup 解绑；

6. C 层所有 malloc 的 Payload 必须随控件销毁释放；

7. 禁止在帧任务内直接操作 UI，只能更新 State；

8. 禁止在 onFrame 内编写复杂 if/else 业务逻辑。

## 十、落地实施顺序（推荐）

1. 新增帧任务队列底层代码，主循环接入 run \(\)；

2. 完成 C 层全局载荷 \+ 唯一回调 \+ 通用派发；

3. 迁移所有控件，删除旧具名回调；

4. JS 完善 Topic 常量、模块化 Action；

5. 统一所有弹窗改为「状态驱动视图」模式；

6. 所有旧项目耗时逻辑逐步迁移至帧队列。
