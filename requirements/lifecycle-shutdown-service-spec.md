# kwcc Lifecycle Shutdown 服务规范 v2（窄边界）

> 定位：框架级基础服务，**退出时的统一清理调度器**
> 演进：v1 → v2 见 [lifecycle-shutdown-v2-discussion.md](lifecycle-shutdown-v2-discussion.md)。
> v2 核心变化：职责收敛为仅 force_all，runtime 回收交给模块自治，服务零依赖（不再依赖 bus）。
> HTTP 接入细节见 [http-shutdown-implementation-plan.md](http-shutdown-implementation-plan.md)

---

## 背景

kwcc 的模块持有各类 C 侧资源（子进程、fd、buffer 等），需要在两个时机回收：

1. **运行时**：资源被标记废弃后，周期性回收（如 HTTP cancel 后的僵尸子进程）
2. **退出时**：应用关闭时，终止所有活跃操作并回收全部资源

此前没有统一机制，导致：
- `kwcc_http_cancel` 直接 kill + cleanup，子进程未退出就 memset → 僵尸进程
- 退出路径（cleanup / kill 信号）没有统一的资源回收入口

**v2 职责划分**：
- **运行时回收 = 模块自治**。模块在自己的 per-frame 循环里回收（如 HTTP 在 `check_progress()` 中 reap cancelled slot）。服务不参与。
- **退出时回收 = 服务统一调度**。`force_all()` 逆序调所有模块的 shutdown()。这是服务唯一职责，也是它不可替代的原因：注册表管理、逆序调用、kill 信号可达、全局 `exiting` 守卫。

---

## 规范一：服务 API

### 头文件

```c
// kwcc_lifecycle_shutdown.h

/* 注册表条目：模块提供自己的全量回收函数 */
typedef struct kwcc_lifecycle_shutdown_entry {
    const char *name;
    void (*shutdown)(void);   /* 全量释放；只应在 force_all 上下文中执行 */
} kwcc_lifecycle_shutdown_entry_t;

/* 注册约定：entry 中的 name / shutdown 指向目标和 entry 本身必须是静态存储期，
 * 不得是栈上临时变量。服务只保存指针值，不复制指向目标。 */

/* 服务 API */
void kwcc_lifecycle_shutdown_init(void);
void kwcc_lifecycle_shutdown_register(const kwcc_lifecycle_shutdown_entry_t *entry);
void kwcc_lifecycle_shutdown_force_all(void);
int  kwcc_lifecycle_shutdown_is_exiting(void);
```

### API 约定

| API | 作用 | 调用方 |
|-----|------|--------|
| `init()` | 清零注册表和状态 | main.m init()（必须在各模块 init 之前） |
| `register(entry)` | 注册模块进服务 | 模块 init 时（如 kwcc_http_init） |
| `force_all()` | 逆序调所有模块 shutdown() | main.m cleanup() + kill 信号处理 |
| `is_exiting()` | 返回 `state.exiting`，模块运行期防御用 | 模块 shutdown 入口 |

### shutdown 契约（v2 简化）

v1 的 `shutdown(int force)` 双模式（force=0 轻量回收 / force=1 全量释放）在 v2 简化为 `shutdown(void)` 单模式：

- force=0（轻量回收）的唯一调用方是 runtime poll，而 runtime 路径已在 v2 移除——**没有调用方的模式不进契约**
- 轻量回收降级为模块内部实现细节（HTTP 的 check_progress 自治 reap），不经过服务
- 模块 shutdown() 语义 = 全量释放：停新请求 → 终止活跃操作 → 回收全部资源，幂等，打 `log_info`

### is_exiting 运行期防御（讨论 4）

模块 shutdown() 入口检查，防止运行期误调导致正常请求被中断：

```c
void kwcc_http_shutdown(void) {
    if (!kwcc_lifecycle_shutdown_is_exiting()) {
        log_warn("http: shutdown called outside force_all, skipped");
        return;
    }
    /* ... 全量释放 ... */
}
```

- `exiting == 1` → 正常执行（在 force_all 上下文中）
- `exiting == 0` → `log_warn` + 跳过（运行期误调）

不提供 `get_state()`：没有真实监控需求，出现时再加是向后兼容扩展（讨论 2）。

---

## 注册容量

`KWCC_LIFECYCLE_SHUTDOWN_MAX_MODULES = 8`（论证：kwcc 是编译期模块个位数的架构，当前客户仅 http 1 个，未来候选 io/bus/mempool/js 合计 ≤ 6；entry 为两个指针 16 字节，8 个共 128 字节静态开销可忽略）。注册满时 `log_warn` 拒绝并保留其余 entry，不静默失败。

---

## 规范二：模块接入约定

### 接入 Checklist

```
□ 实现 void shutdown(void)：全量释放（停新请求 → 终止活跃操作 → 回收全部资源），幂等，log_info
□ shutdown 入口检查 kwcc_lifecycle_shutdown_is_exiting()，非退出上下文 warn + 跳过
□ 服务层 API 加 g_shutdown 防御，拒绝 shutdown 后的新请求
□ 运行期回收：模块自治（在 per-frame 循环内），不依赖服务
□ 模块 init 时调 kwcc_lifecycle_shutdown_register()
```

---

## 规范三：服务内部行为

### 状态结构（内聚，讨论 1）

```c
typedef struct {
    int exiting;        /* 0=运行中，1=force_all 已开始 */
    int module_count;   /* 已注册 entry 数量 */
} kwcc_lifecycle_shutdown_state_t;
```

不订阅 bus、不 poll（讨论 3），服务对 bus 零依赖。

### force_all 流程

```
1. 幂等守卫：exiting 已为 1 → log_warn + return（防 cleanup 和 kill 信号双重触发）
2. exiting = 1
3. 逆序遍历注册表，调 entry->shutdown()（直接调用，不走 bus）
4. log_info 记录完成（module_count 个模块）
```

**不走 bus 的原因（讨论 5）**：force_all 可能在 kill 信号处理中被调用，此时 bus 可能不可用（模块正在被 unload），走 bus 会失败且无报错。退出时调用方明确知道要做什么，不需要解耦。

**逆序原因**：和构造/析构对称原则一致，只是惯例。模块回收各自独立的资源，互不依赖，逆序不提供依赖保证。

**不需要 grace period（讨论 6）**：curl 是可靠响应 SIGTERM 的轻量子进程，SIGTERM + waitpid(阻塞) 足够，不需要 sleep 循环或 SIGKILL 升级。

### 幂等场景

cleanup()（窗口正常关闭）先执行 force_all，期间用户又发 SIGINT → 信号处理函数调 force_all → 幂等守卫 warn + return → `_exit()`。双重触发安全。

---

## 规范四：退出时序（main.m 衔接）

### init() 顺序

```c
kwcc_mempool_init();            /* 0. 内存池 */
kwcc_io_init();                 /* 1. I/O reactor */
kwcc_lifecycle_shutdown_init(); /* 2. 生命周期服务（必须在 http_init 之前） */
kwcc_http_init();               /* 3. HTTP 引擎（内部注册 entry） */
g_js_ctx = kwcc_create_js();    /* 4. JSContext */
...
```

**顺序约束**：`lifecycle_init` 必须在 `http_init` 之前——http_init 内部调 register，服务必须已初始化。

同批注册 kill 信号处理（讨论 7）：

```c
signal(SIGINT, kwcc_on_signal);
signal(SIGTERM, kwcc_on_signal);

static void kwcc_on_signal(int sig) {
    kwcc_lifecycle_shutdown_force_all();
    _exit(sig);   /* _exit 而非 exit：跳过 atexit/stdio，防止重入 cleanup */
}
```

**已知权衡**：信号处理函数内的 log/fprintf 不是 async-signal-safe，理论上存在 stdio 锁死窗口。kwcc 是单线程桌面应用，SIGINT/SIGTERM 场景下窗口极小，接受此权衡；完整的 async-signal-safe 关闭（信号只置标志 + 主循环检查）复杂度不成比例，不做。

**kill 路径只保证 C 侧资源回收**（子进程/pipe/fd），JS 堆随进程销毁由 OS 回收，不调 destroy_js。

### frame() —— 无改动

v2 服务不订阅 bus，frame() 不需要发布 frame/tick。HTTP 运行时回收已由 `kwcc_http_check_progress()` 自治完成（现有调用点不变）。

### cleanup() 顺序

```c
static void cleanup(void) {
    log_info("=== kwcc exiting ===");
    kwcc_lifecycle_shutdown_force_all();  /* C 侧资源全量回收（此时日志仍打开） */
    if (g_kwcc_vg) { nvgDeleteGL3(g_kwcc_vg); g_kwcc_vg = NULL; }
    kwcc_destroy_js(g_js_ctx);            /* JS 侧清理 + FreeContext */
    kwcc_ui_free();
    kwcc_mempool_shutdown();
    sg_shutdown();
    if (g_log_fp) { fflush(g_log_fp); fclose(g_log_fp); g_log_fp = NULL; }
}
```

**顺序原则**：
- force_all 在 destroy_js 之前：先回收 C 侧资源，再清 JS 侧
- mempool 倒数第二：其他模块可能依赖它
- **日志关闭移到末尾**：现行 cleanup 第一行就 fclose，导致后续所有清理（含 force_all 的 log_info）无法落盘——v2 修正

---

## 规范五：Cancel 语义（沿用 v1，不变）

Cancel 是运行时操作，与服务无关（运行时回收模块自治）。

### Cancel = 逻辑放弃

| 模型 | cancel 含义 | 对外部资源的操作 |
|------|-----------|--------------|
| 直接终止 | "我命令你停下" | `kill(SIGTERM)` 主动杀 |
| 逻辑放弃 | "我不再关心你的结果" | 关掉 pipe，外部程序自然退出 |

**采用模型 2**。理由：
- cancel 的本意是"不要结果"，不是"杀进程"。外部程序退出是 cancel 的**后果**，不是 cancel 的**定义**
- 关 pipe 后 curl 写 pipe 失败 → SIGPIPE → 自然退出，比 SIGTERM 强杀更干净
- 超时机制已有保底：即使 curl 卡在连接阶段，超时后也会退出

### 三级关闭协议

```
Level 1: 关 pipe     → curl SIGPIPE 自然退出（通常 < 1ms）  ← cancel 只做这级
Level 2: SIGTERM    → 礼貌请求退出                           ← shutdown() 用
Level 3: SIGKILL    → 强制终止                               ← 不做（讨论 6）
```

### Cancel 后的资源回收

cancel 时**不调 cleanup，不等回收**。`in_use` 保持 1，pid 保留。

回收时机（模块自治）：
- **每帧**：`check_progress()` 对 cancelled slot 做 waitpid(WNOHANG) 尝试回收
- **slot 耗尽时**：`request()` on-demand 紧急回收
- **退出时**：force_all → shutdown() 全量回收

---

## 模块接入示例：HTTP

```c
/* kwcc_http.c */
static void kwcc_http_shutdown(void);   /* 实现见 implementation plan v2 */

static kwcc_lifecycle_shutdown_entry_t g_kwcc_http_shutdown_entry = {
    .name     = "http",
    .shutdown = kwcc_http_shutdown,
};

/* kwcc_http_init() 中注册 */
kwcc_lifecycle_shutdown_register(&g_kwcc_http_shutdown_entry);
```

---

## 服务自身依赖

**v2 零依赖**：不依赖 bus（v1 依赖 bus 订阅 frame/tick，runtime 路径移除后此依赖消失），不依赖 io/http/js。只依赖 llog 打日志。

v1 的"服务依赖 bus 且 bus 又是被管模块"循环依赖分析随 runtime 路径一并消失。

---

## 注册顺序

模块的 shutdown 回收各自独立的 C 侧资源（HTTP 回收子进程/pipe），互不依赖。注册顺序不强制约束，force_all 逆序调用只是惯例。
