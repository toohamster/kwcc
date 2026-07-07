# kwcc Lifecycle Shutdown 服务规范

> 定位：框架级基础服务，统一管理模块的资源回收生命周期
> 被依赖：所有持有 C 侧资源的服务层模块

---

## 背景

kwcc 的模块持有各类 C 侧资源（子进程、fd、buffer、连接等），需要在两个时机回收：

1. **运行时**：资源被标记废弃后，周期性回收（如 HTTP cancel 后的僵尸子进程）
2. **退出时**：应用关闭时，终止所有活跃操作并回收全部资源

此前没有统一机制，导致：
- `kwcc_http_cancel` 直接 kill + cleanup，子进程未退出就 memset → 僵尸进程
- 各模块各自猜测何时清理、如何清理，行为不一致
- 帧循环里混入清理逻辑，职责不清

lifecycle_shutdown 服务解决这些问题：**模块注册进来，服务统一管理检测、触发和回收**。

---

## 核心概念：服务治理模型

```
┌──────────────────────────────────────────────────────┐
│  main.m frame()                                      │
│    → kwcc_bus_publish("frame/tick")                  │
│                                                       │
│  lifecycle_shutdown 服务                              │
│    ← bus subscribe("frame/tick")                     │
│    → 遍历注册表，调 dirty_count()                     │
│    → dirty_count >= threshold → 调 shutdown(0)   │
│                                                       │
│  应用退出                                             │
│    → lifecycle_shutdown_force_all()                   │
│    → 逆序调所有 shutdown(1)                        │
└──────────────────────────────────────────────────────┘
```

**三角色**：

| 角色 | 职责 | 例子 |
|------|------|------|
| 触发源 | 发周期性通知，不关心谁在听 | main.m 发布 "frame/tick" |
| 服务 | 检测脏资源，调用回收 | lifecycle_shutdown |
| 模块 | 定义脏资源计数和回收行为 | HTTP、IO、Bus |

**触发源可替换**：当前用 "frame/tick" 每帧触发，未来可切换为定时器或跨进程消息，模块和服务不需要改。

---

## 规范一：服务 API

### 头文件

```c
// kwcc_lifecycle_shutdown.h

#include "kwcc_bus.h"  // 依赖 bus 的 subscribe/unsubscribe 和 callback 类型

/* 注册表条目：模块定义自己的脏资源计数和回收行为 */
typedef struct kwcc_lifecycle_shutdown_entry {
    const char *name;
    int  (*dirty_count)(void);          // 当前脏资源数量（必须非阻塞、必须准确）
    int  threshold;                     // dirty_count >= threshold 时触发 shutdown(force=0)
    void (*shutdown)(int force);       // 回收函数，force=1 全量释放，force=0 轻量释放
} kwcc_lifecycle_shutdown_entry_t;

/* 注册约定：entry 中的函数指针（dirty_count / shutdown）必须在服务生命周期内有效。
 * 通常用 static 函数，其地址在程序运行期间不变。 */

/* 服务 API */
void kwcc_lifecycle_shutdown_init(void);
void kwcc_lifecycle_shutdown_register(const kwcc_lifecycle_shutdown_entry_t *entry);
void kwcc_lifecycle_shutdown_force_all(void);
```

### API 约定

| API | 作用 | 调用方 |
|-----|------|--------|
| `init()` | 初始化服务，订阅 "frame/tick" | main.m init() |
| `register(entry)` | 注册模块进服务 | 模块 init 时 |
| `force_all()` | 逆序调所有 shutdown(force=1) | main.m cleanup() |

---

## 规范二：模块接入约定

### 接入 Checklist

```
□ 实现 dirty_count() 函数：返回当前脏资源数量
  - 必须非阻塞
  - 必须准确（直接读源数据计算，不维护手动计数器）

□ 实现 shutdown(int force) 函数
  - force=0：只回收已自然结束的资源，不阻塞，不发信号，幂等
  - force=1：终止所有活跃操作，可阻塞，幂等，有 log_info

□ 定义 threshold：dirty_count 达到多少触发 shutdown(force=0)

□ 服务层 API 加 g_shutdown 防御，拒绝 shutdown 后的新请求

□ 在模块 init 时调 lifecycle_shutdown_register()
```

### dirty_count 设计原则

**用函数指针实时计算，不维护手动计数器**：

| | 手动计数器（cancel+1 / reap-1） | 函数指针实时计算 |
|--|------|------|
| 一致性 | cancel/cleanup/request 三处同步维护，易遗漏 | 直接读源数据，永远准确 |
| 维护成本 | 多处同步改 | 只在一个函数里 |
| 开销 | 无 | 调用时遍历一次，bounded 资源可忽略 |

### shutdown(int force) 两种模式

| | `force=0`（轻量释放） | `force=1`（全量释放） |
|--|--|--|
| 触发方 | 服务 poll 检测到 dirty_count >= threshold | force_all() 退出时 |
| 阻塞 | **禁止** | **允许** |
| 发信号 | **禁止**（不发 SIGTERM/SIGKILL） | **允许**（SIGTERM → grace → SIGKILL） |
| 做什么 | 只回收已自然结束的资源 | 终止所有活跃操作 + 回收所有资源 |
| 日志 | 不打日志（可能高频） | `log_info` 记录回收结果 |
| 幂等 | 必须 | 必须 |

### `force=1` 实现模板

```c
void kwcc_xxx_shutdown(int force) {
    if (!force) {
        /* 轻量释放：见下方模板 */
        return;
    }

    /* ── 全量释放 ── */

    /* 1. 停止接受新请求 */
    g_kwcc_xxx_shutdown = 1;

    /* 2. 对所有活跃资源，发终止信号 */
    for (each active resource) {
        send_termination_signal(resource);
    }

    /* 3. grace period 等待（同步阻塞，有超时） */
    for (int attempt = 0; attempt < MAX_WAIT; attempt++) {
        if (all_reaped) break;
        usleep(WAIT_INTERVAL);
    }

    /* 4. 强制终止超时未退出的资源 */
    for (each still-active resource) {
        force_kill(resource);
    }

    /* 5. 释放所有 C 侧资源 */
    for (each resource) {
        kwcc_xxx_cleanup(resource);
    }

    log_info("xxx: shutdown complete");
}
```

### `force=0` 实现模板

```c
void kwcc_xxx_shutdown(int force) {
    if (force) {
        /* 全量释放：见上方模板 */
        return;
    }

    /* ── 轻量释放 ── */

    /* 只回收已自然结束的资源，不发信号，不等，不阻塞 */
    for (each resource) {
        if (!resource_is_abandoned(resource)) continue;        // 正常活跃，不管
        if (!resource_has_naturally_ended(resource)) continue;  // 还没结束，不管
        kwcc_xxx_cleanup(resource);                             // 已自然结束，回收
    }
}
```

### `g_shutdown` 防御标志

shutdown(force=1) 后，服务的任何 API 调用都应该安全返回：

```c
const char *kwcc_http_request(...) {
    if (g_kwcc_http_shutdown) {
        log_warn("http: rejecting request, module is shutting down");
        return NULL;
    }
    // ... 原有逻辑
}
```

---

## 规范三：服务内部行为

### poll 流程（每帧触发）

```
1. 收到 "frame/tick" 通知
2. 遍历注册表
3. 对每个 entry：
   a. 调 dirty_count()
   b. 如果 < threshold → 跳过
   c. 如果 >= threshold → 调 shutdown(force=0)
4. 完成
```

**幂等保证**：shutdown(force=0) 是幂等的，重复调用无害。如果上一帧没清完，下一帧继续。

### force_all 流程（退出时调用）

```
1. 逆序遍历注册表
2. 对每个 entry：
   a. 调 shutdown(1)
   b. log_info 记录结果
3. 服务自身标记为 shutdown
4. unsubscribe bus sub_id
5. 完成
```

**逆序原因**：和构造/析构对称原则一致，只是惯例。模块回收各自独立的资源，互不依赖，逆序不提供依赖保证。

### 服务状态

- `g_kwcc_lifecycle_shutdown_shutdown`：force_all 后设为 1，之后 register 和 poll 都不再执行
- poll 无最小间隔：由触发源决定频率（当前每帧一次）
- shutdown(force=1) 失败不阻塞后续模块：每个 shutdown(force=1) 应限时完成，不应长时间阻塞

---

## 规范四：和现有架构的衔接

### `kwcc_js_module_t.unload` 职责变化

| | 旧职责 | 新职责 |
|--|--------|--------|
| 调 shutdown(1) | unload 内部调 | lifecycle_shutdown_force_all() 统一调 |
| 清理 JS 侧资源 | unload 做 | unload 仍然做 |

**新约定**：
- `unload` 只负责清理 JS 侧资源（释放 JSValue、取消 $notify 注册等）
- `unload` **不再调** `shutdown(1)`，由服务统一管理
- `unload` 不发 bus 事件，不调 `ops->eval`，不假设其他模块还活着

### `kwcc_destroy_js` 职责变化

```c
void kwcc_destroy_js(JSContext *ctx) {
    /* 1. 逆序调用所有模块 unload（只清 JS 侧） */
    for (int i = g_kwcc_js_module_count - 1; i >= 0; i--) {
        if (g_kwcc_js_modules[i]->unload) {
            g_kwcc_js_modules[i]->unload(&g_kwcc_js_ops);
        }
    }

    /* 2. 销毁 JS 引擎 */
    if (ctx) {
        JS_FreeContext(ctx);
    }
}
```

不再负责调 shutdown(1)，这个职责转移到了 lifecycle_shutdown 服务。

### `main.m` 改动

```c
/* init() */
kwcc_lifecycle_shutdown_init();   // 初始化服务（内部订阅 frame/tick）

/* frame() */
kwcc_bus_publish("frame/tick", NULL, 0);   // 触发源，通知 lifecycle_shutdown poll

/* cleanup() */
kwcc_lifecycle_shutdown_force_all();  // 逆序调所有 shutdown(1)
kwcc_destroy_js(g_js_ctx);           // 只清 JS 侧 + FreeContext
kwcc_mempool_shutdown();             // 内存池最后
```

**顺序原则**：
- lifecycle_shutdown_force_all() 在 destroy_js 之前：先回收 C 侧资源，再清 JS 侧
- mempool 永远最后：其他模块可能依赖它

---

## 规范五：Cancel 语义

Cancel 是运行时操作，不是 shutdown。但 cancel 产生的"已放弃但子进程还在跑"的资源，由 lifecycle_shutdown 服务负责回收。

### Cancel = 逻辑放弃

| 模型 | cancel 含义 | 对外部资源的操作 |
|------|-----------|--------------|
| 直接终止 | "我命令你停下" | `kill(SIGTERM)` 主动杀 |
| 逻辑放弃 | "我不再关心你的结果" | 关掉 pipe，外部程序自然退出 |

**采用模型 2**：cancel = 逻辑放弃。

理由：
- cancel 的本意是"不要结果"，不是"杀进程"。外部程序退出是 cancel 的**后果**，不是 cancel 的**定义**
- 关 pipe 后 curl 写 pipe 失败 → SIGPIPE → 自然退出，比 SIGTERM 强杀更干净
- 超时机制已有保底：即使 curl 卡在连接阶段，超时后也会退出

### 三级关闭协议

```
Level 1: 关 pipe     → curl SIGPIPE 自然退出（通常 < 1ms）
Level 2: SIGTERM    → 礼貌请求退出（grace period 内等待）
Level 3: SIGKILL    → 强制终止（grace period 超时后）
```

cancel 时只做 Level 1。shutdown(force=1) 时按需升级到 Level 2/3。

### Cancel 后的资源回收

cancel 时**不调 cleanup，不等回收**。`in_use` 保持 1，pid 保留。

回收时机：
- **运行时**：lifecycle_shutdown poll 检测到 dirty_count >= threshold → 调 shutdown(0)
- **退出时**：force_all() 调 shutdown(1)
- **模块内部优化**：模块可自行实现额外回收路径（如 on-demand），不经过服务

---

## 模块接入示例

### HTTP 模块

```c
static int http_dirty_count(void) {
    int count = 0;
    for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
        if (g_kwcc_http_reqs[i].in_use && g_kwcc_http_reqs[i].pipe_read_fd < 0)
            count++;
    }
    return count;
}

static kwcc_lifecycle_shutdown_entry_t http_shutdown_entry = {
    .name        = "http",
    .dirty_count = http_dirty_count,
    .threshold   = 1,
    .shutdown    = kwcc_http_shutdown,
};

/* kwcc_http_init() 中注册 */
kwcc_lifecycle_shutdown_register(&http_shutdown_entry);
```

### IO 模块（未来）

```c
static int io_dirty_count(void) { return 0; }  // IO 没有"废弃"概念

static kwcc_lifecycle_shutdown_entry_t io_shutdown_entry = {
    .name        = "io",
    .dirty_count = io_dirty_count,
    .threshold   = 1,  // dirty_count 永远 0，poll 永远不触发
    .shutdown    = kwcc_io_shutdown,
};
```

dirty_count=0 → poll 永远不调 shutdown(force=0)，但 force_all 退出时仍会调 shutdown(force=1)。

### Bus 模块（未来）

```c
static int bus_dirty_count(void) { return 0; }  // 纯内存，无废弃资源

static kwcc_lifecycle_shutdown_entry_t bus_shutdown_entry = {
    .name        = "bus",
    .dirty_count = bus_dirty_count,
    .threshold   = 1,
    .shutdown    = kwcc_bus_shutdown,
};
```

---

## 服务自身依赖

lifecycle_shutdown 依赖 bus（订阅 "frame/tick"）。bus 也是被管模块，退出时被 force_all 调 shutdown。

**循环依赖分析**：
- 运行时：bus 正常工作，lifecycle_shutdown 正常订阅和收到通知，无问题
- 退出时：force_all 逆序调 shutdown(1)，bus 在最后被 shutdown。force_all 执行期间不依赖 bus 消息
- 无循环依赖

**服务自身不注册进自己**：lifecycle_shutdown 是框架基础设施，不是业务模块。force_all 执行完后，服务自身只是设 g_shutdown=1 + unsubscribe，无需注册。

---

## 注册顺序

模块的 shutdown 回收各自独立的 C 侧资源（HTTP 回收子进程/pipe，IO 回收 fd 槽位，Bus 回收订阅表），互不依赖。注册顺序不强制约束，force_all 逆序调用只是惯例。
