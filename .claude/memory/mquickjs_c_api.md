# mquickjs C API 完整参考

> 基于 `deps/mquickjs/mquickjs.h` 源码（382 行），2026-06-02 验证
> **每次涉及 mquickjs C API 调用时，先查阅此文件，不要重新读 mquickjs.h**

## JSValue 类型系统

```c
// JSValue 是 uint64_t（64位平台）/ uint32_t（32位平台）
// 是值类型（tagged integer），不是结构体，不需要引用计数
typedef uint64_t JSWord;
typedef uint64_t JSValue;

// 常量
JS_NULL        // 宏常量
JS_UNDEFINED   // 宏常量
JS_FALSE / JS_TRUE
JS_EXCEPTION   // 异常值

// 类型判断（内联函数/宏）
JS_IsBool(v)        // 宏，检查 tag
JS_IsNull(v)        // 宏，v == JS_NULL
JS_IsUndefined(v)   // 宏，v == JS_UNDEFINED
JS_IsUninitialized(v)
JS_IsException(v)
JS_IsInt(v)         // 31-bit int tag 检查
JS_IsPtr(v)         // pointer tag 检查

// 类型判断（函数）
JS_BOOL JS_IsNumber(JSContext *ctx, JSValue val);
JS_BOOL JS_IsString(JSContext *ctx, JSValue val);
JS_BOOL JS_IsBool(JSValue v);              // 内联
JS_BOOL JS_IsNull(JSValue v);              // 内联
JS_BOOL JS_IsUndefined(JSValue v);         // 内联
JS_BOOL JS_IsError(JSContext *ctx, JSValue val);
JS_BOOL JS_IsFunction(JSContext *ctx, JSValue val);
```

## 创建 JSValue

```c
// 对象
JSValue JS_NewObject(JSContext *ctx);
JSValue JS_NewObjectClassUser(JSContext *ctx, int class_id);

// 数组
JSValue JS_NewArray(JSContext *ctx, int initial_len);

// 字符串
JSValue JS_NewString(JSContext *ctx, const char *buf);
JSValue JS_NewStringLen(JSContext *ctx, const char *buf, size_t buf_len);

// 数字
JSValue JS_NewInt32(JSContext *ctx, int32_t val);
JSValue JS_NewUint32(JSContext *ctx, uint32_t val);
JSValue JS_NewInt64(JSContext *ctx, int64_t val);
JSValue JS_NewFloat64(JSContext *ctx, double d);

// Bool
JSValue JS_NewBool(int val);  // 内联宏
```

## 属性访问

```c
// 获取/设置字符串属性
JSValue JS_GetPropertyStr(JSContext *ctx, JSValue this_obj, const char *str);
JSValue JS_SetPropertyStr(JSContext *ctx, JSValue this_obj,
                          const char *str, JSValue val);
// 注意：JS_SetPropertyStr 返回 JSValue（表示成功/失败），val 被 transfer 到对象上

// 获取/设置数组元素
JSValue JS_GetPropertyUint32(JSContext *ctx, JSValue obj, uint32_t idx);
JSValue JS_SetPropertyUint32(JSContext *ctx, JSValue this_obj,
                             uint32_t idx, JSValue val);

// 获取全局对象
JSValue JS_GetGlobalObject(JSContext *ctx);
```

## C 字符串转换

```c
typedef struct { uint8_t buf[5]; } JSCStringBuf;

// JSValue → C 字符串
const char *JS_ToCString(JSContext *ctx, JSValue val, JSCStringBuf *buf);
const char *JS_ToCStringLen(JSContext *ctx, size_t *plen, JSValue val, JSCStringBuf *buf);
// ⚠️ 可能返回 NULL！必须 NULL 检查
// ⚠️ 返回的指针可能指向 buf（短字符串内联）或 JS 内部内存
// ⚠️ 不要长期保存返回的指针

// C 字符串 → JSValue
JSValue JS_NewString(JSContext *ctx, const char *buf);
JSValue JS_NewStringLen(JSContext *ctx, const char *buf, size_t buf_len);
```

## 数字转换

```c
int JS_ToInt32(JSContext *ctx, int *pres, JSValue val);
int JS_ToUint32(JSContext *ctx, uint32_t *pres, JSValue val);
int JS_ToInt32Sat(JSContext *ctx, int *pres, JSValue val);
int JS_ToNumber(JSContext *ctx, double *pres, JSValue val);
// ⚠️ 返回值是 int（成功/失败状态码），必须检查！
```

## 类与不透明指针

```c
int JS_GetClassID(JSContext *ctx, JSValue val);
// JS_CLASS_OBJECT = 0, JS_CLASS_ARRAY = 1（枚举值，从0开始）
// if (JS_GetClassID(ctx, val) == JS_CLASS_ARRAY) → 是数组
// ⚠️ Object 类 class_id = 0，不能用 > 0 判断是否为对象

void JS_SetOpaque(JSContext *ctx, JSValue val, void *opaque);
void *JS_GetOpaque(JSContext *ctx, JSValue val);
```

## GC 管理

```c
// 栈式保护（函数内临时 JSValue）
JSValue *JS_PushGCRef(JSContext *ctx, JSGCRef *ref);
JSValue JS_PopGCRef(JSContext *ctx, JSGCRef *ref);

// 便捷宏
#define JS_PUSH_VALUE(ctx, v) do { JS_PushGCRef(ctx, &v##_ref); v##_ref.val = v; } while (0)
#define JS_POP_VALUE(ctx, v) v = JS_PopGCRef(ctx, &v##_ref)

// 使用方式：
// JSGCRef resp_ref;
// JSValue resp = JS_NewObject(ctx);
// JS_PUSH_VALUE(ctx, resp);  // 保护 resp 不被 GC
// ... 操作 ...
// JS_POP_VALUE(ctx, resp);   // 释放保护

// 列表式保护（长期存储，如 config）
JSValue *JS_AddGCRef(JSContext *ctx, JSGCRef *ref);
void JS_DeleteGCRef(JSContext *ctx, JSGCRef *ref);

// ⚠️ mquickjs 没有 JS_FreeValue！
// ⚠️ mquickjs 没有 JS_Duplicate！
// ⚠️ JSValue 是值类型，直接赋值即可
```

## JS 函数调用

```c
// 调用 JS 函数（只有 2 个参数！不是标准 QuickJS 的 5 参数版本）
int JS_StackCheck(JSContext *ctx, uint32_t len);  // 必须先用
void JS_PushArg(JSContext *ctx, JSValue val);     // 压栈参数
JSValue JS_Call(JSContext *ctx, int call_flags);  // 执行调用

// 压栈顺序：arg[n-1]...arg[0], func, this_obj
// call_flags = 参数数量 | 可选标志（如 FRAME_CF_CTOR）
```

## 代码执行

```c
JSValue JS_Eval(JSContext *ctx, const char *input, size_t input_len,
                const char *filename, int eval_flags);
JSValue JS_Parse(JSContext *ctx, const char *input, size_t input_len,
                 const char *filename, int eval_flags);
JSValue JS_Run(JSContext *ctx, JSValue val);

// eval flags
#define JS_EVAL_RETVAL    (1 << 0)  // 返回最后表达式的值
#define JS_EVAL_REPL      (1 << 1)  // 隐式定义全局变量（REPL 行为）
#define JS_EVAL_STRIP_COL (1 << 2)  // 去除调试信息（省内存）
#define JS_EVAL_JSON      (1 << 3)  // 解析为 JSON
#define JS_EVAL_REGEXP    (1 << 4)  // 内部使用
```

## 异常处理

```c
JSValue JS_Throw(JSContext *ctx, JSValue obj);
JSValue JS_ThrowError(JSContext *ctx, JSObjectClassEnum error_num,
                      const char *fmt, ...);
JSValue JS_ThrowOutOfMemory(JSContext *ctx);

// 便捷宏
#define JS_ThrowTypeError(ctx, fmt, ...)
#define JS_ThrowReferenceError(ctx, fmt, ...)
#define JS_ThrowInternalError(ctx, fmt, ...)
#define JS_ThrowRangeError(ctx, fmt, ...)
#define JS_ThrowSyntaxError(ctx, fmt, ...)

JSValue JS_GetException(JSContext *ctx);
```

## C 函数定义

```c
// C 函数签名
typedef JSValue JSCFunction(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// 函数定义结构
typedef struct JSCFunctionDef {
    JSCFunctionType func;
    JSValue name;
    uint8_t def_type;    // JS_CFUNC_generic, JS_CFUNC_generic_magic, etc.
    uint8_t arg_count;
    int16_t magic;
} JSCFunctionDef;

// 注册到 js_global_object[] 的宏
#define JS_CFUNC_DEF("name", argc, func_ptr)
// 使用位置：deps/mquickjs/mqjs_stdlib.c 的 js_global_object[] 数组中
// 需要 #ifdef CONFIG_KWCC 保护
```

## 上下文管理

```c
JSContext *JS_NewContext(void *mem_start, size_t mem_size, const JSSTDLibraryDef *stdlib_def);
void JS_FreeContext(JSContext *ctx);
void JS_SetContextOpaque(JSContext *ctx, void *opaque);
void JS_SetInterruptHandler(JSContext *ctx, JSInterruptHandler *interrupt_handler);
void JS_GC(JSContext *ctx);  // 触发垃圾回收
```

## 不存在的关键 API（不要用！）

| 不存在 | 替代方案 |
|--------|----------|
| `JS_FreeValue(ctx, v)` | 不需要（JSValue 是值类型） |
| `JS_Duplicate(ctx, v)` | 不需要（JSValue 是值类型，直接赋值） |
| `JS_Call(ctx, func, this, argc, argv)` | 用 `JS_PushArg` + `JS_Call(ctx, flags)` |
| `JS_GetArrayLength(ctx, &len, arr)` | 用 `JS_GetPropertyStr(ctx, arr, "length")` + `JS_ToInt32` |
| `JS_GetPropertyUint32_array_len` | 同上 |
| `JS_IsArray(ctx, arr)` | 用 `JS_GetClassID(ctx, arr) == JS_CLASS_ARRAY` |

## C→JS Dispatch 安全模式

```c
// ✅ 安全方式：C API 构建对象 + 全局变量传递
JSValue resp = JS_NewObject(ctx);
JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, 200));
JS_SetPropertyStr(ctx, resp, "body", JS_NewStringLen(ctx, body, body_len));

JSValue global_obj = JS_GetGlobalObject(ctx);
JS_SetPropertyStr(ctx, global_obj, "__http_resp_xxx", resp);
JS_Eval(ctx, "$bus.emit('http/end', new Object(), __http_resp_xxx); delete global.__http_resp_xxx;", ...);

// ❌ 危险方式：字符串拼接用户数据
snprintf(buf, "...", body);  // body 含引号/换行会导致 SyntaxError
```

## Config 存储 GC 保护

```c
// 长期存储 JSValue 时必须用 JS_AddGCRef
static JSValue g_configs[CONFIG_MAX];
static JSGCRef g_config_refs[CONFIG_MAX];

void kwcc_config_set(const char *module, JSValue val) {
    int i = find_or_alloc_slot(module);
    if (i < CONFIG_MAX) {
        g_configs[i] = val;
        JS_AddGCRef(ctx, &g_config_refs[i]);  // 保护不被 GC
        g_config_refs[i].val = val;
    }
}
```

## delete global.X 安全规则

```javascript
// ✅ req_id 只包含 [a-zA-Z0-9_]，是合法 JS 标识符
delete global.__http_resp_req_abc123;

// ✅ 安全替代（如果 req_id 可能含特殊字符）
delete global['__http_resp_req_abc123'];

// ❌ 如果 req_id 含 `-`、`.`、空格等，`delete global.__http_resp_a-b` 会 SyntaxError
```
