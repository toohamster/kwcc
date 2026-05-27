# 完整计算器方案（v3）

## 问题根因

1. `mu_label` 内部调用 `mu_layout_next()` 获取位置，文字直接画在 rect.y，**不垂直居中**
2. 窗口内容区高度 296px，内容只有 182px，上方有大量空白
3. display 区域没有背景色，看起来不像计算器显示屏

## UI 草图与精确计算

### 窗口尺寸：280 x 350

```
窗口：280 x 350
┌─────────────────────────────┐ y=0
│ [标题栏: Calculator    x]   │ 24px (固定)
├─────────────────────────────┤ y=24
│  内容区: 280 x 326           │
│  布局区域 (padding=5):       │
│  x: 5..275 (宽270)          │
│  y: 29..345 (高316)         │
│                             │
│  ┌───────────────────────┐  │ y=29+16=45
│  │     "123" (居中)      │  │ h=40 display
│  └───────────────────────┘  │
│                             │ y=85+6=91
│  ┌──┐┌──┐┌──┐┌──┐          │ y=91
│  │7 ││8 ││9 ││+ │          │ h=30
│  └──┘└──┘└──┘└──┘          │
│  ┌──┐┌──┐┌──┐┌──┐          │ y=125
│  │4 ││5 ││6 ││- │          │ h=30
│  └──┘└──┘└──┘└──┘          │
│  ┌──┐┌──┐┌──┐┌──┐          │ y=159
│  │1 ││2 ││3 ││x │          │ h=30
│  └──┘└──┘└──┘└──┘          │
│  ┌──┐┌──┐┌──┐┌──┐          │ y=193
│  │0 ││. ││C ││/ │          │ h=30
│  └──┘└──┘└──┘└──┘          │
│  ┌──────────────────────┐  │ y=227
│  │         =            │  │ h=30
│  └──────────────────────┘  │
│                             │ y=257..345 (剩余88px)
└─────────────────────────────┘
```

### microui 布局机制

`mu_layout_next()` 第 601 行：
```c
res.w = layout->items > 0 ? layout->widths[layout->item_index] : layout->size.x;
```

- **`items == 0`**：每个控件占**整行宽度**，垂直堆叠
- **`items > 0`**：需要传 `widths` 数组指定每列宽度

### `mu_layout_set_next` 机制

```c
void mu_layout_set_next(mu_Context *ctx, mu_Rect r, int relative);
```

- 调用后，下一个 `mu_layout_next()` 返回 r，跳过正常流式计算
- relative=0：绝对坐标；relative=1：相对当前容器
- **关键**：设置后，流式布局继续从该位置往下

## 方案：混合布局

display 区域用 `mu_layout_set_next` 绝对定位（microui 原生支持），然后流式布局按钮。

```js
// 1. 先设置 display 区域的绝对位置（在布局区域内）
ui.setNext(5, 0, 260, 40);  // x=5(相对布局), y=0, w=260, h=40

// 2. 绘制 display 背景（整行矩形）
ui.rect(5, 0, 260, 40, 40, 40, 40);  // x, y, w, h, r, g, b

// 3. 绘制 display 文字（水平+垂直居中）
ui.textCentered("123");

// 4. 设置 display 后跳到下一行
ui.layoutRow(4);   // 空白分隔

// 5. 按钮流式布局
ui.layoutRow(30, 55, 55, 55, 55);
ui.button("7"); ui.button("8"); ...
```

## 修改清单

### 文件 1：`src/bridge.c`

#### 新增 method: `setNext`
```c
if (strcmp(method, "setNext") == 0) {
    int x = 0, y = 0, w = 0, h = 0;
    if (argc > 0) JS_ToInt32(ctx, &x, argv[0]);
    if (argc > 1) JS_ToInt32(ctx, &y, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &w, argv[2]);
    if (argc > 3) JS_ToInt32(ctx, &h, argv[3]);
    mu_layout_set_next(&g_mu, (mu_Rect){x, y, w, h}, 0);
    return JS_UNDEFINED;
}
```

#### 新增 method: `rect`
```c
if (strcmp(method, "rect") == 0) {
    int x = 0, y = 0, w = 0, h = 0;
    int r = 50, g = 50, b = 50;
    if (argc > 0) JS_ToInt32(ctx, &x, argv[0]);
    if (argc > 1) JS_ToInt32(ctx, &y, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &w, argv[2]);
    if (argc > 3) JS_ToInt32(ctx, &h, argv[3]);
    if (argc > 4) JS_ToInt32(ctx, &r, argv[4]);
    if (argc > 5) JS_ToInt32(ctx, &g, argv[5]);
    if (argc > 6) JS_ToInt32(ctx, &b, argv[6]);
    mu_draw_rect(&g_mu, (mu_Rect){x, y, w, h}, (mu_Color){r, g, b, 255});
    return JS_UNDEFINED;
}
```

#### 新增 method: `textCentered`
```c
if (strcmp(method, "textCentered") == 0) {
    JSCStringBuf buf;
    const char *text = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
    mu_Rect r = mu_layout_next(&g_mu);
    mu_draw_control_text(&g_mu, text ? text : "", r, MU_COLOR_TEXT, MU_OPT_ALIGNCENTER);
    return JS_UNDEFINED;
}
```

#### 修改 JS wrapper
```js
"ui.setNext = function(x,y,w,h) { kwcc_ui('setNext',x,y,w,h); };\n"
"ui.rect = function(x,y,w,h,r,g,b) { kwcc_ui('rect',x,y,w,h,r,g,b); };\n"
"ui.textCentered = function(t) { kwcc_ui('textCentered',t); };\n"
```

### 文件 2：`app/main.js`

```js
// display 区域：绝对定位在布局区域顶部
ui.setNext(5, 0, 260, 40);
ui.rect(5, 0, 260, 40, 40, 40, 40);
ui.textCentered(display);

// 按钮用 layoutRow 流式布局
ui.layoutRow(4);  // 空白分隔

ui.layoutRow(30, 55, 55, 55, 55);
ui.button("7"); ui.button("8"); ui.button("9"); ui.button("+");
// ... 等
```

## 验证

1. `make clean && make` 编译通过
2. `./kwcc` 运行无崩溃
3. display 区域：深色背景 + 文字垂直+水平居中
4. 按钮网格：4 列对齐，文字居中
5. 点击按钮功能正常
6. C 清零，小数点正常
