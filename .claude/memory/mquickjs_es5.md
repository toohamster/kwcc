# mquickjs ES5 语法支持分析

> 基于 `deps/mquickjs/mquickjs.c` 源码分析 + `/tmp/mquickjs-main/tests/` 测试用例验证

## 支持的语法

### 变量与赋值
```js
var x = 1;           // 唯一创建新变量的方式
var a = 1, b = 2;    // 多变量声明
for (var i = 0; i < 3; i++) { }  // for 循环中声明
var j in {x:1, y:2}  // for..in 中声明
x = 3;               // 赋值（变量必须已存在，全局赋值需在 eval 中）
```

### 函数
```js
function foo() { }              // 函数声明
var f = function() { };         // 匿名函数表达式
var f = function myname() { };  // 命名函数表达式
f.call(obj, a, b);              // call — this 绑定
f.apply(obj, [a, b]);           // apply — this + 数组参数
f.bind(obj, a);                 // bind — 部分应用
f.length;                       // 参数数量
f.name;                         // 函数名
f.toString();                   // 源码字符串
arguments;                      // 函数内的 arguments 对象
arguments.length; arguments[0];
```

### 闭包
```js
// 支持深层嵌套闭包，自由变量被正确捕获
function outer() {
    var x = 10;
    function inner() {
        function deep() { return x; }  // x 被捕获
        return deep();
    }
    return inner;
}
```

### 控制流
```js
if (cond) { } else { }
while (cond) { break; continue; }
do { } while (cond);
for (init; cond; step) { break; continue; }
for (var k in obj) { }           // for..in 可用
for (k in obj) { }               // 已存在的变量也可用于 for..in
switch (x) { case 1: default: }
try { } catch (e) { } finally { }
try { } finally { }              // 无 catch 也可
throw err;
return val;
// 标签语句
x: { break x; }
L1: for(i=0; i<3; i++) { break L1; continue L1; }
```

### 对象字面量
```js
var o = { x: 1, y: 2 };                    // 基本属性
var o = { "set": 1, "get": 2 };            // 引号属性名
var o = { if: 1, for: 2 };                 // 关键字作为属性名 (无需引号)
var o = { x: 1, __proto__: { z: 3 } };     // __proto__ 设置原型
var o = { get x() { return v; } };         // getter
var o = { set x(v) { b = v; } };           // setter
var o = { f(v) { return v+1; } };          // 方法简写
var o = { "set": setFn, "get": getFn };    // 值作为属性
```

### 对象操作
```js
var o = new Object();                       // 构造函数
var o = Object.create(proto);               // 指定原型创建
Object.defineProperty(o, "x", {...});       // 定义属性 (支持 get/set)
Object.getPrototypeOf(o);                   // 获取原型
Object.setPrototypeOf(o, proto);            // 设置原型
Object.keys(o);                             // 获取键数组
o.hasOwnProperty("x");                      // 自有属性检查
o.toString();                               // 类型检测 "[object Type]"
delete o.x;                                 // 删除属性
"x" in o;                                   // 属性存在性
```

### 构造函数与 new
```js
function F(x) { this.x = x; }
var f = new F(1);
f instanceof F;             // instanceof 检查
```

### 数组
```js
var a = [];
var a = [1, 2, 3];
var a = new Array(10);      // 指定长度
var a = new Array(1, 2);    // 指定元素
a[0] = 1; a.length;
a.push(x); a.pop(); a.join(sep); a.toString();
a.reverse(); a.shift(); a.slice(a, b); a.splice(start, count, ...items);
a.concat(x); a.sort(); a.sort(fn); a.unshift(x);
a.indexOf(x); a.lastIndexOf(x);
a.forEach(fn); a.map(fn); a.filter(fn);
a.reduce(fn); a.reduceRight(fn);
a.every(fn); a.some(fn);
Array.isArray(a);
a.length = 0;               // 可写，截断数组
```

### 字符串
```js
s.charAt(n); s.charCodeAt(n); s.codePointAt(n);
s.slice(a, b); s.substring(a, b); s.concat(x);
s.indexOf(x); s.lastIndexOf(x);
s.match(re); s.replace(re, str); s.replace(re, fn); s.replaceAll(re, str);
s.search(re); s.split(sep); s.split(re);
s.toLowerCase(); s.toUpperCase();
s.trim(); s.trimStart(); s.trimEnd();
s.repeat(n);
String.fromCharCode(n); String.fromCodePoint(n);
s[0];           // 通过索引访问字符
s.length;
```

### 数字
```js
Number.parseInt(str); Number.parseFloat(str);
Number.isNaN(x); Number.isFinite(x);
Number.MAX_VALUE; Number.MIN_VALUE;
Number.MAX_SAFE_INTEGER; Number.MIN_SAFE_INTEGER;
Number.NEGATIVE_INFINITY; Number.POSITIVE_INFINITY;
Number.EPSILON;
n.toFixed(d); n.toExponential(d); n.toPrecision(d);
// 一元 + 转换: +"0b111" === 7,  +"0o123" === 83
```

### Math
```js
Math.min(a, b); Math.max(a, b);
Math.abs(x); Math.floor(x); Math.ceil(x); Math.round(x);
Math.sqrt(x); Math.sin(x); Math.cos(x); Math.tan(x);
Math.asin(x); Math.acos(x); Math.atan(x);
Math.atan2(y, x); Math.exp(x); Math.log(x);
Math.pow(x, y); Math.random();
Math.sign(x); Math.trunc(x); Math.log2(x); Math.log10(x);
Math.imul(a, b); Math.clz32(x); Math.fround(x);
Math.PI; Math.E; Math.LN2; Math.LN10; Math.LOG2E; Math.LOG10E;
Math.SQRT2; Math.SQRT1_2;
```

### JSON
```js
JSON.parse(str); JSON.stringify(obj);
// undefined 值在 stringify 时被跳过
```

### RegExp
```js
var re = /abc/gi;                    // 正则字面量
var re = /abc/gim;                   // g/i/m/u 标志
var re = /\u0061/;                   // 转义序列
var re = /\x61/;                     // 十六进制转义
var re = /\u{20ac}/u;                // unicode 码点 (u 标志)
re.exec(str); re.test(str);
re.source; re.flags; re.lastIndex;
"abc".match(re); "abc".search(re);
"abc".replace(re, str); "abc".replace(re, fn);
"abc".split(re);
// 支持: 捕获组, 反向引用, 零宽断言 (?=), (?!), 字符类 [\q{a\b}]
```

### TypedArray
```js
new Uint8Array(4); new Int8Array(3); new Int32Array(3);
new Uint16Array(3); new Uint32Array(3);
new Float32Array(3); new Float64Array(3);
new Uint8ClampedArray(4);
new ArrayBuffer(16);
new Uint32Array(buffer, offset, length);  // 基于 buffer 的视图
a.buffer; a.byteLength; a.byteOffset; a.BYTES_PER_ELEMENT;
a.set(arr, offset); a.subarray(start, end);
a.join(); a.toString();
```

### eval
```js
eval("1 + 1");              // 直接 eval（在当前作用域执行）
(1,eval)("var z = 2");     // 间接 eval（在全局作用域执行，可创建全局变量）
eval("if (1) 2; else 3;"); // 返回最后表达式的值
```

### "use strict"
```js
"use strict";   // 支持严格模式指令
```

### 其他运算符
- 比较: `<`, `>`, `<=`, `>=`, `==`, `!=`, `===`, `!==`
- 算术: `+`, `-`, `*`, `/`, `%`, `**` (幂运算)
- 赋值: `+=`, `-=`, `*=`, `/=`, `%=`, `**=`, `<<=`, `>>=`, `>>>=`, `&=`, `^=`, `|=`
- 递增/递减: `++`, `--` (前缀和后缀)
- 逻辑: `&&`, `||`, `!`
- 位运算: `&`, `|`, `^`, `~`, `<<`, `>>`, `>>>`
- 三元: `cond ? a : b`
- 逗号: `(a, b)` — 返回最后一个表达式
- void: `void 0` — 返回 undefined
- 其他: `typeof`, `instanceof`, `delete`, `new`, `in`

### 数值字面量
```js
123; 0x1A; 0b111; 0o123;    // 十进制/十六进制/二进制/八进制
1.5; 1.5e10;                 // 浮点/科学计数法
-0; Infinity; NaN;           // 特殊值
```

### 字符串转义
```js
"\n\r\t\\\"\'";       // 常规转义
"\x61";               // 十六进制
"\u0061";             // unicode 4 位
"\u{10ffff}";         // unicode 任意码点
```

## 不支持的语法

### 确认不支持
| 语法 | 原因 |
|------|------|
| `let` / `const` | 仅 tokenized，未实现 |
| 箭头函数 `=>` | 无 `TOK_ARROW` token |
| 模板字符串 `` `...` `` | 无 backquote token |
| 展开运算符 `...` | 未实现 |
| `class` / `extends` | 仅 tokenized |
| `import` / `export` | 仅 tokenized |
| `for..of` 循环 | 抛 "unsupported type" |
| 对象简写 `{a}` | 不支持 shorthand 属性 |
| 计算属性名 `{[k]: v}` | 不支持 |
| 默认参数 | 不支持 |
| 剩余参数 `...args` | 不支持 |
| `async/await` | 不支持 |
| `Promise`, `Map`, `Set`, `Symbol` | 无内置类 |
| `WeakMap`, `WeakSet`, `Proxy`, `Reflect` | 无内置类 |
| `Proxy` | 无内置类 |
| `globalThis` | 等同于 `null` (globalThis 定义为 null) |

## 全局函数与对象
```js
// 全局函数
print(str);         // 打印到 stdout
console.log(str);   // 同上（内置 console 对象）
gc();               // 触发垃圾回收
load(filename);     // 加载并执行文件
kwcc_ui(method, ...args);  // 通过 CONFIG_KWCC 注册的 C 桥接函数。⚠️ mquickjs 不支持 ... 展开，只能传固定参数。在 bridge_create_js() 中通过 JS_NewObject + JS_SetPropertyStr 创建 ui 对象并绑定方法

// 内置构造函数
Object, Function, Number, Boolean, String, Array
Math, Date, JSON, RegExp
Error, EvalError, RangeError, ReferenceError
SyntaxError, TypeError, URIError, InternalError
ArrayBuffer, Uint8ClampedArray, Int8Array, Uint8Array
Int16Array, Uint16Array, Int32Array, Uint32Array
Float32Array, Float64Array

// 全局常量
Infinity, NaN, undefined

// 全局函数
parseInt(str), parseFloat(str), eval(str)
isNaN(x), isFinite(x)
```

## 关键陷阱

### `{}` 歧义
在 mquickjs 中，`{}` 在**语句开头**会被解析为 **block statement** 而非对象字面量：
```js
// 错误 — 被解析为空 block + 无效表达式
{ key: value };     // -> SyntaxError: expecting ';'

// 正确 — 确保在表达式上下文中
var o = { key: value };    // OK
(o = { key: value });      // OK, 括号强制表达式
var o = new Object();      // OK
```

### 变量必须用 var 声明
```js
// 错误 — 全局变量未声明就赋值
ui = {};    // -> ReferenceError 或 SyntaxError

// 正确
var ui = {};           // OK
var ui = new Object(); // OK
```

### kwcc_ui 不支持展开参数
```js
// 错误 — mquickjs 不支持 ...rest
kwcc_ui("button", ...args);

// 正确 — 每个参数单独传
kwcc_ui("button", text);
```

### console 已内置
mquickjs 已经内置 `console` 对象（`console.log` = `print`），不需要再定义：
```js
// 不需要这样做
if (typeof console == "undefined") { var console = { log: print }; }
```

### eval 中创建全局变量
```js
// 直接 eval — 变量在当前作用域创建
eval("var x = 1;");  // x 在当前函数作用域

// 间接 eval — 变量在全局作用域创建
(1,eval)("var x = 1;");  // x 成为全局变量
```
