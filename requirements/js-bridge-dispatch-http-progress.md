# JS Bridge + Dispatch + HTTP Plugin 实施进度

> 创建于 2026-07-01
> 涵盖三个关联方案的实施状态：bridge 架构 + dispatch 机制 + HTTP 模块 Plugin 化

---

## 方案完成状态

| 方案文件 | 状态 | 说明 |
|----------|------|------|
| `js-bridge-architecture.md` | ✅ 已完成 | Facade + Plugin 架构：ops 类型 + 绑定 + $notify |
| `js-module-dispatch-plan.md` | ✅ 已完成 | Part 1 分发机制 + Part 2 core handler 迁移 |
| `js-http-implementation-plan.md` | ✅ 已完成 | HTTP 模块 Plugin 化，Step 1-7 完成 |

---

## js-bridge-architecture.md — 已完成

| Step | 内容 | 状态 |
|------|------|------|
| 1 | `kwcc_js.h` 新增类型和接口定义（`kwcc_js_val_t` / `kwcc_js_ops_t` / `kwcc_js_module_t`） | ✅ |
| 2 | `kwcc_js.c` 实现 ops 绑定 + `$notify` 注入 + 模块注册 | ✅ |
| 3 | ops 接口测试（74 个测试点全部通过） | ✅ |
| 4 | moduleMan + `kwcc_js_call_c` 分发机制 → 详见 dispatch plan | ✅ |
| 5 | core handler 签名迁移 → 详见 dispatch plan Part 2 | ✅ |
| 6 | 实施第一个 Plugin（HTTP） → 详见 HTTP plan | 📋 |

## js-module-dispatch-plan.md — 已完成

### Part 1: 分发机制实施

| Step | 内容 | 状态 |
|------|------|------|
| 1 | `kwcc_js.h` 类型变更（`kwcc_js_handler_t` / `kwcc_js_api_t` / `kwcc_js_dispatch_t` / `register_cfun` → `apis`） | ✅ |
| 2 | `kwcc_js.c` 实现分发机制（module 分组 + `kwcc_js_dispatch_add` / `kwcc_js_dispatch_call` + `kwcc_js_call_c`） | ✅ |
| 3 | mqjs_stdlib 更新 + Stage 1 重建 + JS 调用点迁移（`kwcc_js_mquickjs_call` → `kwcc_js_call_c`） | ✅ |
| 4 | 清理：移除代理表 + 旧声明（`g_kwcc_js_cfun_handlers` / `kwcc_js_cfun_t` / `kwcc_js_cfun_entry_t`） | ✅ |
| 5 | 测试更新 + 编译验证 | ✅ |

### Part 2: core handler 签名迁移

| Step | 内容 | 状态 |
|------|------|------|
| 2a | mempool 组（2 个）签名迁移（已在 Part 1 实施中同步完成） | ✅ |
| 2b | 删除适配器 + 更新 `g_kwcc_js_core_apis[]` + 清理 `.h` 声明 | ✅ |
| 2c | 编译验证 + 测试 | ✅ |

## js-http-implementation-plan.md — 已完成（Step 1-7）

| Step | 内容 | 新增/修改文件 | 状态 |
|------|------|--------------|------|
| 1 | `kwcc_http.h` 隐藏 `phr_header`，改为 header 访问 API | `src/kwcc_http.h` | ✅ |
| 2 | `kwcc_http.c` 适配：删除回调注册表、header API、progress + 增量解析、僵尸回收 | `src/kwcc_http.c` | ✅ |
| 3 | `kwcc_js_http.h` 模块声明（只 expose `kwcc_js_http_module`） | `src/kwcc_js_http.h` | ✅ |
| 4 | `kwcc_js_http.c` 模块实现：`http_load` + `http_apis` + `http_on_bus_event`，通过 ops 操作 JS | `src/kwcc_js_http.c` | ✅ |
| 5 | `kwcc_js.c/h` 删除内联 HTTP 代码 + Makefile 更新 + 注册模块 | `src/kwcc_js.c`, `src/kwcc_js.h`, `Makefile` | ✅ |
| 6 | `promise.js` + `http.js`（MiniPromise + `$http.fetch` + `$notify.on('http')` 回调路由） | `app/runtime/promise.js`, `app/runtime/http.js`, `app/main.js` | ✅ |
| 7 | 编译验证 + 端到端测试 | — | ✅ |

### HTTP 方案注意事项

1. ~~方案文档中 JS 端仍写了 `kwcc_js_mquickjs_call`，实施时统一改为 `kwcc_js_call_c("http", "request", ...)` — 这是方案文档的遗留问题~~ 已修复
2. ~~Step 4+5 合并执行：删除内联代码必须先有替代文件，否则编译会断~~ 已完成
3. C 端不存 JSValue 回调，JS 端通过 `$notify.on('http', handler)` 自己做 `callbacks[id]` 映射
4. MiniPromise bug 修复：REJECTED 状态未透传给 `.then()` 返回的新 promise，导致 `.catch()` 永远不执行。修复：`.then()` 中加 `else if (self.status === "REJECTED") { reject(self.value); }`

---

## Step 8: JS 端 HTTP 集成测试（待实施）

> 文件：`tests/test_http_js.js`，在 `app/main.js` 末尾 `load("tests/test_http_js.js")`
> 风格：和 `tests/test_config_js.js` 一致，用 `assert(cond, name)` + 计数

### A 组：同步测试（无需网络，立即验证）

| # | 测试 | 输入 | 预期 |
|---|------|------|------|
| A1 | `$http` 对象存在 | `typeof $http` | `"object"` |
| A2 | `$http.state` 存在且 activeRequests=0 | `$http.state.activeRequests` | `0` |
| A3 | `$http.cancel` 是函数 | `typeof $http.cancel` | `"function"` |
| A4 | `$http.cancel` 是函数 | `typeof $http.cancel` | `"function"` |
| A5 | `$http.fetch` 是函数 | `typeof $http.fetch` | `"function"` |
| A6 | `$http.callbacks` 存在且为空 | `$http.callbacks` 无 key | true |
| A7 | `$notify` 对象存在 | `typeof $notify` | `"object"` |
| A8 | `$notify.on` 是函数 | `typeof $notify.on` | `"function"` |
| A9 | `$notify.emit` 是函数 | `typeof $notify.emit` | `"function"` |
| A10 | `$notify.registry` http handler 已注册 | `$notify.registry["http"]` | 非 undefined |
| A11 | `MiniPromise` 构造函数存在 | `typeof MiniPromise` | `"function"` |
| A12 | MiniPromise 同步 resolve | `new MiniPromise(function(r){r(42);}).then(function(v){v===42})` | true |
| A13 | MiniPromise 同步 reject | `new MiniPromise(function(_,r){r("err");}).catch(function(e){e==="err"})` | true |
| A14 | `kwcc_js_call_c` 全局函数存在 | `typeof kwcc_js_call_c` | `"function"` |
| A15 | 参数不足返回 undefined | `kwcc_js_call_c("http", "request")` | undefined |
| A16 | 未注册模块返回 undefined | `kwcc_js_call_c("unknown", "func")` | undefined |

### B 组：异步测试（需要 curl + 网络）

用 MiniPromise 链做异步断言——响应到达时 `.then`/`.catch` 自动执行。

**B1: fetch GET 成功**
```js
$http.fetch("https://httpbin.org/get").then(function(data) {
    assert(data.status === 200, "B1: GET status === 200");
    assert(data.body.length > 0, "B1: GET body non-empty");
    assert(typeof data.headers === "object", "B1: GET headers is object");
    assert(data.reqId.length > 0, "B1: GET reqId present");
    assert($http.state.activeRequests === 0, "B1: activeRequests === 0 after resolve");
    test_async_done = test_async_done + 1;
});
```

**B2: fetch 404**
```js
$http.fetch("https://httpbin.org/status/404").then(function(data) {
    assert(data.status === 404, "B2: 404 status === 404");
    test_async_done = test_async_done + 1;
});
```

**B3: fetch POST**
```js
$http.fetch("https://httpbin.org/post", {method: "POST", body: "hello"}).then(function(data) {
    assert(data.status === 200, "B3: POST status === 200");
    test_async_done = test_async_done + 1;
});
```

**B4: cancel → reject**
```js
var reqId = kwcc_js_call_c("http", "request", "GET", "https://httpbin.org/delay/5");
if (reqId) {
    $http.callbacks[reqId] = {
        resolve: function() { assert(false, "B4: cancel should not resolve"); },
        reject: function(err) { assert(err.error === "cancelled", "B4: cancel reject error === cancelled"); test_async_done = test_async_done + 1; },
        onProgress: null
    };
    $http.state.activeRequests = $http.state.activeRequests + 1;
    $http.cancel(reqId);
}
```

**B5: fetch 无效 URL → reject**
```js
// curl 无法连接时 C 端会 dispatch error 事件
$http.fetch("http://127.0.0.1:1/fail").catch(function(err) {
    assert(err !== null, "B5: invalid URL reject received");
    test_async_done = test_async_done + 1;
});
```

### 异步测试 summary 时机

B 组是异步的，不能在文件末尾立即打印 summary。方案：
- B 组开始前记录 `test_async_total = 5`
- 每个 B 组测试完成时 `test_async_done++`
- 用 `setTimeout` 轮询检查 `test_async_done === test_async_total`，满足时打印 B 组 summary
- 超时（10 秒）后打印 timeout 提示

### 前置条件检查

B 组开始前检查 `curl` 是否可用：`kwcc_js_call_c("http", "request", "GET", "https://httpbin.org/get")` 返回 reqId 则可用，返回 undefined 则跳过 B 组。

---

## 当前代码状态

- `kwcc_js_call_c` 已上线，替代旧的 `kwcc_js_mquickjs_call`
- `g_kwcc_js_cfun_handlers` 代理表已清除
- core handler（`js_core_mempool_dump_stats/all`）已是 ops 签名
- `$notify` 通道已注入，`ops->notify_js` 可用（含 NULL id 防御）
- 74 个 ops 测试点全部通过
- HTTP 已独立为 Plugin 模块（`kwcc_js_http.c/h`）
- `kwcc_http.h` 已隐藏 `phr_header`，提供 header 访问 API
- `kwcc_http.c` 已删除回调注册表，新增 progress/僵尸回收
- MiniPromise bug 已修复（REJECTED 透传 + onRejected 执行，和 ES6 Promise 行为一致）
- JS 端集成测试 A 组 16 passed ✅，B 组待 curl 配置修复后验证
- config 层待补齐 `kwcc_config_get_core_tlv_path`（Step 8 补充）
- curl 路径问题待修复：`$config.coreSetTlv("http", {bin_path: "/usr/bin/curl"})`

### Step 8 补充：config 层补齐 core TLV 子路径读取

**问题**：JS 端 `$config.coreSetTlv("http", {bin_path: "/usr/bin/curl", timeout: "30"})` 写入 key `c.http`（TLV 类型）。C 端 `kwcc_config_get_core("http/bin_path", "curl")` 查 key `c.http/bin_path`（不存在），返回默认值 `"curl"`。`access("curl", X_OK)` 不走 PATH 查找，所以找不到。

**根因**：core 域缺少 TLV 子路径读取的 C API。app 域有 `kwcc_config_get_app_slot` + JS 端 `getTlvPath`，但 core 域只有 `kwcc_config_get_core`（按 key 精确读）和 `kwcc_config_get_core_slot`（返回 slot 指针），没有封装 TLV 子路径读取。

**设计决策**：

经过讨论，方案从最初的"简单函数返回 `const char *`"演进为三层架构：

1. **mempool 层**：`kwcc_mempool_tlv_get_path` 新增 `uint8_t *out_type` 参数，输出子字段类型（FIELD/OBJECT/ARRAY）
2. **base 层**：新增 `kwcc_base_str_t` 字符串值类型 + 生命周期管理函数
3. **config 层**：新增 `kwcc_config_tlv_t` TLV 视图 struct + 一组 get 方法

**改动明细**：

#### 1. `kwcc_mempool.h/c` — tlv_get_path 加 out_type

- 签名：`const char *kwcc_mempool_tlv_get_path(data, len, path, &out_len, &out_type)`
- `out_type` 可传 NULL（向后兼容），现有调用处全部传 NULL
- 输出值：`KWCC_MEMPOOL_TLV_FIELD(0x01)` / `TLV_OBJECT(0x02)` / `TLV_ARRAY(0x03)`
- 改动文件：`kwcc_mempool.h`、`kwcc_mempool.c`、`kwcc_js.c`、`kwcc_config.c`、`test_mempool.c`、`test_config.c`、`test_config_js.c`

#### 2. `kwcc_base.h/c` — kwcc_base_str_t 字符串值类型

```c
typedef struct {
    const char *val;   // null-terminated C 字符串（始终 malloc，始终需 free）
    size_t      len;   // 字符串长度（不含 \0）
} kwcc_base_str_t;

/* 从 data[0..len-1] 创建 null-terminated 字符串（始终 malloc，行为一致） */
kwcc_base_str_t kwcc_base_str_new(const char *data, size_t len);

/* 释放（内部 free val） */
void kwcc_base_str_free(kwcc_base_str_t *s);

/* 类型转换函数 */
const char *kwcc_base_str_cstr(const kwcc_base_str_t *s, const char *def);
int32_t     kwcc_base_str_int(const kwcc_base_str_t *s, int32_t def);
double      kwcc_base_str_double(const kwcc_base_str_t *s, double def);
```

- `kwcc_base_str_new`：始终 malloc(len+1)，拷贝 len 字节，追加 `\0`。default 值也 malloc，行为一致，调用者始终需 `kwcc_base_str_free`
- `kwcc_base_str_cstr`：返回 null-terminated C 字符串指针（`s->val`），找不到返回 def
- `kwcc_base_str_int`：`atoi(s->val)`，找不到返回 def
- `kwcc_base_str_double`：`atof(s->val)`，找不到返回 def
- 删除之前的 `kwcc_base_str_cstr(const char *data, size_t len)` 函数（返回裸指针，无法区分所有权）

#### 3. `kwcc_config.h/c` — kwcc_config_tlv_t TLV 视图 + get 方法

```c
typedef struct {
    const uint8_t *data;   // slot 内部 TLV 数据指针（不需 free）
    size_t         size;   // TLV 数据长度
} kwcc_config_tlv_t;

/* 从 core 域获取 TLV 对象 */
kwcc_config_tlv_t kwcc_config_get_core_tlv(const char *key);

/* 获取子字段值（返回 kwcc_base_str_t，需 free） */
kwcc_base_str_t kwcc_config_tlv_get_field(const kwcc_config_tlv_t *tlv, const char *path);

/* 获取子字段类型 */
uint8_t kwcc_config_tlv_get_type(const kwcc_config_tlv_t *tlv, const char *path);

/* 获取嵌套对象（返回 kwcc_config_tlv_t 子视图） */
kwcc_config_tlv_t kwcc_config_tlv_get_object(const kwcc_config_tlv_t *tlv, const char *path);
```

- `kwcc_config_get_core_tlv(key)`：`kwcc_config_get_core_slot(key)` → 检查 slot->type == TLV → 返回 `{slot->data, slot->size}`
- `kwcc_config_tlv_get_field(tlv, path)`：`kwcc_mempool_tlv_get_path(tlv->data, tlv->size, path, &vlen, &subtype)` → `kwcc_base_str_new(val, vlen)` 返回
- `kwcc_config_tlv_get_type(tlv, path)`：调 tlv_get_path 取 subtype
- `kwcc_config_tlv_get_object(tlv, path)`：tlv_get_path 检查 subtype == TLV_OBJECT → 返回 `{val, vlen}` 子视图
- **删除旧的** `kwcc_config_get_core_tlv_path` 函数

#### 4. `kwcc_http.c` — 改用新 API

```c
kwcc_config_tlv_t http_cfg = kwcc_config_get_core_tlv("http");
kwcc_base_str_t bin_val = kwcc_config_tlv_get_field(&http_cfg, "bin_path");
const char *bin_path = kwcc_base_str_cstr(&bin_val, "curl");
kwcc_base_str_t timeout_val = kwcc_config_tlv_get_field(&http_cfg, "timeout");
int32_t timeout = kwcc_base_str_int(&timeout_val, 30);
// ... 使用 bin_path 和 timeout ...
kwcc_base_str_free(&bin_val);
kwcc_base_str_free(&timeout_val);
```

#### 5. 其他已有改动（之前 session 已完成）

- `kwcc_js_http.c` — 删除 `$http.config` 方法 ✅
- `http.js` — 加 `$config.coreSetTlv("http", { bin_path: "/usr/bin/curl", timeout: "30" })` ✅
- `test_http_js.js` — A 组 15 passed ✅（移除了 `$http.config` 测试）
- MiniPromise bug 修复 ✅

---

## Step 9: 请求级超时配置（待实施）

**背景**：当前 `timeout` 是全局配置，所有请求共用。但某些请求（如下载）耗时较长，需要单独设置超时时间。

**思路**：`$http.fetch(url, { timeout: 120 })` 在 options 中传入请求级超时，覆盖全局默认值。C 端 `kwcc_http_request` 接收额外 timeout 参数。

---

## 当前代码状态
