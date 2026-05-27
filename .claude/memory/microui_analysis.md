# Microui 核心机制分析

> 基于 deps/microui/microui.c + microui.h 源码分析

## 1. Layout 系统

### mu_layout_row(items, widths, height)

```c
void mu_layout_row(mu_Context *ctx, int items, const int *widths, int height)
```

- **`items == 0`**: 不定义 per-item 宽度，每个控件拿 `layout->size.x`（默认 68）。`item_index` 不会自增，所有控件宽度一致。
- **`items > 0`**: 布局引擎循环使用 `widths` 数组。当 `item_index == items` 时自动换行（重置 position.x/y）。注意：换行时 `widths` 传 NULL，layout struct 内已有缓存。
- **`widths[i] == -1`**: "填充剩余空间"。在 `mu_layout_next` 中：`res.w += layout->body.w - res.x + 1`

### mu_layout_next() 返回 rect 的计算过程（line 578-624）

1. 先检查 `next_type`：如果调用了 `mu_layout_set_next`，用其 rect 覆盖
2. 如果 `item_index == items`，触发换行（重置位置到下一行）
3. `res.x = layout->position.x`, `res.y = layout->position.y`
4. `res.w` = `widths[item_index]` 或 `layout->size.x`（默认 68）
5. `res.h` = `layout->size.y`
6. 零值补默认样式大小，负值补剩余空间
7. `position.x += res.w + spacing`（水平移动）
8. `next_row = max(next_row, res.y + res.h + spacing)`（记录行底）
9. **加上 body 偏移**: `res.x += body.x`, `res.y += body.y`（窗口滚动偏移在此处理）

### mu_layout_set_next(rect, relative)

- **`relative = 0` (ABSOLUTE)**: 跳过布局引擎，直接返回 rect。不推进位置、不加 body 偏移。用于精确定位控件。
- **`relative = 1` (RELATIVE)**: 用自定义 rect，但仍参与布局流（推进位置、加 body 偏移）。

## 2. Widget ID 生成（fnv-1a 哈希）

```c
mu_Id id = mu_get_id(ctx, &value, sizeof(value));  // 用指针地址
mu_Id id = mu_get_id(ctx, label, strlen(label));   // 用字符串内容
```

**ID 栈机制**: `mu_begin_window_ex` 把窗口 ID 压入栈，内部控件的 ID 都是 `window_id + hash(data)` 的组合，防止不同窗口间 ID 冲突。

**为什么指针地址很重要**: microui 用传入数据的原始字节做 fnv-1a 哈希。对 `mu_Real *value`，ID 来自指针地址（不是值）。同一变量地址稳定 = ID 稳定跨帧。**局部变量地址在函数返回后无效**，下一帧同一地址可能是完全不同的控件 = 状态错乱。

## 3. 立即模式状态持久化

microui 不在内部保存控件值。状态通过以下机制跨帧持久化：

| 机制 | 作用 | 存储位置 |
|------|------|---------|
| **Focus/Hover** | 哪个控件被聚焦/悬停 | `ctx->focus`, `ctx->hover` |
| **Container pool** | 窗口位置、大小、滚动 | 48 个槽位，按 ID 匹配 |
| **TreeNode pool** | 树节点展开/折叠 | 按 ID 匹配 |
| **Widget 值指针** | slider/checkbox 的当前值 | 调用者内存（C 变量/JS 绑定） |
| **Frame counter** | LRU 淘汰依据 | `ctx->frame++` |

**关键**: `mu_end()` 中 `if (!ctx->updated_focus) ctx->focus = 0;` — 如果一帧没有控件声明 focus，focus 会被清除。

## 4. mu_slider_ex 的值指针问题

```c
int mu_slider_ex(mu_Context *ctx, mu_Real *value, mu_Real low, mu_Real high,
    mu_Real step, const char *fmt, int opt)
{
    mu_Real v = *value;                    // 读当前值
    mu_Id id = mu_get_id(ctx, &value, sizeof(value));  // ID = 指针地址
    ...
    *value = v = clamp(v, low, high);      // 写回调用者变量
}
```

- **ID 从 `&value`（指针地址）生成**，不是 `*value`（值）
- 控件状态跨帧追踪依赖稳定的指针地址
- **致命错误**: 传入局部变量地址 → 函数退出后地址失效 → 下一帧 microui 找不到对应控件 → 状态机紊乱

**正确做法**: 使用持久化的变量（全局、静态、或 JS 层绑定的值）：
```c
static mu_Real g_slider_val = 0.5f;
mu_slider_ex(&g_mu, &g_slider_val, flow, fhigh, 0.01f, "%g", MU_OPT_ALIGNCENTER);
```

## 5. 窗口 Body 计算

`mu_begin_window_ex` 中的 body 计算流程（line 1083-1159）：

1. `body = cnt->rect`（窗口完整矩形）
2. 减去标题栏（默认 24px）
3. 滚动条占位（scrollbar_size = 12px）
4. `expand_rect(body, -padding)` → 向内缩 5px（padding=5）

最终 layout body 尺寸 = 窗口尺寸 - 标题栏 - 滚动条 - padding * 2

## 6. mu_draw_control_text 对齐

```c
pos.y = rect.y + (rect.h - ctx->text_height(font)) / 2;  // 垂直居中（始终执行）
if (opt & MU_OPT_ALIGNCENTER) {
    pos.x = rect.x + (rect.w - tw) / 2;                  // 水平居中
} else if (opt & MU_OPT_ALIGNRIGHT) {
    pos.x = rect.x + rect.w - tw - padding;              // 右对齐
} else {
    pos.x = rect.x + padding;                            // 左对齐（默认）
}
```

**注意**: 垂直居中**不受 opt 影响**，始终应用。`MU_OPT_ALIGNCENTER` 只影响水平方向。

## 7. 默认样式值

| 字段 | 值 | 含义 |
|------|-----|------|
| `size.x` | 68 | 默认控件宽度 |
| `size.y` | 10 | 默认控件高度 |
| `padding` | 5 | 内边距 |
| `spacing` | 4 | 控件间距 |
| `title_height` | 24 | 标题栏高度 |
| `scrollbar_size` | 12 | 滚动条宽度 |
| `thumb_size` | 8 | 滑块拇指最小尺寸 |

## 8. 鼠标坐标期望

microui 用 `int` 存储所有坐标，**不做任何 DPI 缩放**。它期望接收与 widget rect 同一坐标系的鼠标输入。

- 如果渲染用逻辑像素 → 鼠标坐标应该是逻辑像素
- 如果渲染用物理像素 → 鼠标坐标应该是物理像素
- **关键**: 鼠标坐标和 microui rect 必须在同一坐标系中
