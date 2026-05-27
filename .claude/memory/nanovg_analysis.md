# NanoVG 坐标空间与 DPR 模型分析

> 基于 deps/nanovg/nanovg.h + nanovg_gl.h 源码分析

## nvgBeginFrame 参数

```c
void nvgBeginFrame(NVGcontext* ctx, float windowWidth, float windowHeight, float devicePixelRatio)
```

### windowWidth / windowHeight

定义**逻辑坐标空间**的大小。所有绘图命令（`nvgRect`, `nvgText`, `nvgScissor`）使用这个坐标系。

GL 顶点着色器归一化到裁剪空间：
```glsl
gl_Position = vec4(2.0*vertex.x/viewSize.x - 1.0, 1.0 - 2.0*vertex.y/viewSize.y, 0, 1);
```

### devicePixelRatio

**不改变坐标空间**。只控制渲染质量：

| 参数 | 公式 | 效果 |
|------|------|------|
| `tessTol` | `0.25 / ratio` | 曲线细分容差，DPR 越高越精细 |
| `distTol` | `0.01 / ratio` | 距离容差，更紧密的曲线近似 |
| `fringeWidth` | `1.0 / ratio` | 抗锯齿边缘宽度（物理像素） |

GL 后端的 `glnvg__renderViewport` 完全忽略 DPR，只存 width/height。

**结论**：NanoVG 所有坐标都在一个统一的逻辑空间中。DPR 不影响坐标转换，只影响渲染精细度。

## 正确的 HiDPI 渲染模式

要得到 Retina 清晰渲染：

```c
float dpr = sapp_dpi_scale();
// 方式1：逻辑坐标 + DPR
nvgBeginFrame(vg, logical_w, logical_h, dpr);
// 渲染命令用逻辑坐标，自动精细渲染

// 方式2：物理坐标 + DPR=1.0
nvgBeginFrame(vg, fb_w, fb_h, 1.0f);
// 渲染命令用物理坐标，DPR 不参与
```

**关键**：两种方式是等价的。方式1 更优雅（DPR 自动处理质量），方式2 更直接（手动处理缩放）。

## nvgTextBounds 返回值

```c
float nvgTextBounds(NVGcontext* ctx, float x, float y, const char* s, const char* end, float* bounds);
```

**返回 `[xmin, ymin, xmax, ymax]`，全部是逻辑像素坐标。**

内部计算过程：
1. `scale = transformScale * devicePixelRatio`
2. 用 `fontSize * scale`（物理像素）光栅化字体
3. 结果除以 `scale` 转回逻辑坐标

所以可以直接用返回值与 `nvgRect` 等逻辑坐标函数配合。

## nvgFontSize + nvgFontFace

```c
nvgFontFace(ctx, "sans");   // 选择字体
nvgFontSize(ctx, 14);       // 设置字号（逻辑像素）
```

渲染时实际物理字号 = `fontSize * transformScale * devicePixelRatio`

`fontSize` 是逻辑像素单位，NanoVG 自动缩放用于光栅化。

## nvgScissor

与所有绘图命令使用同一逻辑坐标空间。会被当前变换矩阵预乘。

## 在 kwcc 中的应用

### 当前代码（high_dpi=false 时）

```c
int w = sapp_width();     // = 800 (逻辑像素)
int h = sapp_height();    // = 600 (逻辑像素)
nvgBeginFrame(vg, w, h, 1.0f);  // DPR=1.0，坐标一致
```

这是正确的：窗口 800x600，NanoVG 画布 800x600，DPR=1.0。

### 如果改用 high_dpi=true

```c
int fb_w = sapp_width();       // = 1600 (物理像素)
int fb_h = sapp_height();      // = 1200 (物理像素)
float dpr = sapp_dpi_scale();  // = 2.0

// 方式 A：物理坐标
nvgBeginFrame(vg, fb_w, fb_h, 1.0f);

// 方式 B：逻辑坐标 + DPR
float log_w = fb_w / dpr;      // = 800
float log_h = fb_h / dpr;      // = 600
nvgBeginFrame(vg, log_w, log_h, dpr);
```

两种方式效果相同，但方式 B 下 JS 代码可以继续用逻辑像素（更直观）。

## 常见错误

1. **`nvgBeginFrame(vg, fb_w, fb_h, dpr)`** → 错误！width/height 用了物理像素但 DPR 又乘一次 → 坐标空间放大 DPR^2 倍
2. **用 `len * 7` 做文字宽度** → 不同字体/字号/DPR 下完全不准确 → 应用 `nvgTextBounds`
3. **NanoVG 坐标和 microui rect 不在同一空间** → 严重错位
