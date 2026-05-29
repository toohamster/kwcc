# 开发计划：store-data-flow-v2 实施方案

## 当前状态分析

| 维度 | 现状 | 目标 |
|------|------|------|
| 目录结构 | 仅 `app/main.js` + `app/examples/` | `runtime/` + `constant/` + `modules/` + `utils/` |
| 状态管理 | JS 全局变量（`var display = "0"`） | `$store.state` + `createStore` |
| 事件流 | `if (ui.button("7")) { _digit(7); }` | topic 绑定 → C 回调 → EventBus → dispatch → Action |
| 模块组织 | 无注册机制 | `registerModule` + `registerModuleView` |
| C 层事件 | 无统一事件回调 | 全局唯一回调 + `id -> topic` 映射 + JSValue 构造 |
| 帧循环 | `kwcc_process_js` 直接 eval | `onFrame` 遍历模块 render |

## 实施阶段

### 阶段一：运行时骨架（纯 JS）

**目标**：创建 `runtime/store.js` + `runtime/bus.js` + `constant/topic.js`，验证 Store + EventBus 可工作

**新增文件**：
- `app/runtime/store.js` — `createStore`（双参数 dispatch：`dispatch(module, actionName, payload)`）
- `app/runtime/bus.js` — EventBus（精确匹配 + `*` 末尾通配 + `onGroup/offGroup`）
- `app/constant/topic.js` — TOPIC 常量

**验收**：
- 在 mquickjs 下 `load()` 可正常加载
- `$store.state` / `$store.dispatch("module", "action", data)` 可工作
- `$bus.on()` / `$bus.emit()` 可工作

**暂不改动**：现有 `app/main.js` 和 `app/examples/` 不动

---

### 阶段二：模块注册框架

**目标**：创建 `registerModule` + `registerModuleView` 框架，改写 `app/main.js` 为入口调度

**新增/修改文件**：
- `app/main.js` — 改为 `modules` / `moduleKeys` 注册表 + `initStore()` / `initEvents()` / `onFrame()` 遍历
- `app/runtime/framework.js` — 包含 `registerModule` + `registerModuleView` 的框架代码（也可放在 main.js 里）

**验证方式**：写一个最小模块验证闭环
```javascript
// app/modules/test.js
registerModule("test", {
    state: { count: 0 },
    actions: { increment: function(s) { s.count = s.count + 1; } },
    initEvents: function() {
        $bus.onGroup("test", TOPIC.TEST_BTN, function(action, data) {
            $store.dispatch("test", "increment");
        });
    },
    cleanup: function() { $bus.offGroup("test"); }
});

// app/modules/test_view.js
registerModuleView("test", function(s) {
    ui.beginWindow("Test", 50, 50, 200, 100, 0);
    ui.label("Count: " + s.count);
    ui.button("Increment", TOPIC.TEST_BTN);
    ui.endWindow();
});
```

---

### 阶段三：C 层统一事件分发

**目标**：改造 C 层，新增全局回调 + `id -> topic` 映射 + JSValue 事件构造

**修改文件**：
- `src/kwcc.c` — 新增：
  1. `g_topic_map[256]` + `g_topic_map_count`（Zero-Alloc 单帧覆盖）
  2. `kwcc_bind_topic(mu_Id id, const char *topic)` — 绘制时绑定
  3. `kwcc_begin_frame()` — 每帧清零 `g_topic_map_count`
  4. `kwcc_dispatch_event(topic, action, data)` — 构造 JSValue 并调用 `js_on_event`
  5. 修改 `ui.button()` 等 handler 支持 topic 参数

- `src/kwcc.h` — 新增公共 API 声明

- `src/main.m` — 在 `frame()` 开头调用 `kwcc_begin_frame()`

**关键设计**：
- `topic` 为最后一个参数，所有控件**强制传入**，不传视为调用错误
- JS 侧 `ui.button("确认", TOPIC.CONFIRM)` 调用时，C 层绘制后调用 `kwcc_bind_topic(mu_get_id(), topic)`
- 用户点击时，C 层全局回调根据 ID 查 topic，构造 `{topic, action, data}` JSValue 调用 `js_on_event`
- 修改 `js_ui_dispatch` 各 handler，从最后一个参数读取 topic 字符串

**验收**：
- 带 topic 的按钮点击后能正确触发 `$bus.emit`
- 所有交互控件都传 topic，旧例代码（`if (ui.button(...))`）替换为 `ui.button(text, topic)` 写法

---

### 阶段四：迁移计算器示例

**目标**：将 `app/examples/calculator/` 改造为 `app/modules/calc.js` + `app/modules/calc_view.js`

**新增文件**：
- `app/modules/calc.js` — 计算器 state + actions + initEvents
- `app/modules/calc_view.js` — 计算器 view（registerModuleView）
- `app/constant/topic.js` — 补充计算器相关 TOPIC 常量

**删除文件**：
- `app/examples/calculator/main.js`（合并到 calc_view.js）
- `app/examples/calculator/calc_logic.js`（合并到 calc.js）

**验证闭环**：
- `app/main.js` 加载 `calc.js` + `calc_view.js`
- `initStore()` + `initEvents()` 后，每帧 `onFrame()` 遍历 render
- 点击按钮 → C 回调 → EventBus → dispatch → Action → state 更新 → 下一帧刷新
- 保持现有计算器功能完整（四则运算、小数点、清除等）

---

### 阶段五：验证与清理

**目标**：确保 SVG 示例不受影响，清理旧代码

**验证**：
- SVG 示例窗口正常渲染（不受事件系统重构影响）
- `make clean && make` 无 warning
- 运行无崩溃

---

### 阶段六（未来）：弹窗状态驱动

独立专项，在计算器迁移验证成功后实施。

---

### 阶段七（未来）：帧任务队列

作为独立性能优化专项引入。

---

## 风险与注意事项

1. **mquickjs ES5 语法**：不能使用 `let`/`const`/箭头函数/模板字符串，所有 JS 必须 `var`
2. **`{}` 语句歧义**：mquickjs 会把语句开头的 `{}` 解析为 block，需用 `var x = new Object();`
3. **`for...in` 不可靠**：必须用 `moduleKeys` 数组记录顺序
4. **C 层不能 include mquickjs.h 两次**：注意头文件包含顺序
5. **每阶段独立可验证**：每个阶段完成后必须 `make && ./kwcc` 验证，不能跨阶段合并提交
