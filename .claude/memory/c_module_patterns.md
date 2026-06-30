# C 语言模块开发模式

> 两种 C 模块组织方式，适用场景不同，项目内按需选用。

---

## 模式 A：模块前缀 + 静态全局

```c
/* kwcc_bus.h */
void kwcc_bus_publish(const char *topic, const void *data, size_t len);
void kwcc_bus_subscribe(const char *topic, kwcc_bus_cb cb);

/* kwcc_bus.c */
static struct { /* ... */ } g_kwcc_bus_groups[KWCC_BUS_GROUP_MAX];

void kwcc_bus_publish(const char *topic, const void *data, size_t len) {
    /* 直接操作 g_kwcc_bus_groups */
}
```

**特征**：
- 数据：`static` 全局变量，模块内部私有
- 操作：`kwcc_模块_动词()` 命名，直接调用
- 实例：全局唯一（单例）
- 内聚：文件级（同 .c 文件 + 同前缀，靠约定）

**优势**：
- 零间接调用开销（函数直接调用，无函数指针跳转）
- 零堆分配（静态全局，无 malloc/free）
- 代码直观，读函数名就知道做什么
- 不需要"创建/销毁"生命周期管理

**劣势**：
- 只能有全局一个实例
- 运行时不能替换实现
- 数据和操作的关联靠命名约定，编译器不强制

**适用**：
- 全局唯一的模块（bus、config、mempool、io）
- 只有一种实现、不需要替换
- 性能敏感路径（函数指针间接调用有代价）

---

## 模式 B：struct + 函数指针（laid 模式）

```c
/* kwcc_js.h */
typedef struct kwcc_js_ops kwcc_js_ops_t;
struct kwcc_js_ops {
    void           *ctx;          /* 不透明数据 */
    kwcc_js_val_t  (*new_object)(kwcc_js_ops_t *ops);
    void           (*set_str_prop)(kwcc_js_ops_t *ops, kwcc_js_val_t obj,
                                   const char *key, kwcc_js_val_t val);
    /* ... */
};

/* kwcc_js.c — 绑定实现 */
void kwcc_js_ops_init(JSContext *ctx) {
    g_kwcc_js_ops.new_object   = js_new_object_impl;
    g_kwcc_js_ops.set_str_prop = js_set_str_prop_impl;
    /* ... */
}
```

**特征**：
- 数据：struct 成员字段
- 操作：struct 内函数指针，调用时通过 `obj->method(obj, ...)` 间接调用
- 实例：可创建多个（堆分配或静态全局）
- 内聚：类型级（数据和操作绑定在同一个 struct，编译器强制）

**优势**：
- **更高内聚**：数据和操作在同一个类型上，不靠约定
- **可替换性**：换实现只改 `xxx_init` 绑定，调用方不动
- **多实例**：同一类型可创建多个对象（请求槽、连接、定时器）
- **跨模块传递**：对象作为参数传递，接收方不需要知道实现细节
- **接口即规范**：struct 定义就是 API 契约，实现和调用方解耦

**劣势**：
- 函数指针间接调用有微小开销（一次额外跳转）
- 堆分配需要 malloc/free（也可用静态全局避免）
- 绑定代码：`create_xxx` / `xxx_init` 需要逐个赋值函数指针
- 调用语法稍长：`ops->new_object(ops)` vs `kwcc_js_new_object()`

**适用**：
- 需要隔离底层实现（`kwcc_js_ops_t` 隔离 mquickjs）
- 需要多实例（HTTP 请求槽、WS 连接、Timer）
- 需要跨模块传递"对象"（`kwcc_js_module_t` 让 core 统一管理模块）
- 需要运行时替换实现（测试时 mock、换引擎）

---

## 选择决策

```
需要多实例？ ── 是 ──→ 模式 B
               │
               否
               │
需要隔离/可替换？ ── 是 ──→ 模式 B
               │
               否
               │
需要跨模块传递？ ── 是 ──→ 模式 B
               │
               否
               │
           模式 A（简单、零开销）
```

**核心原则**：模式 B 的代价（间接调用 + 绑定代码）必须换来实际收益（隔离/多实例/可替换）。如果只有一个实现、只有一个实例，模式 A 更简单。

---

## 项目中的实际应用

| 模块 | 模式 | 原因 |
|------|------|------|
| `kwcc_js_ops_t` | B | 隔离 mquickjs，子模块通过 ops 操作 JS |
| `kwcc_js_module_t` | B | 跨模块传递，core 统一管理模块生命周期 |
| `kwcc_bus` | A | 全局唯一，无需隔离 |
| `kwcc_http` | A | 全局唯一，请求槽用数组而非对象 |
| `kwcc_config` | A | 全局唯一，无需替换 |
| `kwcc_mempool` | A | 全局唯一，性能敏感 |
| `kwcc_io` | A | 全局唯一，性能敏感 |

---

## 模式 B 的两种实例化方式

```c
/* 方式 1：静态全局（我们项目用这种） */
static kwcc_js_ops_t g_kwcc_js_ops;       /* 零分配，生命周期 = 进程 */
void kwcc_js_ops_init(JSContext *ctx) {   /* 初始化时绑定函数指针 */
    g_kwcc_js_ops.new_object = js_new_object_impl;
}

/* 方式 2：堆分配（laid 原文用这种） */
struct laid *create_laid(char *name) {
    struct laid *l = malloc(sizeof(struct laid));  /* 堆分配，调用方负责 free */
    l->say_hello = say_hello;
    return l;
}
```

**选择原则**：
- 生命周期 = 进程 → 静态全局（零分配，无泄漏风险）
- 生命周期 = 动态 → 堆分配（多实例，用完 free）
- 我们项目中 `kwcc_js_ops_t` 和 `kwcc_js_module_t` 都是进程级生命周期，用静态全局

---

## 内聚性对比总结

| 维度 | 模式 A | 模式 B |
|------|--------|--------|
| 内聚级别 | 文件级（约定） | 类型级（编译器强制） |
| 数据-操作关联 | 同文件 + 同前缀 | 同 struct |
| 编译器检查 | 无（约定可违反） | 有（struct 成员必须存在） |
| 简单程度 | 更简单 | 稍复杂 |
| 灵活程度 | 低（单例、固定实现） | 高（多实例、可替换） |

**结论**：模式 B 内聚性确实更高，但内聚高不等于更好——要看场景是否需要模式 B 提供的额外能力（隔离/多实例/可替换）。不需要时，模式 A 的简单性是更大优势。
