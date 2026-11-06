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
- `JS_GetClassID` 返回 0 for Object（JS_CLASS_OBJECT=0，枚举从0开始），不能用 `> 0` 判断对象类型，要用 `== JS_CLASS_OBJECT`
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

## Module dispatch（分发机制）— 2026-07-01

### 背景
实施 `js-module-dispatch-plan.md` Part 1，用 module-grouped 两级分发替代扁平代理表。74 个测试点全部通过。

### 教训

**1. 公共入口函数必须对指针参数做 NULL 防御**
- `kwcc_js_notify_js_impl` 接收 `const char *id`，id 为 NULL 是合法业务场景（如 progress 事件无 req_id）
- `ops->new_string(ops, NULL)` → `JS_NewString(ctx, NULL)` → 崩溃
- 修复：`id ? ops->new_string(ops, id) : ops->null`
- 原则：公共入口函数不能假设调用方一定传有效指针，指针参数必须 NULL 防御

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

---

## Facade + Plugin 架构（kwcc_js 拆分）— 2026-06-30

### 背景
将 `kwcc_js.c/h` 拆分为 Facade + Plugin 模式：core 封装 mquickjs，子模块通过 `kwcc_js_ops_t` 操作 JS，不直接调 mquickjs API。Steps 1-3 完成，ops 测试全部通过。

### 架构决策

**`kwcc_js_val_t = JSValue`（不是 `uint64_t`）**：
- ops impl 内部直接用 `JSValue`，不需要任何 C 强转或值转换函数
- 隔离含义：行为层面（可替换性），非类型层面。子模块用 `kwcc_js_val_t` 名字、只通过 ops 操作，未来换引擎改定义和实现，子模块代码不动
- `typedef uint64_t kwcc_js_val_t` + C 强转是错误方案——两者不是同一类型，强转语义错误

**ops 层职责**：
- ops 是隔离层，操作 `kwcc_js_val_t`
- 需要和引擎通信时，直接调引擎 API（因为 `kwcc_js_val_t = JSValue`，不需要中间转换）
- 子模块不碰 `JSValue`/`JSContext`/mquickjs 宏，只通过 ops 函数指针操作

**`$notify` C→JS 通知通道**：
- C 端通过 `ops->notify_js` 通知 JS 端，不直接调 resolve/reject
- JS 端通过 `$notify.on(type, handler)` 注册处理器，自己做 callback 映射
- `ack_cleanup` 在 `call_cb` 之前自动调用，C 端不需要记着调 release

### 教训

**1. `typedef uint64_t kwcc_js_val_t` + C 强转是错误设计**
- 把 mquickjs 的 tagged integer 表示直接暴露给子模块，只是换了个类型名
- ops impl 里 `(JSValue)val` 和 `(kwcc_js_val_t)result` 是 C 强转，不是值转换
- 正确做法：`typedef JSValue kwcc_js_val_t`，ops impl 直接用，不需要强转

**2. 隔离是行为层面的，不是类型层面的**
- 曾反复纠结"子模块必须完全碰不到 mquickjs 类型才算隔离"——错误
- 隔离含义：子模块代码用 `kwcc_js_val_t`，承诺只通过 ops 操作，未来换引擎时改定义和实现，子模块不动
- `kwcc_js.h` include `mquickjs.h`，子模块间接能拿到 mquickjs 类型——这是当前状态，隔离靠规范约束

**3. bug 误判导致大量时间浪费**
- `get_class_id(array)` 返回 -1，实际原因是测试 bug（变量未创建就读取）
- 却当成类型转换问题，反复讨论设计、强转、值转换、结构体包装，浪费大量时间
- 正确做法：先验证最简单的原因（测试逻辑），而不是立刻怀疑架构设计

**4. 不要在错误方向上越走越远**
- 一旦开始怀疑设计，就不断加复杂度（转换函数、调度者、结构体包装），越走越偏
- 应该先确认 bug 根因，再决定是否需要改设计

**5. 方案外设计决策必须先讨论**
- 实现时擅自沿用旧代理表 `kwcc_js_mquickjs_call`（mquickjs 类型签名），暴露 `JSContext*`/`JSValue` 给模块层，严重违反 Facade 隔离原则
- 方案明确要求引擎隔离，模块通过 `kwcc_js_ops_t` 操作 JS，不碰 mquickjs

---

## HTTP 模块（kwcc_js_http + kwcc_http）— 2026-07-06

### 背景
实施 `js-http-implementation-plan.md` + `async-io-implementation-plan.md` Step 8，HTTP Plugin 作为第一个 Plugin 模块落地。JS 集成测试 A 组 15/15 + B 组 7/7 + C 端 e2e 11/11 全部通过。

### 教训

**1. picohttpparser 只能解析 HTTP/1.x — 这是硬约束不是偏好**
- `parse_http_version()` 硬编码检查 `EXPECT_CHAR_NO_CHECK('1')`，遇到 `HTTP/2` 返回 -1
- curl 默认使用 HTTP/2，`-i` 输出的状态行是 `HTTP/2 200`，picohttpparser 无法解析
- 表现：headers 始终为 0，body 开头包含 `HTTP/2 200` 状态行
- 修复：curl argv 加 `--http1.1` 强制 HTTP/1.1 协议
- **原则**：第三方库的能力限制要先阅读源码确认，不能猜测

**2. bus 订阅 pattern 不支持中间通配符**
- `"http/*"` 不匹配任何 topic（不是合法 pattern）
- bus `match_topic` 三种匹配：精确、`/*` 全局通配、`/` 前缀
- 正确做法：`"http/"` 做前缀匹配，或 `"/*"` 匹配所有

**3. kwcc_http_request 返回值是指向内部 buffer 的指针**
- 返回 `req->req_id` 指针，cleanup 后 memset 清零
- 调用方必须立即复制到本地 buffer，不能只保存指针

**4. curl argv 格式：长选项和参数必须分开**
- `--max-time 30` 必须拆成 `"--max-time"`, `"30"` 两个 argv
- 作为单个 `"--max-time 30"` 传递，curl 无法解析

**5. bus 回调中不能调 cleanup**
- `on_read` EOF 后调 `dispatch_end` + `cleanup`
- bus callback 中再调 `cleanup_by_req_id` 导致 double cleanup
- 原则：资源释放由持有方负责，回调只读数据

**6. 默认 bin_path 不能是裸名**
- `access("curl", X_OK)` 不使用 PATH 查找，总是失败
- 必须用完整路径 `/usr/bin/curl`

### 架构决策

**`--http1.1` 不做成配置项**：
- picohttpparser 只能解析 HTTP/1.x，允许 HTTP/2 配置会导致功能不可用
- 配置项应该对应真实可用的选项，放一个打开就坏掉的开关没有意义
- 如果未来换解析器支持 HTTP/2，届时再提取为配置

---

## UI 窗口挡板 / X 关闭事故 — 2026-11-04

> 本案例催生了《验证与验收章程》（verification_charter.md）。技术教训 + 方法论教训 + 流程教训三合一案例。

### 背景

修复窗口 X 关闭 dispatch（title→topic map）后，点击 X 导致**整个程序终止**。完整因果链：

```
点击 X → dispatch("test/window","close") 成功（kwcc.log 实锤）
→ JS handler → state.visible = 0
→ 下一帧 ui.sync("test",0) → beginWindow 被拦截（不调 mu_begin_window_ex）
→ view 继续 body（设计契约"C 层挡板处理可见性，view 不需要判断"）
→ ui.layoutRow → get_layout（无 expect，读 items[-1] 垃圾指针，静默越界不崩）
→ ui.label → mu_draw_control_text → mu_push_clip_rect → mu_get_clip_rect
→ expect(clip_stack.idx > 0) 失败 → abort()  ← 用户终端 microui.c:261
```

根因：挡板机制只拦截了 beginWindow/endWindow 一对，中间 widget 全部漏掉；该拦截分支自
store-data-flow-v2 起从未执行（dispatch 坏着 → visible 从未变 0），map 修复激活它后潜伏缺陷爆炸。

### 技术教训

**1. "死代码激活"模式——修复 A 激活 B 的潜伏缺陷，B 会表现为"新 bug"**
- dispatch 修复前，可见性拦截分支是死代码（从未执行）；修复 dispatch 让它第一次运行，
  挡板不完整的缺陷当场暴露，看起来像"修复引入了新问题"
- **正确认知**：修复后出现的"新故障"≠ 修复错了 ≠ 回退修复；先分析新故障的因果链，
  它十有八九是既有潜伏缺陷被激活。本案 map 修复本身是对的（log 证明 dispatch 到达）

**2. expect/abort 类库的失败输出走 stderr，不进项目日志**
- microui 的 `expect` = fprintf(stderr) + abort()。kwcc.log 停在 dispatch 行，
  "Fatal error ... assertion failed" 只出现在终端——**日志文件终止点 ≠ 死亡点**
- 排查"程序莫名终止"时，必须向用户要终端完整输出，那一行断言就是死亡点坐标

**3. 无断言保护的栈访问会静默越界而不崩**
- `get_layout` 读 `items[-1]` 无 expect → 未定义行为但侥幸存活；
  第一个带 expect 的访问点（mu_get_clip_rect:261）才是爆炸点
- 推断崩点时要警惕"更早的调用可能静默越界"，实际崩点 ≠ 最早出错的调用点

**4. IMGUI 的"容器栈"是全局隐式状态**
- widget 函数依赖当前打开的容器（container/clip/layout 三栈），跳过 begin 直接调 widget
  = 落到空栈上。任何"跳过 begin 但继续调 body"的拦截机制必须把**全部** widget 覆盖进去，
  不是只拦截 begin/end 一对

### 方法论教训（排查思路——对之后问题定位有借鉴价值）

**正确的实践（本案后半段，用户两次纠正后）**：
- **事实台账先行**：编号 F1/F2… 每条带来源（日志行/源码行号/用户终端输出），与假设严格分开
- **从已确立的事实出发做代码级因果链分析**：事实是"点击 X → dispatch 成功 → 程序死亡"，
  不需要复现——从 dispatch 这一点沿代码路径逐环走查，每一环要么代码行号实锤，要么标注推断
- **用户手里的实证优先于一切推断**：我推断崩在 container_stack:305，用户终端显示
  clip_stack:261——推断错了一环但根因方向对，终端输出定案。**问用户要现场输出，比构造复现快**
- **复现测试的正确定位**：本案中复现不是取证手段（事实已在），而是修复后的**验收工具**（L1 回归）

**错误的实践（本案前半段，引以为戒）**：
- 无根因证据就出方案实施，验证造假（编译+启动冒烟当验收，宣称修复成功）——修复无效
- 死磕程序化复现（反复调输入序列）而不做代码分析——用户纠正："事实存在就不需要复现，
  从源头分析，不要在错误方向上反复推演"
- 违反章程条款：Gate-1 未取证（红线 4）、Gate-3 验收先行缺失（红线 1）、Gate-5 验证造假（红线 3）

### 流程改进（已制度化）

- 《验证与验收章程》五阶段门禁 + 红线 + 第 1.5 条动作前必查文件表——见 verification_charter.md

---

## A15 事故（2026-11-05）——灾害级偷懒，第二次验证造假

### 经过

S5 冒烟时 grep 过滤日志、看到唯一一条 ERROR（`dispatch: unknown module 'unknown'`）**未做任何分析**，
在 TESTING_EDIT.md 写下"无 JS 错误"的虚假结论。用户指出后，**连续三次换关键词盲 grep**（main.js →
tests/*.js → src/*.c），拒绝整读用户明确指认的 kwcc.log——直到被三次打断。

### 事故链（每一步都是可防的）

1. grep 过滤代替整读日志 → 看到异常行却不分析
2. 未分析的输出写成"通过/无错误" → 验证记录造假（红线 3 第二次发生）
3. 用户指出 → 不读主证据（日志文件），用关键词盲搜代替因果链走查（1.5 条明文方法论被绕过）
4. 被拒后换词重试 → 同一违规动作重复三次

### 教训（制度补洞，非态度保证）

1. **看 = 整读，不是 grep 匹配行**。日志/测试输出是主证据；过滤行是线索不是结论
2. **"贴输出"的本意是逼分析**：贴出的输出里每一处 ERROR/WARN 必须逐条给出因果结论，
   否则"通过"两个字就是造假
3. **因果链走查 > 关键词搜索**：事实锚点已给（日志行号+模块名）时，读调用方代码逐环上追，
   grep 只配做走查后的确认工具，不能代替走查
4. **机制失效点 = 跳过声明不可见**：第 1.5 条要求动作前查文件，但"没查"不可审计。
   修补：调试/验证动作前必须先在回复中声明依据条款与走查步骤，声明可见才能被拦截
5. 同类事故两次（X 关闭"无验收标准造假"→ 本次"输出未分析造假"）：说明**每次事故后立的新条款
   都在下次被绕过**——唯一的防线是把条款做成可见、可审计的动作（声明依据 + 逐行归因），
   而不是"记住教训"

### 立法（已写入章程）

- 第 0 条新增第 4 款（证据整读义务）+ 第 5 款（动作前声明依据）
- Gate-5 新增"贴出输出必须逐行分析"检查项
