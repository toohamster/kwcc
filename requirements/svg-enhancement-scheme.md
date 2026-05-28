# SVG 网桥增强方案（v2 — 安全重构版）

> 目标：让 `ui.svg()` 支持字符串渲染 + 缓存优化，提升实用价值。

## 需求

1. **内存字符串渲染**：`ui.svg("<svg>...</svg>", x, y, w, h)` 直接接收 SVG 源码字符串，JS 层可动态拼接生成折线图、仪表盘等
2. **解析缓存**：每帧 60 次重绘不应重复读取/解析 .svg 文件，首次加载后缓存 `NSVGimage*`

---

## 推荐方案：fnv1a 哈希 + 帧安全二级缓存

### 核心思路

- 通过检测字符串首字符是否为 `<` 来区分文件路径和 SVG 源码
- **网桥层（kwcc.c）完成所有工作**：计算 fnv1a 哈希 → 查缓存 → 解析（未命中时）→ 得到 `cache_idx`
- **命令流只携带 `cache_idx`（int）**，不携带任何 SVG 字符串/路径，彻底避免命令缓冲区膨胀
- 渲染层（main.m）直接通过 `cache_idx` 从缓存表取 `NSVGimage*`，零查找开销

### 缓存表结构

```c
#define SVG_CACHE_SIZE 128  /* 128 槽位 ≈ 4 KB 内存，覆盖 ~128 个不同 SVG */

typedef struct {
    uint32_t     hash;           /* fnv1a 32-bit 哈希 */
    size_t       content_len;    /* 原始字符串长度，用于防碰撞校验 */
    NSVGimage   *image;          /* 解析后的 SVG 数据 */
    int          frame_id;       /* 最后一次命中的帧号 */
    int          in_use;         /* 是否已占用 */
} svg_cache_t;

static svg_cache_t g_svg_cache[SVG_CACHE_SIZE];
static int         g_svg_cache_next = 0;  /* LRU 淘汰指针 */
```

**新增字段说明**：
- `content_len`：哈希命中时多校验一步长度比较，零成本防御 fnv1a 碰撞
- `frame_id`：记录最后一次被命中的帧号，淘汰时用于保护当前帧正在使用的槽位

### fnv1a 哈希（3 行）

```c
static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261;
    while (*s) { h ^= *s++; h *= 16777619; }
    return h;
}
```

### SVG 缓存查找/填充（kwcc.c 核心逻辑）

```c
/* 外部传入当前帧号（由 main.m 在 frame() 中设置） */
static int g_current_frame = 0;
void kwcc_set_frame(int frame) { g_current_frame = frame; }

/*
 * svg_resolve: 查找或解析 SVG，返回 cache_idx
 *
 * data:    文件路径 或 inline SVG 字符串（以 '<' 开头）
 * is_inline: 是否为 inline SVG（data[0] == '<'）
 *
 * 返回: cache_idx (0..SVG_CACHE_SIZE-1)，-1 表示解析失败
 */
static int svg_resolve(const char *data, int is_inline) {
    uint32_t hash = fnv1a(data);
    size_t   len  = strlen(data);

    /* 1. 线性扫描缓存，匹配 hash + content_len */
    for (int i = 0; i < SVG_CACHE_SIZE; i++) {
        if (g_svg_cache[i].in_use &&
            g_svg_cache[i].hash == hash &&
            g_svg_cache[i].content_len == len) {
            g_svg_cache[i].frame_id = g_current_frame;  /* 更新活跃帧 */
            return i;  /* 命中 */
        }
    }

    /* 2. 未命中 → 解析 */
    NSVGimage *img = NULL;
    if (is_inline) {
        /* inline SVG 需要可写副本（nsvgParse 会修改输入字符串） */
        char *buf = strdup(data);
        img = nsvgParse(buf, "px", 96.0f);
        free(buf);
    } else {
        img = nsvgParseFromFile(data, "px", 96.0f);
    }
    if (!img) return -1;  /* 解析失败 */

    /* 3. 淘汰 + 存入 */
    int slot = g_svg_cache_next;

    /* 安全检查：禁止淘汰当前帧正在使用的槽位 */
    if (g_svg_cache[slot].in_use &&
        g_svg_cache[slot].frame_id >= g_current_frame) {
        /* 顺延找下一个非当前帧槽位 */
        int found = -1;
        for (int i = 0; i < SVG_CACHE_SIZE; i++) {
            if (g_svg_cache[i].in_use && g_svg_cache[i].frame_id < g_current_frame) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            slot = found;
        } else {
            /* 所有槽位都在当前帧使用过 → 不淘汰，直接返回 -1 兜底 */
            /* 渲染层会尝试直接解析（不写缓存） */
            nsvgDelete(img);
            return -1;
        }
    }

    /* 释放旧数据 */
    if (g_svg_cache[slot].in_use && g_svg_cache[slot].image) {
        nsvgDelete(g_svg_cache[slot].image);
    }

    /* 写入新数据 */
    g_svg_cache[slot].hash = hash;
    g_svg_cache[slot].content_len = len;
    g_svg_cache[slot].image = img;
    g_svg_cache[slot].frame_id = g_current_frame;
    g_svg_cache[slot].in_use = 1;

    /* 更新 LRU 指针 */
    g_svg_cache_next = (slot + 1) % SVG_CACHE_SIZE;

    return slot;
}
```

### mu_SvgCommand 结构（microui.h）— 精简版

```c
typedef struct { mu_BaseCommand base; mu_Rect rect; int cache_idx; } mu_SvgCommand;
```

**v2 变化**：
- 移除 `char path[1]` 变长字段 → 命令大小固定为 `sizeof(mu_BaseCommand) + sizeof(mu_Rect) + sizeof(int)` ≈ 20 字节
- `cache_idx`：缓存表索引（>=0 命中，-1 未命中兜底）
- **不再拷贝 SVG 字符串到命令缓冲区**，彻底防止挤爆 microui 256KB 命令栈

### 入队流程（kwcc.c）

```
1. 检测 data[0] == '<' → inline SVG；否则是文件路径
2. 调用 svg_resolve(data, is_inline) → 得到 cache_idx
3. mu_SvgCommand 只存 cache_idx + rect（屏幕坐标）
4. 无需拷贝任何字符串到命令缓冲区
```

```c
static void kwcc_queue_svg(const char *data, int is_inline, float x, float y, float w, float h) {
    int cache_idx = svg_resolve(data, is_inline);

    mu_Rect clip = mu_get_clip_rect(&g_mu);
    mu_SvgCommand *cmd = (mu_SvgCommand *)mu_push_command(&g_mu, MU_COMMAND_SVG, sizeof(mu_SvgCommand));
    cmd->rect.x = (int)(clip.x + x);
    cmd->rect.y = (int)(clip.y + y);
    cmd->rect.w = (int)w;
    cmd->rect.h = (int)h;
    cmd->cache_idx = cache_idx;
}
```

### JS 层 `svg` handler 改动（kwcc.c）

```c
if (strcmp(method, "svg") == 0) {
    JSCStringBuf buf;
    JSValue arg = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (JS_IsUndefined(arg)) return JS_UNDEFINED;
    const char *js_data = JS_ToCString(ctx, arg, &buf);
    if (!js_data) return JS_UNDEFINED;

    /* 复制到本地缓冲（最大支持 4KB inline SVG） */
    char data[4096];
    strncpy(data, js_data, sizeof(data) - 1);
    data[sizeof(data) - 1] = '\0';

    int ix = 0, iy = 0, iw = 100, ih = 100;
    if (argc > 1) JS_ToInt32(ctx, &ix, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &iy, argv[2]);
    if (argc > 3) JS_ToInt32(ctx, &iw, argv[3]);
    if (argc > 4) JS_ToInt32(ctx, &ih, argv[4]);

    int is_inline = (data[0] == '<');
    kwcc_queue_svg(data, is_inline, (float)ix, (float)iy, (float)iw, (float)ih);
    return JS_UNDEFINED;
}
```

### 渲染流程（main.m）

```
1. 读取 cmd->cache_idx
2. cache_idx >= 0 → g_svg_cache[cache_idx].image 直接取数据画 NanoVG
3. cache_idx < 0 → 兜底路径：
   a. 理论上不应发生（网桥层已解析）
   b. 防御性处理：跳过渲染或记录日志
4. 更新 g_svg_cache[cache_idx].frame_id = g_current_frame
```

```c
case MU_COMMAND_SVG:
    {
        mu_SvgCommand *c = (mu_SvgCommand *)cmd;
        if (c->cache_idx < 0 || c->cache_idx >= SVG_CACHE_SIZE) {
            log_warn("svg: invalid cache_idx=%d, skipping", c->cache_idx);
            break;
        }

        NSVGimage *image = g_svg_cache[c->cache_idx].image;
        if (!image) { log_warn("svg: cache[%d] has NULL image", c->cache_idx); break; }

        /* 更新活跃帧（防止被淘汰） */
        g_svg_cache[c->cache_idx].frame_id = g_current_frame;

        /* ... 原有 NanoVG 渲染逻辑不变 ... */
    }
    break;
```

### 帧号同步（main.m → kwcc.c）

main.m 的 `frame()` 回调中，在调用 `kwcc_process_js()` 之前：

```c
/* 需要在 kwcc.h 中暴露此接口 */
kwcc_set_frame(sapp_frame_count());  /* 或用自增计数器 */
kwcc_process_js(js_ctx, js_text);
```

或者更简单，在 `kwcc_process_js()` 内部自增帧号：

```c
static int g_frame_counter = 0;
void kwcc_process_js(JSContext *ctx, const char *js_text) {
    g_frame_counter++;
    /* ... existing code ... */
}
```

### 缓存容量分析

#### 128 槽位能支持多少 SVG？

| 场景 | 不同 SVG 数 | 缓存是否够用 |
|------|------------|-------------|
| 纯图标应用 | 10-20 | ✅ 充裕到浪费 |
| 仪表盘 + 图表 | 5-15 固定 + 1-3 动态 | ✅ 充裕 |
| 图片浏览器（大量不同 SVG） | 50+ | ✅ 基本不淘汰 |
| 实时数据可视化（每帧新 SVG） | 无限 | ❌ 缓存无效 |

**关键区分**：
- **固定 SVG**（文件路径 / JS 硬编码字符串）：每帧哈希相同 → 只占 1 槽 → **128 个不同 SVG 完全够用**
- **动态 inline SVG**（每帧根据数据生成新字符串）：每帧哈希不同 → 每帧 miss → 频繁 LRU 淘汰 → **缓存无效**

#### 扩展建议

如果未来需要支持更多 SVG，可逐步升级：

1. **分离缓存**：文件路径和 inline SVG 各用独立缓存表，避免互相挤占
2. **标记常驻**：给常用 SVG 加 `pin` 标志，不参与 LRU 淘汰

### 性能分析

| 阶段 | 成本 | 说明 |
|-----|------|------|
| fnv1a 哈希 | ~5μs/500字符 | 单次遍历字符串 |
| 缓存查找 | < 1μs (128项) | 128 次 uint32 + size_t 比较（L1 缓存内） |
| 命中后渲染 | 零查找开销 | 直接取指针 |
| 未命中解析 | 100μs-1ms | 磁盘 I/O + XML 解析 |
| 命令入队 | ~100ns | 仅拷贝 20 字节固定结构 |

### 优点

- **极简单**：fnv1a 3 行代码，线性扫描 128 项可忽略（~4KB 数据，L1 缓存内）
- **无额外依赖**：fnv1a 是 microui 已有的 hash 算法
- **零解析开销**：首次解析后后续帧直接渲染
- **自动淘汰**：表满时 LRU 替换，`nsvgDelete` 释放旧内存
- **inline + 文件**：两种模式都走缓存，动态/静态都支持
- **帧安全**：`frame_id` 机制防止当前帧正在使用的槽位被误淘汰 → **杜绝野指针 SegFault**
- **防碰撞**：`content_len` 零成本校验，双重保险
- **命令流极小**：每个 SVG 命令仅 20 字节，不挤爆 microui 256KB 命令缓冲区

### 缺点

- **128 槽位仍有上限**：极端场景（>128 个不同 inline SVG）会挤占缓存
- **动态 SVG 缓存无效**：每帧生成新字符串的场景无法命中缓存，且极端情况下可能导致所有槽位都被标记为当前帧 → 退化为每帧解析
- **inline SVG 长度限制**：当前设计 4KB 缓冲，超大 SVG 需要截断或动态分配

---

## 备选方案对比

### 方案 B：临时文件（已否决）
- inline SVG 每帧写临时文件 → `nsvgParseFromFile`
- **问题**：磁盘 I/O 在 60fps 下不可接受

### 方案 C：文件缓存 + inline 不缓存
- 只缓存文件路径，inline 每帧解析
- **问题**：不变的 inline SVG 也会重复解析浪费 CPU

### robin-hood hashing（已否决）
- O(1) 查找，但删除/淘汰复杂，C 实现容易出错
- 对于 128 项线性扫描场景属于过度优化

---

## 改动文件清单

| 文件 | 改动 |
|------|------|
| `deps/microui/microui.h` | `mu_SvgCommand` 改为 `{ base; rect; cache_idx }`，移除 `path[1]` |
| `src/kwcc.c` | 新增缓存结构体、`svg_resolve()`、帧号管理；修改 `kwcc_queue_svg` 和 `svg` handler |
| `src/kwcc.h` | 新增 `kwcc_set_frame()` 声明（或整合到 `kwcc_process_js`） |
| `src/main.m` | 同步帧号；`MU_COMMAND_SVG` 渲染改为从缓存取 `image`，移除 `nsvgParseFromFile` |
