# JS Bridge + Dispatch + HTTP Plugin 实施进度

> 创建于 2026-07-01
> 涵盖三个关联方案的实施状态：bridge 架构 + dispatch 机制 + HTTP 模块 Plugin 化

---

## 方案完成状态

| 方案文件 | 状态 | 说明 |
|----------|------|------|
| `js-bridge-architecture.md` | ✅ 已完成 | Facade + Plugin 架构：ops 类型 + 绑定 + $notify |
| `js-module-dispatch-plan.md` | ✅ 已完成 | Part 1 分发机制 + Part 2 core handler 迁移 |
| `js-http-implementation-plan.md` | 📋 待实施 | HTTP 模块 Plugin 化，7 步 |

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

## js-http-implementation-plan.md — 待实施

| Step | 内容 | 新增/修改文件 | 状态 |
|------|------|--------------|------|
| 1 | `kwcc_http.h` 隐藏 `phr_header`，改为 header 访问 API | `src/kwcc_http.h` | 📋 |
| 2 | `kwcc_http.c` 适配：删除回调注册表、header API、progress + 增量解析、僵尸回收 | `src/kwcc_http.c` | 📋 |
| 3 | `kwcc_js_http.h` 模块声明（只 expose `kwcc_js_http_module`） | `src/kwcc_js_http.h` | 📋 |
| 4 | `kwcc_js_http.c` 模块实现：`http_load` + `http_apis` + `http_on_bus_event`，通过 ops 操作 JS | `src/kwcc_js_http.c` | 📋 |
| 5 | `kwcc_js.c/h` 删除内联 HTTP 代码 + Makefile 更新 + 注册模块 | `src/kwcc_js.c`, `src/kwcc_js.h`, `Makefile` | 📋 |
| 6 | `promise.js` + `http.js`（MiniPromise + `$http.fetch` + `$notify.on('http')` 回调路由） | `app/runtime/promise.js`, `app/runtime/http.js`, `app/main.js` | 📋 |
| 7 | 编译验证 + 端到端测试 | — | 📋 |

### HTTP 方案注意事项

1. 方案文档中 JS 端仍写了 `kwcc_js_mquickjs_call`，实施时统一改为 `kwcc_js_call_c("http", "request", ...)` — 这是方案文档的遗留问题
2. Step 4+5 合并执行：删除内联代码必须先有替代文件，否则编译会断
3. C 端不存 JSValue 回调，JS 端通过 `$notify.on('http', handler)` 自己做 `callbacks[id]` 映射

---

## 当前代码状态

- `kwcc_js_call_c` 已上线，替代旧的 `kwcc_js_mquickjs_call`
- `g_kwcc_js_cfun_handlers` 代理表已清除
- core handler（`js_core_mempool_dump_stats/all`）已是 ops 签名
- `$notify` 通道已注入，`ops->notify_js` 可用（含 NULL id 防御）
- 74 个 ops 测试点全部通过
- HTTP 内联代码仍在 `kwcc_js.c` 中（`kwcc_js_http_request` / `kwcc_js_http_cancel` / `kwcc_register_http_js`），待 Step 4+5 迁移到独立文件
