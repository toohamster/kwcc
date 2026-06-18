# 命名规范整改清单

## 规则

| 类别 | 前缀 | 示例 |
|------|------|------|
| 公共函数 | `kwcc_<module>_` | `kwcc_mempool_alloc` |
| 内部 static 函数 | `kwcc_<module>_` | `kwcc_mempool_slab_alloc` |
| 全局变量 | `g_kwcc_<module>_` | `g_kwcc_mempool_mgr` |
| static 变量 | `g_kwcc_<module>_` | `g_kwcc_mempool_last_gc_time` |
| 类型(struct/typedef) | `kwcc_<module>_` | `kwcc_mempool_slot_t` |
| 宏/枚举 | `KWCC_<MODULE>_` | `KWCC_MEMPOOL_MAX_TYPES` |
| mquickjs stub | `js_` | `js_print`（保留 mquickjs 惯例）|

---

## kwcc_mempool.c

| 行 | 类别 | 旧名 | 新名 |
|---|------|------|------|
| 15-22 | 宏 | `SLOTS_L0`~`SLOTS_L7` | `KWCC_MEMPOOL_SLOTS_L0`~`L7` |
| 24 | static 变量 | `slots_per_pool` | `kwcc_mempool_slots_per_pool` |
| 30 | static 变量 | `chunk_sizes` | `kwcc_mempool_chunk_sizes` |
| 725 | static 函数 | `tlv_write_le16` | `kwcc_mempool_tlv_write_le16` |
| 839 | static 函数 | `json_escape_len` | `kwcc_mempool_json_escape_len` |
| 850 | static 函数 | `json_write_escaped` | `kwcc_mempool_json_write_escaped` |

## kwcc_config.c

| 行 | 类别 | 旧名 | 新名 |
|---|------|------|------|
| 13 | static 函数 | `build_key` | `kwcc_config_build_key` |

## kwcc_bus.c

| 行 | 类别 | 旧名 | 新名 |
|---|------|------|------|
| 10 | 宏 | `TOPIC_MAP_SIZE` | `KWCC_BUS_TOPIC_MAP_SIZE` |
| 12 | static 变量 | `g_topic_map_count` | `g_kwcc_bus_topic_map_count` |

## kwcc_ui.h

| 行 | 类别 | 旧名 | 新名 |
|---|------|------|------|
| 11 | 宏 | `SVG_CACHE_SIZE` | `KWCC_UI_SVG_CACHE_SIZE` |
| 21 | 类型 | `svg_cache_t` | `kwcc_ui_svg_cache_t` |
| 23 | extern 变量 | `g_svg_cache` | `g_kwcc_ui_svg_cache` |
| 24 | extern 变量 | `g_frame_counter` | `g_kwcc_ui_frame_counter` |

## kwcc_ui.c

| 行 | 类别 | 旧名 | 新名 |
|---|------|------|------|
| 31 | 宏 | `MAX_WIN_DEPTH` | `KWCC_UI_MAX_WIN_DEPTH` |
| 37 | 宏 | `MAX_MODULES` | `KWCC_UI_MAX_MODULES` |
| 100 | static 函数 | `fnv1a` | `kwcc_ui_fnv1a` |
| 106 | static 函数 | `svg_resolve` | `kwcc_ui_svg_resolve` |

## kwcc_io.c

| 行 | 类别 | 旧名 | 新名 |
|---|------|------|------|
| 17 | static 变量 | `g_io_slots` | `g_kwcc_io_slots` |
| 18 | static 变量 | `g_io_max_fds` | `g_kwcc_io_max_fds` |

## 已确认规范（无需改动）

| 文件 | 说明 |
|------|------|
| kwcc_mempool.h | 全部规范 |
| kwcc_config.h | 全部规范 |
| kwcc_js.h | 全部规范 |
| kwcc_js.c | js_* stub 是 mquickjs 惯例，其余 kwcc_* 规范 |
| kwcc_bus.h | 全部规范 |
| kwcc_io.h | 全部规范 |
| kwcc_base.h | 全部规范 |
| kwcc.h | 全部规范 |
| main.m | 全部规范 |

---

## 总计：19 处
