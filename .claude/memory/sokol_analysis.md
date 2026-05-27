# Sokol 高分屏 DPI 行为分析

> 基于 deps/sokol/sokol_app.h 源码分析（macOS/Cocoa 后端）

## high_dpi 参数的核心作用

`sapp_desc.high_dpi` 控制 `wantsBestResolutionOpenGLSurface` 的开关（macOS 线 5344-5347）：

```objc
if (_sapp.desc.high_dpi) {
    [_sapp.macos.view setWantsBestResolutionOpenGLSurface:YES];
} else {
    [_sapp.macos.view setWantsBestResolutionOpenGLSurface:NO];
}
```

## high_dpi=false vs true 对比

假设请求 `width=640, height=480`，Retina 屏 `backingScaleFactor=2.0`：

| 值 | high_dpi=false（默认） | high_dpi=true |
|---|---|---|
| 窗口外观 | 640x480 points | 640x480 points |
| Framebuffer | 640x480 像素 | 1280x960 像素（2x Retina） |
| `sapp_width()` | 640 | 1280 |
| `sapp_height()` | 480 | 960 |
| `sapp_dpi_scale()` | **1.0**（硬编码） | **2.0**（backingScaleFactor） |
| `ev->mouse_x/y` | 在 points 范围 (0-640) | 在像素范围 (0-1280) |
| 渲染清晰度 | OS 拉伸（模糊） | 原生像素（清晰） |

## 关键源码依据

### sapp_dpi_scale() 设置（线 5646-5651）

```c
_SOKOL_PRIVATE void _sapp_macos_update_dimensions(void) {
    if (_sapp.desc.high_dpi) {
        _sapp.dpi_scale = [_sapp.macos.window screen].backingScaleFactor;
    } else {
        _sapp.dpi_scale = 1.0f;  // 永远 1.0
    }
```

### Framebuffer 尺寸计算（线 5363-5368）

```c
_sapp.framebuffer_width  = view_bounds.size.width  * _sapp.dpi_scale;
_sapp.framebuffer_height = view_bounds.size.height * _sapp.dpi_scale;
```

### 鼠标坐标转换（线 5707-5712）

```c
float new_x = mouse_pos.x * _sapp.dpi_scale;
float new_y = _sapp.framebuffer_height - (mouse_pos.y * _sapp.dpi_scale) - 1;
```

## 坐标系总结

```
high_dpi=false（默认）:
  window_size = 640x480  points
  framebuffer = 640x480  pixels
  mouse coords = 0..640  points
  sapp_dpi_scale = 1.0
  → 一切都在同一坐标系中（简单，但不清晰）

high_dpi=true:
  window_size = 640x480  points
  framebuffer = 1280x960 pixels
  mouse coords = 0..1280 pixels
  sapp_dpi_scale = 2.0
  → 两套坐标系需要转换
```

## 在 kwcc 中的正确用法

### 推荐方案：high_dpi=false（默认）

简单起见，不用 `high_dpi`，所有东西在同一坐标系：

```c
sapp_desc sokol_main() {
    return (sapp_desc){
        .width  = 800,
        .height = 600,
        // 不设置 high_dpi，默认 false
    };
}
```

- `sapp_width()/height()` = 窗口逻辑像素 = NanoVG 画布尺寸
- `ev->mouse_x/y` = 同一坐标系
- `nvgBeginFrame(vg, w, h, 1.0f)` — DPR=1.0

### 如果要用 high_dpi=true

需要统一坐标系，三选一：

**方案 A：全部物理像素**
- JS 坐标 × dpi_scale → microui 用物理像素
- NanoVG: `nvgBeginFrame(vg, fb_w, fb_h, 1.0f)` — DPR=1.0
- 鼠标: 直接使用（已是物理像素）

**方案 B：全部逻辑像素**
- NanoVG: `nvgBeginFrame(vg, w/dpi_scale, h/dpi_scale, dpi_scale)` — 用逻辑坐标
- 鼠标: `/ dpi_scale` → 转为逻辑像素
- JS 坐标 = 逻辑像素（自然写法）

**方案 B 更好**，因为 JS 代码用逻辑像素更直观。

## 常见错误

1. **鼠标不乘/不除 dpi_scale** → 点击位置偏移
2. **JS 坐标不缩放** → 窗口/按钮位置错位
3. **NanoVG 用物理像素但 DPR=1.0** → 渲染模糊
4. **mix 物理和逻辑像素** → 严重错位
