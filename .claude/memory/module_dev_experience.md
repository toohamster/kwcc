# 模块开发经验

> 每个模块独立一节，记录背景、教训、正确做法。后续新增模块直接加新节。

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

### 正确做法

1. **实施前先完整读方案**，逐行对照，确认理解正确再动手
2. **C 端优先测试**：纯 C → C handler → JS 集成，每层问题在本层验证
3. **命名规范写的时候就遵守**，不是做完再改
4. **方案怎么写就怎么做**，有疑问先讨论，确认后改方案再改代码
5. **`src/` 和 `deps/` 变更一律不重置**，先 `git diff` 确认
6. **用代理机制**（`g_kwcc_js_cfun_handlers`）避免改 `mqjs_stdlib.c`
7. **每次功能完成后更新测试记录**（`tests/TESTING_*.md`）
