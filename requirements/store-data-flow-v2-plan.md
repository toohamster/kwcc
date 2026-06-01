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

### 阶段一：运行时骨架（纯 JS）✅ 已完成

**目标**：创建 `runtime/store.js` + `runtime/bus.js` + `constant/topic.js`，验证 Store + EventBus 可工作

**已完成**：
- `app/runtime/store.js` — `createStore`（双参数 dispatch）
- `app/runtime/bus.js` — EventBus（精确匹配 + `*` 末尾通配 + `onGroup/offGroup`）
- `app/constant/topic.js` — TOPIC 常量
- `app/main.js` — 改为 init 时 eval 一次注册，frame 时只调 `onFrame()`
- `app/modules/test.js` / `app/modules/test_view.js` — 最小测试模块

**验证结果**：
- `render: 21 commands` — Test Counter 窗口正常渲染
- init 只执行一次，frame 纯渲染
- `$store.dispatch("test", "increment")` 机制就绪（需阶段三 C 层支持后触发）

**暂不改动**：计算器示例暂不迁移

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

### 阶段三：C 层统一事件分发 ✅ 已完成

**目标**：改造 C 层，新增全局回调 + `id -> topic` 映射 + JSValue 事件构造

**已完成**：
- `src/kwcc.c` — 新增：
  1. `g_topic_map[256]` + `g_topic_map_count`（Zero-Alloc 单帧覆盖）
  2. `kwcc_begin_frame()` — 每帧清零 `g_topic_map_count`（在 `kwcc_process_js` 中调用）
  3. `kwcc_bind_topic(mu_Id id, const char *topic)` — 绘制时绑定
  4. `kwcc_dispatch_event()` — JS_Eval 调用 `$bus.emit()`
  5. 修改 button handler 支持 topic 参数
- `src/kwcc.h` — 新增 `kwcc_begin_frame()` 声明
- `app/constant/topic.js` — 补充 TEST_RESET 常量
- `app/modules/test.js` — 补充 reset 事件处理

**验证结果**：
- 点击 "+" 按钮 count 从 0 变为 1，闭环通
- `$bus.emit()` → 订阅回调 → `$store.dispatch` → Action → state 更新 → 渲染 全链路正常

---

### 阶段四：topic 全覆盖 + C 层窗口挡板 + microui X close 回调 🔄 进行中

**目标**：所有交互控件统一 topic 参数 + C 层可见性挡板 + microui X 关闭事件外化

**核心设计**：
- 所有带交互的控件，**最后一个参数固定为 topic**（与 v2 方案一致）
- C 层 `ui.sync(key, visible)` 同步模块状态，`beginWindow` 用 sync 设置的上下文查可见性

**topic 参数范围**：

| 函数 | 参数变化 |
|------|---------|
| `ui.button(text, topic)` | 已有，不需改 |
| `ui.beginWindow(title, x, y, w, h, opt, topic)` | 加第 7 个参数 topic |
| `ui.slider(text, value, min, max, topic)` | 加第 5 个参数 topic |

**修改文件**：
- `deps/microui/microui.h` — `mu_Context` 新增 `on_window_close` 回调
- `deps/microui/microui.c` — `cnt->open = 0` 替换为 `ctx->on_window_close(ctx, title)`
- `src/kwcc.c`：
  1. C 层挡板：`g_win_intercepted[32]` + `g_win_top` — 记录 beginWindow 是否被拦截
  2. `ui.sync(key, state)` — 同步 JS 端模块状态，设置 `g_current_mod_key` 上下文
  3. `beginWindow` handler：读第 7 个参数存为窗口 topic，查 `g_current_mod_key` 可见性，visible=0 时标记拦截、跳过 microui 调用
  4. `endWindow` handler：检查 g_win_intercepted，只对未拦截的窗口调用 mu_end_window
  5. `slider` handler：读第 5 个参数 topic，value 变化时 dispatch
  6. `on_window_close` 回调实现：dispatch 窗口 topic → JS state.visible = 0
  7. `kwcc_dispatch_event()` — JS_Eval 调用 `$bus.emit()`

**JS 层配合**：
- `onFrame` 框架在 render 前自动调 `ui.sync(key, $store.state[key].visible)`
- view 函数：`beginWindow` 和 `slider` 都传 topic
- `endWindow` 正常调用，C 层自动配对

**验证**：
- 点击 X → C 回调 → JS state.visible = 0 → 下帧窗口消失，不崩溃
- 拖拽 slider → C 层 dispatch topic → JS state 更新 → 下一帧渲染新值
- visible=1 重新出现时，窗口正常渲染

### 阶段五：迁移计算器示例

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

### 阶段六：验证与清理

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
