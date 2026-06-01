# 方案：microui ID 覆盖机制

> 状态：待论证
> 优先级：store-data-flow-v2 完成后的下一项

## 背景问题

### 现状：所有控件用 label/value 计算 ID

```c
// window
mu_Id id = mu_get_id(ctx, title, strlen(title));
// button
mu_Id id = mu_get_id(ctx, label, strlen(label));
// slider / checkbox
mu_Id id = mu_get_id(ctx, &value, sizeof(value));  // 指针地址
```

### 三个核心问题

| 问题 | 场景 | 影响 |
|------|------|------|
| **title 动态变化** | 窗口标题随内容变化（如 "未命名" → "document.txt"） | 每帧 ID 不同，container 状态丢失（位置/大小/滚动） |
| **title/label 重复** | 两个不同窗口都叫 "设置" | 共享同一个 container 状态，互相干扰 |
| **国际化不兼容** | 中英文切换："确认" ↔ "Confirm" | 按钮 ID 不同，跨帧状态不持久化 |

### 国际化场景具体说明

假设未来要做多语言：
```javascript
// 中文模式
ui.button("确认", TOPIC.CONFIRM);  // ID = hash("确认")
// 切换英文
ui.button("Confirm", TOPIC.CONFIRM);  // ID = hash("Confirm")
```
同一个按钮，切换语言后 ID 完全变了。状态持久化、焦点、hover 全部丢失。

## 方案设计

### 核心思路

在 microui 新增 **ID 覆盖机制**：控件创建前可主动指定 ID，覆盖默认的 hash 计算。

### 改动清单

#### 1. `microui.h` — 新增字段和函数声明

```c
/* ID override: when non-zero, mu_get_id returns this instead of hash */
mu_Id id_override;

void mu_set_id_override(mu_Context *ctx, mu_Id id);
```

#### 2. `microui.c` — 修改 mu_get_id + 新增函数

```c
void mu_set_id_override(mu_Context *ctx, mu_Id id) {
    ctx->id_override = id;
}

mu_Id mu_get_id(mu_Context *ctx, const void *data, int size) {
    /* ID override: return specified id, skip hash calculation */
    if (ctx->id_override != 0) {
        mu_Id id = ctx->id_override;
        ctx->id_override = 0;  /* consume once */
        ctx->last_id = id;
        return id;
    }

    /* original logic unchanged */
    int idx = ctx->id_stack.idx;
    mu_Id res = (idx > 0) ? ctx->id_stack.items[idx - 1] : HASH_INITIAL;
    hash(&res, data, size);
    ctx->last_id = res;
    return res;
}
```

#### 3. `kwcc.c` — 所有带 topic 的 handler 中调用

```c
/* button handler */
if (topic) {
    mu_set_id_override(&g_mu, fnv1a(topic));
}
int res = mu_button_ex(&g_mu, text ? text : "", 0, MU_OPT_ALIGNCENTER);

/* beginWindow handler */
if (topic) {
    mu_set_id_override(&g_mu, fnv1a(topic));
}
mu_begin_window_ex(&g_mu, title ? title : "", rect, opt);

/* slider handler */
if (topic) {
    mu_set_id_override(&g_mu, fnv1a(topic));
}
mu_slider_ex(&g_mu, &g_slider_val, ...);
```

### JS 层

**零改动**。topic 参数已经是最后一个参数，ID 覆盖对 JS 透明。

```javascript
// ID 从 topic 稳定派生，不受 label 影响
ui.button("确认", $topics.confirm);    // 中文
ui.button("Confirm", $topics.confirm); // 英文 → 同一 ID
```

## 优势

1. **一处改动覆盖所有控件**：window/button/slider/checkbox/header/treenode 全部受益
2. **完全向后兼容**：不传 topic 的控件走原逻辑，零影响
3. **不破坏现有 ID 栈**：`id_override` 和 `id_stack` 独立
4. **ID 稳定**：topic 固定，不受 label/title 变化影响
5. **侵入最小**：只改 `mu_get_id` + 一个字段
6. **零性能开销**：多一次条件判断，无内存分配

## 风险与挑战

### 挑战 1：ID 栈嵌套行为

**问题**：`mu_get_id` 的结果受 `id_stack` 影响。当 `id_override` 非零时，我们跳过了 hash 计算，但没有考虑 `id_stack` 的作用。

**分析**：
- 当前 `mu_get_id` 会基于 `id_stack` 的顶层 hash 再叠加新的 hash
- `id_override` 直接返回固定 ID，完全跳过这个机制
- 对 button/slider 等平级控件没有影响（id_stack 为空）
- **对窗口内嵌套控件（panel/header）可能影响 ID 隔离**

**评估**：window/button/slider 都是顶层控件，不受 id_stack 影响。只有 panel/header 嵌套场景需要注意。当前项目没有使用嵌套 panel，风险可控。

### 挑战 2：容器匹配

**问题**：`mu_begin_window_ex` 用 ID 查找 container pool。如果 ID 变了，旧的 container 状态（位置/大小）会丢失。

**分析**：
- 旧 ID（基于 title）对应的 container 不会被清理，占用 pool 槽位
- 新 ID 对应的新 container 需要重新初始化
- **长期运行可能导致 container pool（48 槽）占满**

**评估**：切换 ID 只会发生一次（从 title-based 切换到 topic-based）。之后 ID 稳定，不会再变。pool 占用不是问题。

### 挑战 3：fnv1a 冲突

**问题**：不同 topic 可能 hash 冲突。

**分析**：fnv1a 冲突概率极低，且 topic 是我们控制的，可以避开。风险极低。

### 挑战 4：microui 第三方库修改

**问题**：修改 microui 核心逻辑，后续升级上游代码会冲突。

**评估**：microui 已经 fork 修改过（`on_window_close` 回调），增加一个字段和一个函数不会显著增加维护成本。可以在代码注释中标明修改点。

## 替代方案对比

| 方案 | 侵入性 | 覆盖范围 | 复杂度 | 推荐度 |
|------|--------|----------|--------|--------|
| **ID 覆盖（本方案）** | 低 | 所有控件 | 低 | ⭐⭐⭐ |
| `mu_begin_window_id` 新增函数 | 中 | 仅 window | 中 | ⭐⭐ |
| C 层 topic→ID 映射表 | 低 | 仅已映射控件 | 高（需改造容器查找） | ⭐ |
| 不改，接受 title 限制 | 无 | 无 | 无 | ⭐（仅限简单项目） |

## 待论证问题

1. **id_override 和 id_stack 的交互**：是否需要保留 id_stack 的叠加效果？还是 override 就应该完全独立？
2. **是否需要 `mu_pop_id_override`**：当前设计是"用一次清除"，是否需要在某些场景手动清除？
3. **container pool 清理**：切换 ID 后旧 container 是否需要主动清理？
4. **是否需要在 `mu_end()` 中重置 `id_override`**：防止跨帧泄漏？

## 实施步骤（确认后）

1. 修改 `microui.h` + `microui.c`
2. 修改 `kwcc.c` 中所有带 topic 的 handler
3. 写测试验证：title 变化后 ID 不变、国际化切换状态不丢失
4. `make clean && make` 验证无 warning
