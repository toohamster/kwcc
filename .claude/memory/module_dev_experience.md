# 模块开发经验

> 每个模块独立一节，记录背景、教训、正确做法。后续新增模块直接加新节。
>
> ⚠️ 通用工作流规则见 [workflow_rules.md](workflow_rules.md)

---

## 内存池（mempool）— 2026-06-18

### 背景
实现 L0-L7 分层 Slab 内存池 + TLV 序列化 + Config 层 + $config JS API + 代理机制 + dump 功能。Phase 1-7 全部完成，24 个测试点全部通过。

### 教训

**1. 方案没逐行确认就实施**
- JS wrapper 方案定义是扁平格式（`$config.appSetInt`），我改成了嵌套格式（`$config.app.setInt`）
- 方案中 C handler 应该委托给 C 层 config 函数，我直接操作了 mempool
- 结果：测试不通过，反复重构

**2. 在 JS 层反复调试，不用 C 端测试**
- TLV 路径查询失败，在 JS 层反复改了 2 小时
- 写 C handler 测试后 5 分钟就定位到问题（`kwcc_js_value_to_tlv` 返回 NULL）

**3. 命名规范做完再统一改**
- 先做完再统一加 `kwcc_` 前缀，replace_all 引入重复前缀错误（`kwcc_mempool_kwcc_mempool_*`）

**4. 自己发挥改方案**
- 方案定义了 C handler 委托给 C 层，我直接操作 mempool
- 方案定义了 JS wrapper 调 `kwcc_config_set_int`，我改成了调 `kwcc_config_set_app_int`

**5. 构建产物盲目重置**
- 看到 `mqjs_stdlib.h` 被修改就 `git checkout --` 恢复，丢掉了 host tool 重新生成的正确内容

**6. 每次加函数都改 mqjs_stdlib.c**
- 声明/注册/原子表三者必须同步，容易漏

### 技术细节教训

**mquickjs API 陷阱**：
- `JS_ToBool(ctx, val)` 不存在 → 用 `val == JS_TRUE` 比较
- `JS_GetClassID` 返回 0 → mquickjs 精简版 Object 类 ClassID=0，不能用它判断对象类型
- `Object.keys(obj)` 在 JS_Eval 中不可用 → 直接调 C 侧 `js_object_keys(ctx, &obj, 1, &obj)` 内部 API
- `JS_IsObject` 不存在 → 用 `JS_GetClassID(ctx, val) == JS_CLASS_OBJECT`
- mquickjs 不支持 `...rest` 展开参数，JS 侧必须传固定参数

**命名整改陷阱**：
- `replace_all` 会破坏包含子串的正常名称（`vg` → `g_kwcc_vg` 把 `nanovg` 改成 `nanog_kwcc_vg`）→ 用精确 Edit 不用 replace_all
- 宏改名时注意数组初始化器里的引用（`SLOTS_L7` 9 处遗漏）
- 重复前缀问题（`kwcc_mempool_kwcc_mempool_chunk_sizes`）

**Config 层缺失函数**：
- `kwcc_config_get_app_slot` 不存在 → TLV 路径查询需要查 App 域 slot，C 层只有 `get_core_slot` → 需要补充

**JSON 类型处理**：
- `appSetJson` → 存 type=JSON，`appGet` 发现 type=JSON 时 C 侧 JSON.parse 返回对象
- `appSetJsonString` → 存 type=STRING，`appGet` 返回字符串，JS 侧可选 JSON.parse
- 两者不能混用

**TLV 转换**：
- `kwcc_js_value_to_tlv` 接收 JSValue 对象，返回 `uint8_t*` TLV 字节数组
- 嵌套对象递归处理：内层 type=OBJECT，叶子 type=FIELD
- 调用方负责 `free(tlv)`

### 架构决策

**const_lookup 在 set 中做即可，不需要在 alloc 中做**：
- 方案原文要求 alloc 时先查常量表，匹配到则不占 slab chunk
- 实际调用链：`config_set_app_int` → `mempool_alloc` → `mempool_set`
- `mempool_set` 中已有 const_lookup 兜底（line 518-531），匹配到则 `slot->data` 指向常量表，跳过 memcpy
- 在 set 中做和在 alloc 中做功能等价，提前到 alloc 只是省一次 slab 预留，收益可忽略
- **结论**：set 中的 const_lookup 已足够，不需要在 alloc 中再做一遍

### 方案编写陷阱

**去掉冗余变量时连带丢掉了核心逻辑**：
- 用户要求去掉 `kwcc_js_init_bus_whitelist` + `g_js_bus_whitelist` 数组（冗余缓存）
- 我在替换时把逗号分隔的白名单解析逻辑也一起删了
- 原因：没有先分析函数的完整职责，只盯着"去掉冗余变量"一个目标
- **正确做法**：改动前先看完整函数，确认哪些是冗余的、哪些是核心的，只删冗余部分

### 正确做法

1. **实施前先完整读方案**，逐行对照，确认理解正确再动手
2. **C 端优先测试**：纯 C → C handler → JS 集成，每层问题在本层验证
3. **命名规范写的时候就遵守**，不是做完再改
4. **方案怎么写就怎么做**，有疑问先讨论，确认后改方案再改代码
5. **`src/` 和 `deps/` 变更一律不重置**，先 `git diff` 确认
6. **用代理机制**（`g_kwcc_js_cfun_handlers`）避免改 `mqjs_stdlib.c`
7. **每次功能完成后更新测试记录**（`tests/TESTING_*.md`）

### 沟通教训

**回答不要敷衍，要让用户能看懂**
- 用户说看不懂时，不要用更复杂的话重复解释
- 要从用户角度出发，用简单的语言、具体的例子说明
- 回答前先看全貌，不要只盯着局部

**用户问题没结束前，不要写代码**
- 讨论阶段只做分析，得到用户肯定后再编码
- 用户的问题可能有多层，不要急着跳到实施

---

## Bus 重构（bus split）— 2026-06-24

### 背景
将 `kwcc_bus.c`（52 行，混杂 UI topic map + JS dispatch）拆分为三个独立模块：
- `kwcc_ui_bus.c/h` — UI→JS 事件桥接
- `kwcc_bus.c/h` — 通用 C Pub/Sub，零 mquickjs 依赖
- `kwcc_base.h/c` — topic 清洗 + 校验工具
7 步全部完成，19 个测试点全部通过。

### 架构决策

**`/*` 通配符是 topic group 的一种 pattern，不是独立机制**：
- `/*` 存储在 topic group 链表中，和 `"calc/btn0"` 用同一个数据结构
- `kwcc_bus_match_topic` 三种匹配：精确、`/*` 通配、`/` 前缀
- 曾讨论把 `/*` 单独分离为 global_cbs 数组，但引入两套存储 + unsubscribe 归属问题，复杂度翻倍不可取

**两套通配符约定，各用各的**：
- bus subscribe：`/*` = 匹配所有 topic
- JS 白名单：`*` = 全部转发，逗号分隔前缀列表 = 前缀匹配
- 两者语义不同，不要混淆

**JS 白名单不额外缓存**：
- 每次 `kwcc_js_on_bus_event` 直接从 config 读 `bus/js_whitelist`
- config 层本身有 mempool 缓存，再套一层是多余的

**`KWCC_BUS_WILDCARD` 常量定义在 `kwcc_bus.h`**：
- `kwcc_bus.c` 的 `kwcc_bus_match_topic` 用该常量
- `kwcc_base.c` 的 `topic_check` 中 `strcmp(topic, "/*")` 保留硬编码 — 这是校验规则，不是通配符语义

**`KWCC_BUS_GROUP_MAX_CB = 16` 未经论证**：
- 写方案时拍脑袋选的数字，没有数据支撑
- 当前实际使用每个 topic 只有 1-2 个 subscriber

### 教训

**1. static 函数也要遵守命名规范**
- `match` 太泛，改为 `kwcc_bus_match_topic`
- 主流开源项目（Linux kernel、Nginx、SQLite）static 和 non-static 用同一模块前缀
- Redis 不加前缀，被认为是代码风格弱点

**2. 命名规范写的时候就遵守**
- 讨论时随手写 `g_global_cbs`，没有加 `kwcc_bus_` 前缀
- `module_dev_experience.md` 已经记录过这条教训，又犯了

**3. sanitize bug：`out[j] = '\0'` 写在了 `/*` 追加之前**
- 导致 `/*` 被截断为空字符串
- 修复：把 `out[j] = '\0'` 移到函数末尾

**4. 不要提出"看似更优"但引入复杂度的方案**
- 分离 `/*` 到 global_cbs 看似语义更清晰，但 unsubscribe 要遍历两边
- 简单方案优于"更优雅"的方案

### 技术细节

**`kwcc_bus_match_topic(pattern, topic)` 三种匹配**：
- 精确：`strcmp(pattern, topic) == 0`
- 通配：`strcmp(pattern, KWCC_BUS_WILDCARD) == 0`，匹配所有
- 前缀：pattern 以 `/` 结尾且 `strncmp(pattern, topic, plen) == 0`

**topic 清洗规则**：
- 只保留 `A-Z a-z 0-9 / _`
- 末尾 `/*` 保留（通配符），其余 `*` 丢弃
- 四个入口都加 sanitize + check：bus publish、bus subscribe、ui_bus dispatch、ui_bus bind_topic
