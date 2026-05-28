# nanosvg 源码分析

> nanosvg: 单头 SVG 解析库，将 SVG 转换为 cubic bezier 路径列表。
> 仓库: https://github.com/memononen/nanosvg

## 核心数据结构

```c
// 解析后的 SVG 图像
typedef struct NSVGimage {
    float width;           // 图像宽度
    float height;          // 图像高度
    NSVGshape* shapes;     // 形状链表
} NSVGimage;

// 单个形状（对应 SVG 中的一个元素）
typedef struct NSVGshape {
    char id[64];           // 可选的 id 属性
    NSVGpaint fill;        // 填充颜色
    NSVGpaint stroke;      // 描边颜色
    float opacity;         // 透明度
    float strokeWidth;     // 描边宽度
    char strokeLineJoin;   // 描边连接: NSVG_JOIN_MITER/ROUND/BEVEL
    char strokeLineCap;    // 描边端点: NSVG_CAP_BUTT/ROUND/SQUARE
    char fillRule;         // 填充规则: NSVG_FILLRULE_NONZERO/EVENODD
    unsigned char flags;   // 标志位: NSVG_FLAGS_VISIBLE = 0x01
    float bounds[4];       // 紧贴边界框 [minx,miny,maxx,maxy]
    NSVGpath* paths;       // 路径链表
    NSVGshape* next;       // 下一个形状
} NSVGshape;

// 路径（一组 bezier 曲线）
typedef struct NSVGpath {
    float* pts;            // 点数组: x0,y0, [cp1x,cp1y,cp2x,cp2y,x1,y1], ...
    int npts;              // 总点数（每个 bezier 段 3 个点 = 6 个 float）
    char closed;           // 是否闭合路径
    float bounds[4];       // 紧贴边界框
    NSVGpath* next;        // 下一个路径
} NSVGpath;

// 颜色/渐变
typedef struct NSVGpaint {
    signed char type;      // NSVG_PAINT_NONE=0, COLOR=1, LINEAR_GRADIENT=2, RADIAL_GRADIENT=3
    union {
        unsigned int color; // RGB 格式: R | (G<<8) | (B<<16), 无 alpha
        NSVGgradient* gradient;
    };
} NSVGpaint;
```

## 解析 API

```c
// 从文件解析
NSVGimage* nsvgParseFromFile(const char* filename, const char* units, float dpi);
// 从字符串解析（会修改输入字符串）
NSVGimage* nsvgParse(char* input, const char* units, float dpi);
// 释放
void nsvgDelete(NSVGimage* image);
```

- `units`: 单位字符串，常用 `"px"`
- `dpi`: DPI，常用 `96.0f`
- 解析失败返回 `NULL`（文件不存在、XML 格式错误等）

## 颜色提取

`shape->fill.color` / `shape->stroke.color` 是 `unsigned int`，格式为 `NSVG_RGB(r,g,b)`：
```c
unsigned int c = shape->fill.color;
unsigned char r = (c >> 16) & 0xFF;  // 注意: RGB 顺序
unsigned char g = (c >>  8) & 0xFF;
unsigned char b = (c >>  0) & 0xFF;
unsigned char a = 255;  // 无 alpha，默认不透明
```

## 路径点格式

`pts` 数组格式：
```
[x0, y0,  cp1x, cp1y, cp2x, cp2y, x1, y1,  cp3x, cp3y, ...]
 ↑起点     ↑第一个 bezier 段 (2 控制点 + 终点)   ↑第二段...
```

每个 cubic bezier 段需要 6 个 float（3 个点）。
`npts` 是总点数，包括起点。一个有 N 个 bezier 段的路径有 `npts = 1 + N * 3`。

遍历方式：
```c
nvgMoveTo(vg, pts[0], pts[1]);
for (int i = 1; i + 2 < npts; i += 3) {
    nvgBezierTo(vg, pts[i*2], pts[i*2+1],
                     pts[(i+1)*2], pts[(i+1)*2+1],
                     pts[(i+2)*2], pts[(i+2)*2+1]);
}
if (path->closed) nvgClosePath(vg);
```

## 渲染流程

```
1. nsvgParseFromFile() → NSVGimage
2. 遍历 shapes 链表
3. 对每个 shape: 检查 flags & NSVG_FLAGS_VISIBLE
4. 遍历 shape->paths 链表
5. 对每个 path: 用 NanoVG 绘制 bezier 曲线
   - nvgBeginPath → nvgMoveTo → nvgBezierTo
   - Fill (如果 fill.type == NSVG_PAINT_COLOR)
   - Stroke (如果 stroke.type == NSVG_PAINT_COLOR && strokeWidth > 0)
6. nsvgDelete() 释放
```

## 与 NanoVG 的对应关系

| nanosvg | NanoVG | 说明 |
|---------|--------|------|
| NSVG_CAP_BUTT | NVG_BUTT | 线段端点 |
| NSVG_CAP_ROUND | NVG_ROUND | 线段端点 |
| NSVG_CAP_SQUARE | NVG_SQUARE | 线段端点 |
| NSVG_JOIN_MITER | NVG_MITER | 线段连接 |
| NSVG_JOIN_ROUND | NVG_ROUND | 线段连接 |
| NSVG_JOIN_BEVEL | NVG_BEVEL | 线段连接 |
| NSVG_FILLRULE_NONZERO | nvgPathWinding(NVG_CW) | 填充规则 |
| NSVG_FILLRULE_EVENODD | nvgPathWinding(NVG_CCW) | 填充规则 |

## 注意事项

1. **不支持文本/滤镜/动画**: nanosvg 只解析基本形状（circle, rect, polygon, path, line, ellipse）
2. **不支持 CSS 样式**: 只支持内联 `fill="#hex"` 属性
3. **circle/rect/polygon 会被转换为 bezier 路径**: 解析器自动将 SVG 元素转为 cubic bezier
4. **SVG 命名空间不影响解析**: `xmlns` 属性被忽略
5. **文件路径**: `nsvgParseFromFile` 使用 `fopen("rb")`，需要正确的相对或绝对路径
6. **闭合路径**: `path->closed` 为 1 时需要在绘制后调用 `nvgClosePath`

## kwcc 集成架构

### SVG 作为 microui 命令（MU_COMMAND_SVG = 32）

SVG 渲染不使用独立队列，而是通过 `mu_push_command` 插入 microui 命令流：

```c
// microui.h — kwcc 扩展命令类型（type=32，给官方留 31 个空间）
enum {
  MU_COMMAND_MAX,
  MU_COMMAND_SVG = 32,
};

typedef struct { mu_BaseCommand base; mu_Rect rect; char path[1]; } mu_SvgCommand;
```

**入队（kwcc.c）**：
```c
static void kwcc_queue_svg(const char *path, float x, float y, float w, float h) {
    mu_Rect clip = mu_get_clip_rect(&g_mu);
    float sx = clip.x + x;  // 布局相对坐标 → 屏幕坐标
    float sy = clip.y + y;
    mu_SvgCommand *cmd = (mu_SvgCommand *)mu_push_command(&g_mu, MU_COMMAND_SVG, sizeof(mu_SvgCommand) + strlen(path));
    cmd->rect.x = (int)sx; cmd->rect.y = (int)sy;
    cmd->rect.w = (int)w;  cmd->rect.h = (int)h;
    memcpy(cmd->path, path, strlen(path) + 1);
}
```

**渲染（main.m render_mu_commands）**：
- SVG 命令在 `mu_next_command` 遍历时自动跟随所属窗口的 zindex
- 每个 SVG 命令紧跟在对应 microui 命令之后渲染
- 点击其他窗口时，SVG 不会浮动在最上层

### 已解决的问题

1. **C ABI float 参数提升**：`kwcc.c` 没有 include `kwcc.h`，导致 `kwcc_queue_svg` 的 float 参数在 x86_64 下被提升为 double，ABI 不匹配导致参数全为 0。**修复**：添加 `#include "kwcc.h"`。

2. **NANOSVG_IMPLEMENTATION 重复定义**：在 `main.m` 和 `kwcc.c` 中都定义了 `NANOSVG_IMPLEMENTATION`，链接时 6 个重复符号。**修复**：只在 `main.m` 中定义。

3. **JS_ToNumber vs JS_ToInt32**：mquickjs 中 `JS_ToNumber` 对 JS 整数参数可能返回 0，改用 `JS_ToInt32`。
