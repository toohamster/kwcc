# UI 设计与实现经验总结

> 基于计算器实例的全部修复经验，供开发新控件时参考

---

## 1. 布局系统（microui layout）

### `ui.layoutRow` 的参数规则

```js
// 满行（控件横跨整个窗口宽度）
ui.layoutRow(高度, -1);

// N 列均分（显式指定每列宽度）
ui.layoutRow(高度, 宽1, 宽2, 宽3, 宽4);
```

**关键**: `-1` 在 microui 中表示 "填充剩余空间"（`res.w += body.w - res.x + 1`）。每个 `mu_layout_row` 调用会通过 `memcpy` 完全覆盖旧的 widths 数组，不会残留上一行的列数状态。

### 布局行切换必须显式

当从 4 列按钮行切换到单列行时，**必须**显式调用 `ui.layoutRow(高度, -1)`，让 microui 重置 `items=1, widths[0]=-1`，否则会继续用上一行的 4 列布局。

```js
// 4 列按钮行
ui.layoutRow(30, 55, 55, 55, 55);
ui.button("7"); ui.button("8"); ui.button("9"); ui.button("+");

// 单列行：显式 -1 重置状态
ui.layoutRow(30, -1);
ui.button("=");
```

### `mu_layout_next` 的工作流程（line 578-624）

1. 检查 `next_type`：如果有 `mu_layout_set_next`，用其 rect 覆盖
2. 如果 `item_index == items`，触发换行（调用 `mu_layout_row` 重置到下一行）
3. 取 `position.x/y` 作为当前位置
4. 取 `widths[item_index]` 作为宽度（`items=0` 时用 `size.x=68` 默认宽度）
5. `position.x += res.w + spacing`（水平移动）
6. `next_row = max(next_row, res.y + res.h + spacing)`（追踪行底）
7. **加上 body 偏移**：`res.x += body.x`, `res.y += body.y`（窗口 padding 和滚动）

### `mu_layout_set_next(rect, relative)`

- `relative = 0` (ABSOLUTE): 跳过布局引擎，不推进位置、不加 body 偏移
- `relative = 1` (RELATIVE): 用自定义 rect 但仍参与布局流

---

## 2. 文字测量与对齐

### 必须用 `nvgTextBounds` 测量真实字体宽度

**错误做法**（导致所有居中计算错误）：
```c
static int mu_text_width(mu_Font font, const char *str, int len) {
    return len * 7;  // 完全不准确
}
```

**正确做法**：
```c
static int mu_text_width(mu_Font font, const char *str, int len) {
    if (!str || !vg) return len > 0 ? len * 7 : 0;
    if (len < 0) len = (int)strlen(str);
    float bounds[4];
    nvgFontFace(vg, "sans");
    nvgFontSize(vg, 14);
    nvgTextBounds(vg, 0, 0, str, str + len, bounds);
    return (int)(bounds[2] - bounds[0]);
}
```

**原因**: microui 的居中公式是 `pos.x = rect.x + (rect.w - tw) / 2`。`tw` 错了，居中就全错。按钮文字会偏左或偏右。

### 渲染器对齐设置

```c
// MU_COMMAND_TEXT 渲染
nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
nvgText(vg, c->pos.x, c->pos.y, c->str, NULL);
```

`NVG_ALIGN_LEFT | NVG_ALIGN_TOP` 确保 NanoVG 以左上角为锚点，与 microui 计算的位置一致。

### 显示屏右对齐（计算器类）

```c
int tx = r.x + r.w - tw - padding;  // 右对齐
int ty = r.y + (r.h - th) / 2;       // 垂直居中
mu_draw_text(&g_mu, NULL, text, -1, (mu_Vec2){tx, ty}, white);
```

---

## 3. 坐标系与 DPI

### 推荐的坐标系方案：不用 high_dpi

```c
// main.m - sokol_main()
sapp_desc sokol_main() {
    return (sapp_desc){
        .width  = 800,
        .height = 600,
        // 不设置 high_dpi，默认 false
    };
}

// frame()
nvgBeginFrame(vg, (float)w, (float)h, 1.0f);

// input()
bridge_input_mousemove((int)ev->mouse_x, (int)ev->mouse_y);
// 乘以 sapp_dpi_scale() 当 high_dpi=false 时 = 1.0，等于没变
```

当 `high_dpi=false` 时：
- `sapp_width()/height()` = 窗口逻辑像素
- `ev->mouse_x/y` = 同一坐标系
- `nvgBeginFrame` 的 DPR = 1.0
- **所有组件在同一坐标系中工作，不需要任何转换**

### 鼠标坐标缩放

```c
float scale = sapp_dpi_scale();
int mx = (int)(ev->mouse_x * scale);
int my = (int)(ev->mouse_y * scale);
```

当 `high_dpi=false` 时 `scale=1.0` 不变。如果以后改用 `high_dpi=true`，这段代码会自动正确工作。

---

## 4. 状态持久化（立即模式 IMGUI）

### Slider 值指针问题

**错误**（局部变量地址 = ID 不稳定）：
```c
mu_Real fval = 0.5;
mu_slider_ex(&g_mu, &fval, flow, fhigh, 0.01f, "%g", MU_OPT_ALIGNCENTER);
// fval 地址每帧不同 → microui 找不到控件状态
```

**正确**（持久化变量）：
```c
static mu_Real g_slider_val = 0.5f;
g_slider_val = (mu_Real)val;
mu_slider_ex(&g_mu, &g_slider_val, flow, fhigh, 0.01f, "%g", MU_OPT_ALIGNCENTER);
```

### JS 层状态持久化

```js
if (typeof _calcInit == "undefined") {
    var display = "0";
    var prevValue = 0;
    var operator = "";
    var newNumber = 1;
    var _calcInit = 1;  // 防止每帧重新初始化
}
```

### Widget ID 生成规则

microui 用 fnv-1a 哈希生成 ID：
- `mu_button`: ID = hash(label 字符串内容)
- `mu_slider`: ID = hash(&value 指针地址)
- `mu_begin_window`: ID = hash(title)

同一帧内相同的输入 = 相同 ID。跨帧相同的 ID = 控件被识别为同一个。

---

## 5. 图标渲染

### `MU_COMMAND_ICON` 的正确渲染

**错误**（画实心方块）：
```c
nvgRect(vg, c->rect.x, c->rect.y, c->rect.w, c->rect.h);
nvgFill(vg);
```

**正确**（画 X 形关闭图标）：
```c
mu_IconCommand *c = (mu_IconCommand *)cmd;
mu_Color col = c->color;
float s = 1.5f;  // 线宽
float m = c->rect.w * 0.25f;  // 边距
nvgBeginPath(vg);
nvgStrokeColor(vg, nvgRGBA(col.r, col.g, col.b, col.a));
nvgStrokeWidth(vg, s);
nvgMoveTo(vg, c->rect.x + m, c->rect.y + m);
nvgLineTo(vg, c->rect.x + c->rect.w - m, c->rect.y + c->rect.h - m);
nvgMoveTo(vg, c->rect.x + c->rect.w - m, c->rect.y + m);
nvgLineTo(vg, c->rect.x + m, c->rect.y + c->rect.h - m);
nvgStroke(vg);
```

### JS 层禁用不需要的图标

```js
// MU_OPT_NOCLOSE = (1 << 6) = 64
ui.beginWindow("Calculator", 200, 80, 280, 350, 64);
```

常用 flags：
- `MU_OPT_NOTITLE = 128` — 不显示标题栏
- `MU_OPT_NOCLOSE = 64` — 不显示关闭按钮
- `MU_OPT_NORESIZE = 16` — 不允许调整窗口大小
- `MU_OPT_NOFRAME = 8` — 不绘制窗口边框

---

## 6. Bridge API 开发模式

### ui.* 方法完整清单（kwcc.c js_ui_dispatch + methods_js）

**已实现的 C 层 handler**（`kwcc.c:250-479`）：

| JS 方法 | C handler 行号 | 功能 |
|---------|---------------|------|
| `ui.beginWindow(title, x, y, w, h, opt, topic)` | 250-293 | 开始窗口，第7参 topic 用于 X 关闭事件。带可见性挡板逻辑 |
| `ui.endWindow()` | 294-303 | 结束窗口，只对未拦截的窗口调 microui |
| `ui.sync(key, visible)` | 304-311 | 同步模块状态，设置当前模块 key 上下文 |
| `ui.beginPanel(name, opt)` | 312-319 | 开始面板 |
| `ui.endPanel()` | 320-323 | 结束面板 |
| `ui.button(text, topic)` | 324-336 | 按钮，topic 非空时点击 dispatch `action="click"` |
| `ui.label(text)` | 337-342 | 标签（左对齐） |
| `ui.slider(text, value, min, max, topic)` | 343-364 | 滑块，值变化时 dispatch `action="change"` |
| `ui.layoutRow(height, w1, w2, w3, w4)` | 365-376 | 布局行，最多4列 |
| `ui.setNext(x, y, w, h)` | 377-385 | 设置下一个控件绝对位置 |
| `ui.rect(x, y, w, h, r, g, b)` | 386-398 | 绘制矩形 |
| `ui.display(text)` | 399-416 | 计算器显示屏：深色背景 + 右对齐白色文本 |
| `ui.textCentered(text)` | 417-423 | 水平居中文字 |
| `ui.loadFont(name, path)` | 424-437 | 加载字体 |
| `ui.setFont(name)` | 438-449 | 设置当前字体 |
| `ui.loadFontDir(dir)` | 450-457 | 加载目录下所有字体 |
| `ui.svg(path_or_svg, x, y, w, h)` | 458-478 | SVG 渲染（inline + 文件路径，128 槽缓存） |

### 新增 UI 方法的标准流程

1. **C 层**（`kwcc.c`）：在 `js_ui_dispatch` 添加 `strcmp` 分支
2. **C 层**：调用对应的 microui/NanoVG 函数
3. **JS wrapper**（`kwcc_create_js` 中的 `methods_js`）：添加 JS 方法定义
4. **JS 业务层**（app 代码）：调用新方法

### 参数提取模板

```c
if (strcmp(method, "newMethod") == 0) {
    int x = 0;                    // 默认值
    const char *s = "";
    if (argc > 0) JS_ToInt32(ctx, &x, argv[0]);
    if (argc > 1) JS_ToInt32(ctx, &y, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &w, argv[2]);
    if (argc > 3) JS_ToInt32(ctx, &h, argv[3]);
    // 字符串参数需要 JS_ToCString + NULL 保护
    // ...
}
```

### 字符串参数保护

```c
JSCStringBuf buf;
const char *text = JS_ToCString(ctx, argv[0], &buf);
mu_function(ctx, text ? text : "");  // NULL → 空字符串
```

---

## 7. NanoVG 渲染技巧

### 字体使用

```c
// init() 时注册
nvgCreateFont(vg, "sans", "assets/Roboto-Regular.ttf");

// 渲染时使用
nvgFontFace(vg, "sans");
nvgFontSize(vg, 14);
```

### 矩形绘制

```c
nvgBeginPath(vg);
nvgRect(vg, x, y, w, h);
nvgFillColor(vg, nvgRGBA(r, g, b, a));  // alpha 0-255
nvgFill(vg);
```

### 描边线条

```c
nvgBeginPath(vg);
nvgStrokeColor(vg, nvgRGBA(r, g, b, a));
nvgStrokeWidth(vg, width);
nvgMoveTo(vg, x1, y1);
nvgLineTo(vg, x2, y2);
nvgStroke(vg);
```

### 裁剪区域

```c
// microui 的 MU_COMMAND_CLIP 处理
if (c->rect.w == 0 && c->rect.h == 0) {
    nvgResetScissor(vg);
} else {
    nvgScissor(vg, c->rect.x, c->rect.y, c->rect.w, c->rect.h);
}
```

---

## 8. 窗口 Body 计算

`mu_begin_window_ex` 中 body 的计算顺序：

1. `body = cnt->rect`（窗口完整矩形）
2. 减去标题栏（`title_height = 24`）
3. 滚动条占位（`scrollbar_size = 12`）
4. `expand_rect(body, -padding)` → 向内缩 5px

所以 layout body 内所有控件都会自动偏移 padding=5px。

---

## 10. JS 文件模块化

### load() 函数

mquickjs 提供全局 `load(filename)` 函数，读取并 `JS_Eval` 一个 JS 文件：

```js
// main.js
load("app/calc_logic.js");    // 加载外部文件到当前上下文
```

**实现位置**: `src/mquickjs_stubs.c` 的 `js_load` 函数。

### 模块状态持久化模式

每帧都会执行 `main.js`（调用 `load()`），所以被加载的文件中的状态需要用 `typeof` 检查防止每帧重置：

```js
// calc_logic.js
if (typeof _calcInit == "undefined") {
    var display = "0";
    var prevValue = 0;
    var _calcInit = 1;  // 标记已初始化
}
```

### 推荐的目录结构

```
app/
├── main.js          # UI 入口文件（ui.beginWindow、布局、按钮事件）
├── calc_logic.js    # 业务逻辑（状态、计算函数）
├── styles.js        # 可复用的样式常量（颜色、尺寸）
└── widgets.js       # 可复用的自定义组件
```

### 注意事项

- `load()` 是同步 eval，文件内容在当前作用域中执行
- 没有模块缓存机制，每次调用都会重新 eval（但 `typeof` 检查可防止重复初始化）
- 变量作用域是全局的，`load` 的文件中声明的 `var` 可在 `main.js` 中访问
- 不支持 CommonJS/ESM 语法（`require`、`import/export`），mquickjs 不解析这些语法

当界面出现错位时，按以下顺序检查：

1. **layoutRow 参数是否正确**：满行用 `-1`，多列用具体宽度
2. **text_width 是否准确**：用 `nvgTextBounds` 而非硬编码
3. **坐标系是否统一**：不要混用物理和逻辑像素
4. **Widget 地址是否持久**：slider 值用静态变量
5. **所有命令是否在布局流中**：ABSOLUTE setNext 不加 body 偏移
6. **渲染器是否正确处理所有命令类型**：特别是 ICON 和 TEXT
