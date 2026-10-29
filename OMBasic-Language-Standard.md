# OMBasic 0.1 语言标准

**语言名称：** OMBasic  
**全称：** Oh My BASIC  
**规范版本：** 0.1  
**状态：** 0.1 Language Freeze Candidate
**日期：** 2026-10-27

---

## 1. 范围与目标
**OM-01.00 — 范围与目标**


OMBasic 是一门面向 AI 辅助编程时代设计的、强调**人类可读、人类可审查、局部可分析**的静态语言。

OMBasic 的首要目标不是最短源码，也不是最大元编程能力，而是使阅读者在尽量有限的上下文中能够判断：

- 一段代码会执行什么；
- 哪些值可能变化；
- 哪些操作具有副作用；
- 哪些操作可能跨 Actor 或跨节点；
- 哪些数据可被发送；
- 哪些状态由谁拥有；
- 一次控制流何时开始、何时结束；
- 与 C/C++ 等 Native 世界交互时，安全边界位于何处。

OMBasic 采用 BASIC 风格的可读关键字、Pascal/Modula 风格的显式块结构、静态类型推导、默认不可变绑定、Actor 并发模型和显式 Native 边界。

---

## 2. 规范术语
**OM-02.00 — 规范术语**


本文中的以下词具有规范含义：

- **必须（MUST）**：实现必须满足。
- **不得（MUST NOT）**：实现禁止该行为。
- **应该（SHOULD）**：除非有充分理由，否则应满足。
- **不应该（SHOULD NOT）**：除非有充分理由，否则应避免。
- **可以（MAY）**：实现可选择支持。

示例和实现建议在未明确标为“必须”时不构成强制 ABI 或实现约束。

规范性条款使用稳定 Rule ID，格式为 `OM-SS.RR`：`SS` 对应标准顶层章节，
`RR` 是该章节内稳定编号。信息性示例和实现建议不定义 Rule ID。Rule ID
在 OMBasic 0.1 release candidate 后不得重编号；语义变化必须更新标准和
引用该规则的 conformance cases。

---

## 3. 核心设计原则

### 3.1 局部可分析性
**OM-03.01 — 局部可分析性**


一条语句的基本语义应该主要由以下信息确定：

1. 语句本身；
2. 当前词法作用域；
3. 编译期可获得的符号和类型信息。

语言不得依赖任意远处的运行时代码来重新定义核心语法语义。

### 3.2 语义不可重定义
**OM-03.02 — 语义不可重定义**


以下核心语义由编译器定义，用户代码不得重载或改写：

```text
.
+
-
*
/
=
<>
<
>
<=
>=
IF
FOR
WHILE
RETURN
TRY
SEND
RECEIVE
IMPORT
```

OMBasic 0.1 不支持用户自定义运算符重载。

### 3.3 控制流必须可见
**OM-03.03 — 控制流必须可见**


OMBasic 不提供隐藏的挂起/恢复模型。

OMBasic 0.1 **没有**：

```text
Async
Await
Future
Promise
Coroutine
隐式 continuation
```

需要等待外部结果时，当前 Actor 的 `RECEIVE` 处理结束，结果以后以新的 Message 返回。

### 3.4 变化必须显式
**OM-03.04 — 变化必须显式**


绑定默认不可变。

需要重新赋值的绑定必须使用 `VAR` 声明。

长期可变状态应该由 Actor 拥有。

### 3.5 类型是编译器事实
**OM-03.05 — 类型是编译器事实**


普通 OMBasic 源码不要求、也不鼓励书写类型标注。

编译器必须推导静态类型。编辑器应该通过 inlay hint、hover、补全或生成接口显示推导类型。

Native ABI 描述属于例外：ABI 类型必须明确，见第 24 章。

### 3.6 无共享可变状态
**OM-03.06 — 无共享可变状态**


不同 Actor 不得直接共享普通可变对象。

Actor 之间只能通过 Message 通信。

---

## 4. 源文件

### 4.1 编码
**OM-04.01 — 编码**


源文件必须使用 UTF-8。

### 4.2 文件扩展名
**OM-04.02 — 文件扩展名**


OMBasic 源文件的标准扩展名为：

```text
.bas
```

所有符合 OMBasic 0.1 的工具链必须识别 `.bas` 作为 OMBasic 源文件扩展名。OMBasic 0.1 不定义第二种标准源码扩展名。

### 4.3 语句结束
**OM-04.03 — 语句结束**


换行通常结束一条语句。

OMBasic 不要求分号作为语句结束符。

### 4.4 缩进
**OM-04.04 — 缩进**


缩进不影响语法。

工具链应该提供统一 formatter。

### 4.5 注释
**OM-04.05 — 注释**


OMBasic 0.1 提供两种注释：**普通注释**与**文档注释**。注释不得改变程序运行语义。

#### 4.5.1 普通注释

单引号 `'` 在字符串字面量之外出现时，从该位置开始直到当前行结束的内容均为普通注释。

```basic
' 计算订单总价
total_price = subtotal + tax

amount = 100  ' 金额以最小货币单位保存
```

普通注释可以独占一行，也可以出现在完整语句之后。

字符串中的单引号不是注释起始符：

```basic
message = "It's ready"
```

工具链在词法分析阶段必须忽略普通注释内容，但 formatter 应该保留注释及其相对位置。

#### 4.5.2 文档注释

三个连续单引号 `'''` 在一行的第一个非空白位置出现时，表示文档注释。

```basic
''' 计算订单中所有项目的总金额。
''' 不读取或修改外部状态。
EXPORT FUNCTION calculate_total(items)
    ...
END FUNCTION
```

连续的文档注释行组成一个文档块。文档块与紧随其后的声明关联；文档块与声明之间不得出现其他语句或空白行。

文档注释可以用于说明：

- `RECORD`；
- `MESSAGE`；
- `FUNCTION`；
- `ACTION`；
- `ACTOR`；
- 其他未来版本明确允许的声明。

文档注释不参与类型推导、控制流、优化或运行时行为。编译器、LSP 和文档生成器可以读取并展示文档注释。

`'''` 只有在一行的第一个非空白位置出现时才具有文档注释含义。语句后的单引号始终按普通注释处理。

#### 4.5.3 不提供块注释

OMBasic 0.1 不定义块注释。以下形式不是 OMBasic 注释语法：

```text
/* ... */
(* ... *)
{ ... }
```

这样可以避免大段代码被一个远处的结束符静默屏蔽，也不需要定义嵌套块注释规则。

需要暂时停用代码时，应删除代码、使用版本控制，或显式修改控制结构；不得依赖“注释掉一大片代码”作为语言机制。

#### 4.5.4 注释不得成为隐藏控制通道

实现不得赋予普通注释或文档注释隐藏的执行语义。特别是，OMBasic 0.1 不支持：

- 条件编译注释；
- 可执行注释；
- 通过注释修改类型检查；
- 通过注释修改优化或调度；
- 通过注释关闭错误检查。

例如下面的文字只是注释内容，不得改变编译结果：

```basic
' TODO: later
' compiler: ignore_error
' disable_type_check
```

`TODO:`、`NOTE:`、`FIXME:` 等前缀可以被编辑器作为文本约定高亮，但它们不是语言关键字，也不得具有编译器语义。

#### 4.5.5 注释写作约定

注释应该优先解释**原因、约束、外部事实或不明显的业务规则**，而不是重复代码已经明确表达的行为。

推荐：

```basic
' 银行接口要求金额使用整数分，避免浮点舍入差异
amount_in_cents = amount * 100
```

不推荐：

```basic
' 把 count 加一
count = count + 1
```

对于公开声明，应该优先使用文档注释描述调用者需要知道的契约，而不是描述内部实现步骤。

---

## 5. 标识符、大小写与命名规范

### 5.1 基本标识符字符集
**OM-05.01 — 基本标识符字符集**


OMBasic 0.1 的用户标识符只允许使用 ASCII 字母、数字和下划线。

字符串和注释可以使用完整 Unicode。

用户标识符不得：

- 以数字开头；
- 以 `_` 开头；
- 以 `_` 结尾；
- 包含连续两个 `_`；
- 使用仅靠大小写与同一作用域中另一名称区分的拼写。

例如：

```text
user_name       合法
payment2        合法
Payment_Service 合法
_user           非法
user_           非法
user__name      非法
```

### 5.2 大小写敏感
**OM-05.02 — 大小写敏感**


OMBasic 是大小写敏感语言。

关键字必须使用本标准规定的**全大写规范拼写**。例如只有：

```basic
IF
FUNCTION
ACTOR
SEND
```

是关键字写法。以下写法不得被当作对应关键字接受：

```text
If
if
Function
function
```

编译器应该提供友好诊断，例如遇到 `If` 时提示是否意图写成关键字 `IF`，但不得静默改写源码语义。

用户定义名称同样区分大小写；但为了降低视觉混淆，同一作用域不得声明仅大小写不同的两个用户名称。

### 5.3 一个概念一种规范拼写
**OM-05.03 — 一个概念一种规范拼写**


OMBasic 的命名规范是语言标准的一部分。

所有多单词用户名称都使用 `_` 显式分隔单词。大小写只用来表达名称类别，不承担单词分隔职责。

#### 普通名称：`lower_snake_case`

以下名称必须使用 `lower_snake_case`：

- 局部变量；
- 参数；
- 不可变绑定；
- `VAR` 绑定；
- `FUNCTION` 名；
- `ACTION` 名；
- 导入后的模块限定名；
- `.bas` 文件名；
- 源码目录名。

例如：

```text
user
user_name
payment_result
calculate_total
save_order
payment_service.bas
storage/database.bas
```

标准库中以普通函数或动作形式暴露的可调用名称也遵循 `lower_snake_case`，例如 `print`、`read_file`、`calculate_hash`。

#### 实体名称：`Pascal_Snake_Case`

以下名称必须使用 `Pascal_Snake_Case`：

- `RECORD` 名；
- `MESSAGE` 名；
- `ACTOR` 名。

例如：

```text
User
Order_Item
Payment_Failed
Payment_Service
Database_Worker
```

实体名称中的每个单词首字母大写，其余字母小写。缩写也按普通单词处理：

```text
Http_Server
Json_Parser
Url_Request
Sql_Database
```

不使用：

```text
HTTP_Server
JSON_Parser
URL_Request
```

### 5.4 Native 名称边界
**OM-05.04 — Native 名称边界**


普通 OMBasic 名称必须遵循上述规则。Native interface 中直接对应外部 C ABI 的符号可以保留外部库的原始 C 符号名，以保证 ABI 对照明确；OMBasic 自己定义的 Native wrapper 名仍应遵循 `lower_snake_case` 或 `Pascal_Snake_Case`。

`CInt`、`CDouble`、`CPointer` 等 ABI 内建类型名属于标准定义名称，不受用户实体命名规则约束。

### 5.5 文件与目录命名
**OM-05.05 — 文件与目录命名**


OMBasic 源文件名必须使用 `lower_snake_case.bas`，目录名必须使用 `lower_snake_case`。

例如：

```text
src/
├── main.bas
├── payment_service.bas
└── storage/
    ├── database.bas
    └── memory_cache.bas
```

文件名同时决定导入后的默认模块限定名，因此：

```basic
IMPORT "payment_service.bas"

result = payment_service.start()
```

不存在从文件名到另一套 PascalCase、camelCase 或 namespace 名称的隐藏转换。

### 5.6 禁止魔法名称
**OM-05.06 — 禁止魔法名称**


用户定义名称本身不得触发隐藏语言行为。

OMBasic 0.1 不定义类似以下特殊名称约定：

```text
__init__
__call__
__getattr__
```

名称只是名称。语言行为必须由明确语法、声明或标准规定决定。

### 5.7 保留关键字
**OM-05.07 — 保留关键字**


OMBasic 0.1 保留以下全大写关键字：

```text
ACTION
ACTOR
AND
AT
BREAK
CASE
CONTINUE
ELSE
END
EXPORT
FALSE
FOR
FROM
FUNCTION
IF
IMPORT
IN
MESSAGE
NATIVE
NOT
NULL
OR
RECORD
RECEIVE
RETURN
SELECT
SELF
SEND
SPAWN
STOP
STEP
THEN
TO
TRUE
TRY
VAR
WATCH
WHILE
WITH
```

---

## 6. 基本字面量

### 6.1 整数
**OM-06.01 — 整数**


```basic
0
10
-42
1000000
```

默认整数语义为有符号 64 位整数。

### 6.2 浮点数
**OM-06.02 — 浮点数**


```basic
0.0
3.14159
-12.5
```

默认浮点语义为 IEEE 754 binary64。

### 6.3 Bool
**OM-06.03 — Bool**


```basic
TRUE
FALSE
```

### 6.4 String
**OM-06.04 — String**


```basic
name = "Alice"
```

OMBasic String 必须具有确定的 Unicode 语义。0.1 推荐内部、模块边界和网络传输中的文本编码统一使用 UTF-8。

### 6.5 字符串插值
**OM-06.05 — 字符串插值**


```basic
greeting = $"Hello {name}"
```

插值表达式必须具有编译器已知的格式化规则。

用户不得通过运行时 hook 重定义任意对象的默认字符串转换。

### 6.6 Null
**OM-06.06 — Null**


```basic
value = NULL
```

OMBasic 只定义一个空值概念 `NULL`。

是否允许某个值为 Null 必须可被静态类型系统确定。

---

## 7. 绑定与可变性

### 7.1 默认不可变绑定
**OM-07.01 — 默认不可变绑定**


```basic
x = 10
name = "Alice"
```

上述绑定建立后不得重新赋值：

```basic
x = 10
x = 20     ' 编译错误
```

### 7.2 显式可变绑定
**OM-07.02 — 显式可变绑定**


需要变化时必须使用 `VAR`：

```basic
VAR count = 0
count = count + 1
```

`VAR` 是代码审查中的显式状态变化标记。

### 7.3 赋值不是表达式
**OM-07.03 — 赋值不是表达式**


赋值语句不产生值。

以下写法非法：

```basic
IF x = read_value() THEN
```

若 `=` 位于条件表达式中，它表示相等比较，而不是赋值。

需要赋值时必须拆开：

```basic
x = read_value()

IF x = expected THEN
    ...
END IF
```

### 7.4 Shadowing
**OM-07.04 — Shadowing**


OMBasic 0.1 默认不允许在同一词法路径上静默 shadow 已存在的本地名字。

工具链应该把可能造成误读的 shadowing 作为编译错误或强诊断。

---

## 8. 静态类型系统

### 8.1 普通源码无显式类型标注
**OM-08.01 — 普通源码无显式类型标注**


典型源码：

```basic
name = "Alice"
age = 20
```

编辑器可显示：

```text
name : String
age  : Int
```

类型提示不是源码的一部分。

### 8.2 基础推导类型
**OM-08.02 — 基础推导类型**


OMBasic 0.1 至少包含以下语义类型：

```text
Int
Float
Bool
String
Null
List<T>
Map<K, V>
Record
Message
ActorRef<P>
Result<T, E>
Function<...>
```

### 8.3 不提供普通 `Any`
**OM-08.03 — 不提供普通 `Any`**


OMBasic 0.1 不提供可在业务源码中任意使用的 `Any` 类型。

编译器内部可以存在 `Unknown` 类型变量，但它表示“尚未完成推导”，不是运行时万能值。

### 8.4 不做隐式类型转换
**OM-08.04 — 不做隐式类型转换**


以下代码必须报错：

```basic
x = "10" + 20
```

必须显式转换：

```basic
x = int("10") + 20
```

### 8.5 不提供 truthiness
**OM-08.05 — 不提供 truthiness**


`IF`、`WHILE` 条件必须是 Bool。

以下代码非法：

```basic
IF count THEN
```

应写：

```basic
IF count > 0 THEN
```

### 8.6 绑定类型稳定
**OM-08.06 — 绑定类型稳定**


显式 `VAR` 允许重新赋值，但不得改变其已推导类型：

```basic
VAR x = 10
x = 20       ' 合法
x = "hello"  ' 编译错误
```

### 8.7 泛型推导
**OM-08.07 — 泛型推导**


编译器可以推导泛型函数，而无需用户在源码中声明类型参数：

```basic
FUNCTION identity(value)
    RETURN value
END FUNCTION
```

其编译器接口可以等价表示为：

```text
identity<T>(T) -> T
```

### 8.8 导出接口必须稳定
**OM-08.08 — 导出接口必须稳定**


任何 `EXPORT` 符号在模块公共接口处必须拥有完整、稳定、可写入接口 metadata 的静态类型。

无法形成稳定公共接口的导出必须编译失败。

---

## 9. 运算符

### 9.1 算术运算
**OM-09.01 — 算术运算**


```text
+  -  *  /  %
```

用户不得重载这些运算符。

### 9.2 比较
**OM-09.02 — 比较**


```text
=  <>  <  >  <=  >=
```

### 9.3 布尔运算
**OM-09.03 — 布尔运算**


```text
NOT
AND
OR
```

布尔运算只接受 Bool。

`AND` 与 `OR` 按从左到右的短路规则求值：左侧结果已经决定整个表达式时，跳过右侧表达式。

### 9.4 优先级
**OM-09.04 — 优先级**


从高到低：

```text
1. () 与字段/调用
2. 一元 - 与 NOT
3. * / %
4. + -
5. = <> < > <= >=
6. AND
7. OR
```

复杂表达式应该优先使用括号，而不是依赖读者记忆优先级。

---

## 10. 控制流

### 10.1 If
**OM-10.01 — If**


```basic
IF score >= 60 THEN
    result = "Pass"
ELSE
    result = "Fail"
END IF
```

可以使用：

```basic
IF x > 10 THEN
    ...
ELSE IF x > 5 THEN
    ...
ELSE
    ...
END IF
```

### 10.2 While
**OM-10.02 — While**


```basic
WHILE count < 10
    count = count + 1
END WHILE
```

条件必须是 Bool。

### 10.3 数值 For
**OM-10.03 — 数值 For**


```basic
FOR i = 1 TO 10
    print(i)
END FOR
```

步长：

```basic
FOR i = 10 TO 0 STEP -1
    print(i)
END FOR
```

### 10.4 集合 For
**OM-10.04 — 集合 For**


```basic
FOR item IN items
    process(item)
END FOR
```

### 10.5 Break / Continue
**OM-10.05 — Break / Continue**


`BREAK` 和 `CONTINUE` 只影响当前最内层循环。

### 10.6 Select
**OM-10.06 — Select**


```basic
SELECT command
CASE "start"
    start()
CASE "stop"
    stop()
CASE ELSE
    unknown()
END SELECT
```

`SELECT` 不允许用户定义动态 pattern hook。

---

## 11. Record

### 11.1 声明
**OM-11.01 — 声明**


```basic
RECORD User(name, age)
```

Record 表示纯数据值。

### 11.2 构造
**OM-11.02 — 构造**


```basic
user = User("Alice", 20)
```

### 11.3 字段访问
**OM-11.03 — 字段访问**


```basic
print(user.name)
```

`user.name` 必须表示静态已知字段读取。

不存在 property、descriptor、`__getattr__` 或字段访问 hook。

### 11.4 Record 不可变
**OM-11.04 — Record 不可变**


以下代码非法：

```basic
user.age = 21
```

更新必须产生新值：

```basic
older = user WITH age = 21
```

### 11.5 无类
**OM-11.05 — 无类**


OMBasic 0.1 没有：

```text
Class
Inheritance
Virtual method
Override
Protected
Constructor hierarchy
```

数据由 Record 表示；行为由 Function/Action 表示；可变长期状态由 Actor 表示。

---

## 12. List 与 Map

### 12.1 List
**OM-12.01 — List**


```basic
numbers = [1, 2, 3]
```

列表元素必须具有统一可推导类型。

OMBasic 0.1 不自动把不兼容元素提升为 `Any`。

### 12.2 Map
**OM-12.02 — Map**


```basic
user = {
    "name": "Alice",
    "city": "Tokyo"
}
```

Map 的 key/value 类型必须静态确定。

### 12.3 默认不可变
**OM-12.03 — 默认不可变**


普通 List/Map 值默认不可变。

允许实现提供局部可变容器，但必须通过显式可变构造或 `VAR` 能力暴露，且不得跨 Actor 共享普通可变容器。

---

## 13. Function 与 Action

### 13.1 Function
**OM-13.01 — Function**


`FUNCTION` 表示纯计算。

```basic
FUNCTION add(a, b)
    RETURN a + b
END FUNCTION
```

Function 不得：

- 修改外部可变状态；
- 直接访问 Actor state；
- 执行文件、网络、时钟等外部副作用；
- Spawn Actor；
- Send Message；
- 调用不安全 Native effect。

纯函数在相同输入下必须具有相同语言级结果。

### 13.2 Action
**OM-13.02 — Action**


`ACTION` 表示显式副作用过程：

```basic
ACTION main()
    worker = SPAWN Worker
    SEND worker, Start()
END ACTION
```

Action 可以执行语言允许的显式副作用，但**不得隐式挂起并恢复**。

### 13.3 Return
**OM-13.03 — Return**


```basic
RETURN value
```

无返回值 Action 可以写：

```basic
RETURN
```

### 13.4 调用
**OM-13.04 — 调用**


```basic
result = add(10, 20)
```

函数调用语法不能被用户重定义。

### 13.5 不支持函数重载
**OM-13.05 — 不支持函数重载**


OMBasic 0.1 不以参数类型或参数数量进行同名函数重载。

同一模块作用域中一个名字应对应一个可调用符号。

---

## 14. 错误模型

### 14.1 不使用异常作为普通控制流
**OM-14.01 — 不使用异常作为普通控制流**


OMBasic 0.1 不定义用户级 `throw/catch` 异常机制。

普通可恢复错误使用 `Result<T, E>`。

### 14.2 Result
**OM-14.02 — Result**

`Result<T, E>` 是内建 tagged union。`Ok(value)` 与 `Error(error)` 的类型参数必须能从调用上下文推导；无法推导时编译器必须诊断。

概念上：

```text
Ok(value)
Error(error)
```

### 14.3 Try
**OM-14.03 — Try**


`TRY` 表示显式错误传播：

```basic
user = TRY find_user(id)
```

其语义为：

- 若结果为 `Ok(value)`，提取 `value`；
- 若结果为 `Error(error)`，立即从当前 Function/Action/Receive 返回兼容错误。

因为 `TRY` 出现在调用点，提前返回行为对阅读者可见。

### 14.4 显式处理
**OM-14.04 — 显式处理**


实现可以提供 `SELECT` 或标准库辅助函数对 Result 做显式分支，但不得通过不可见异常跳转绕过源码控制流。

---

## 15. Message

### 15.1 声明
**OM-15.01 — 声明**


```basic
MESSAGE Deposit(amount)
MESSAGE Withdraw(amount, reply)
MESSAGE Reset()
```

Message 是不可变数据值。

### 15.2 Nominal identity
**OM-15.02 — Nominal identity**


Message 使用名义身份。

即使两个 Message 字段结构相同：

```basic
MESSAGE Deposit(amount)
MESSAGE Withdraw(amount)
```

它们仍是不同 Message 类型。

### 15.3 可序列化性
**OM-15.03 — 可序列化性**


可跨节点发送的 Message 必须具有完整静态类型，且所有字段都必须是可序列化值。

普通 Native pointer、线程句柄、本地文件描述符等不得直接成为分布式 Message 字段。

---

## 16. Actor

### 16.1 Actor 定义
**OM-16.01 — Actor 定义**


Actor 是由 OMBasic runtime 调度的轻量级逻辑进程，不等同于 OS process，也不等同于 OS thread。

Actor 至少拥有：

```text
identity
mailbox
local state
execution context
local heap
```

### 16.2 声明
**OM-16.02 — 声明**


```basic
ACTOR Counter
    VAR count = 0

    RECEIVE Increment()
        count = count + 1
    END RECEIVE
END ACTOR
```

### 16.3 Actor state
**OM-16.03 — Actor state**


Actor 的长期可变状态必须由该 Actor 独占。

其他 Actor 不得直接取得指向该状态的普通可变引用。

### 16.4 Receive run-to-completion
**OM-16.04 — Receive run-to-completion**


每个 `RECEIVE` handler 必须 **run-to-completion**。

```basic
RECEIVE Add(amount)
    balance = balance + amount
    log(balance)
END RECEIVE
```

从进入 `RECEIVE` 到 `END RECEIVE` / `RETURN` 期间：

- 当前 Actor 不处理另一条 Message；
- handler 不会被 `AWAIT` 或 coroutine 隐式挂起；
- 同一 Actor 的其他 handler 不会重入当前状态。

这是 OMBasic 的核心局部分析保证。

### 16.5 Spawn
**OM-16.05 — Spawn**


```basic
worker = SPAWN Worker
```

远程 placement 可以写成实现支持的显式形式，例如：

```basic
worker = SPAWN Worker AT node
```

编译器应推导：

```text
worker : ActorRef<Worker>
```

### 16.6 Send
**OM-16.06 — Send**


```basic
SEND worker, Work(item)
```

`SEND` 是异步消息发送操作。

`SEND` 不等待接收方处理完成，也不隐式返回远程函数结果。

### 16.7 协议检查
**OM-16.07 — 协议检查**


编译器必须检查 Actor 是否接受对应 Message。

例如 `ActorRef<Counter>` 不接受 `Login(...)` 时应编译失败。

### 16.8 ActorRef
**OM-16.08 — ActorRef**


Actor 引用是可传递身份，不是普通对象指针。

概念上可表示：

```text
ActorRef = NodeId + ActorId + Protocol
```

具体 wire representation 由 runtime 定义。

---

## 17. 等待、I/O 与系统 Actor

### 17.1 OMBasic 没有 Await
**OM-17.01 — OMBasic 没有 Await**


OMBasic 0.1 禁止：

```basic
result = AWAIT operation()
```

也不存在等价的隐式语法。

### 17.2 等待结果的标准模式
**OM-17.02 — 等待结果的标准模式**


需要外部结果时发送 Message，并结束当前 handler：

```basic
RECEIVE Load_User(id)
    SEND Database, Find_User(id, SELF)
END RECEIVE
```

结果以后以新的 Message 到达：

```basic
RECEIVE User_Found(user)
    process(user)
END RECEIVE
```

### 17.3 系统 I/O 也使用 Actor
**OM-17.03 — 系统 I/O 也使用 Actor**


文件、网络、Timer、数据库等异步或阻塞资源应该通过系统 Actor 暴露：

```basic
SEND Console, Console_Write("hello", SELF)
SEND File_System, File_Read(path, SELF)
SEND File_System, File_Write(path, contents, SELF)
SEND Timer, Timer_After(1000, Tick(), SELF)
```

Console、File_System 操作通过 completion Message 异步返回：

```basic
RECEIVE File_Read_Completed(outcome)
    contents = TRY outcome
END RECEIVE
```

`Console_Write_Completed` 和 `File_Write_Completed` 携带
`Result<Unit, String>`；`File_Read_Completed` 携带
`Result<String, String>`。Console 写入传入的完整文本，不追加换行；文件
API 按 UTF-8 文本读写，`File_Write` 会覆盖目标文件。Timer 到期时向
recipient 投递传入的 Message，本身不返回应答 Message。底层 I/O 在独立
blocking worker pool 执行，Timer 等待由单独的定时队列管理；所有完成事件
都通过 Actor mailbox 传递。

### 17.4 不允许隐藏 continuation
**OM-17.04 — 不允许隐藏 continuation**


编译器和 runtime 不得把普通顺序源码静默转换为可重入 Actor continuation，而又不在语言语义中暴露该事实。

---

## 18. Actor 隔离与消息传输

### 18.1 禁止共享普通可变内存
**OM-18.01 — 禁止共享普通可变内存**


两个 Actor 不能直接共享一个普通 mutable Record/List/Map/Object。

### 18.2 Message 传输
**OM-18.02 — Message 传输**


OMBasic 0.1 的语义模型是按值传输。

同节点 runtime 可以优化 immutable payload，但优化不得改变可观察语义。

### 18.3 Actor-local heap
**OM-18.03 — Actor-local heap**


推荐每个 Actor 拥有自己的 runtime heap。

实现可以使用：

```text
Actor stack
Receive temporary arena
Actor persistent heap
```

其中临时数据若未逃逸，可在 handler 结束时整体释放。

### 18.4 GC
**OM-18.04 — GC**


OMBasic 语言规范不强制具体 GC 算法。

推荐 0.x runtime 使用 Actor-local tracing GC（如 mark-and-sweep），并允许 stack allocation、arena allocation 和 escape analysis 减少 GC 压力。

---

## 19. 调度模型

### 19.1 Actor 不是线程
**OM-19.01 — Actor 不是线程**


实现不得要求“一 Actor 一 OS thread”。

推荐：

```text
大量 Actor
    ↓
Runtime Scheduler
    ↓
少量 Worker Threads
```

### 19.2 单 Actor 串行执行
**OM-19.02 — 单 Actor 串行执行**


同一 Actor 在任意时刻最多由一个 worker 执行。

不同 Actor 可以并行执行。

### 19.3 公平性
**OM-19.03 — 公平性**


实现应该防止单个 Actor 无限占用 worker thread。

字节码 VM 可以使用 reduction/instruction budget 进行协作式抢占，但这种 runtime 抢占不得造成同一 Actor handler 的并发重入。

---

## 20. 分布式语义

### 20.1 Node
**OM-20.01 — Node**


一个 OMBasic Node 通常对应一个 runtime OS process。

Node 内可承载大量 Actor。

### 20.2 Cluster
**OM-20.02 — Cluster**


多个 Node 可组成 Cluster。

### 20.3 本地与远程 ActorRef
**OM-20.03 — 本地与远程 ActorRef**


普通业务代码使用同一种 `ActorRef` 和 `SEND` 语法。

runtime 根据 ActorRef 判断：

```text
local  -> enqueue mailbox
remote -> serialize -> transport -> remote mailbox
```

位置透明不等于失败透明。

### 20.4 Delivery
**OM-20.04 — Delivery**


OMBasic 0.1 默认分布式 Message delivery 语义为：

```text
at-most-once
```

标准不承诺 exactly-once。

### 20.5 Ordering
**OM-20.05 — Ordering**


同一 sender 到同一 receiver 的 Message 应保证发送顺序。

不同 sender 之间不保证全局顺序。

### 20.6 网络失败可见
**OM-20.06 — 网络失败可见**


Node、Actor 或网络可能失败。

远程失败不得被伪装成永远成功的本地调用。

### 20.7 Watch
**OM-20.07 — Watch**


OMBasic 可以提供：

```basic
WATCH worker
```

被观察 Actor 终止后，观察者接收明确的系统 Message，例如 `Actor_Down(...)`。

---

## 21. Module 与路径导入

### 21.1 文件即 Module
**OM-21.01 — 文件即 Module**


OMBasic 中，一个 `.bas` 源文件天然就是一个 Module。

源码中不得再声明额外的 `Module ... End Module` 名称，也不存在独立于文件系统的 namespace 映射。

例如源码根目录为：

```text
src/
├── main.bas
├── payment.bas
└── storage/
    ├── database.bas
    └── cache.bas
```

其中存在四个 Module：

```text
main.bas
payment.bas
storage/database.bas
storage/cache.bas
```

**文件路径就是 Module 的唯一身份。**

源码位置和逻辑 Module 身份不得分离。

### 21.2 唯一源码根目录
**OM-21.02 — 唯一源码根目录**


每个编译目标必须具有一个明确的源码根目录（source root）。

规范示例使用：

```text
src/
```

源码根目录是唯一的 Module 身份空间和文件安全边界。Import literal 可以使用
Source-Root-relative 或显式的 Current-Module-relative 形式；两者解析出的文件
都必须位于该源码根目录内，并规范化为相对于该根目录的 canonical path。

普通路径不以 `./` 开头时相对于 Source Root 解析，无论当前文件位于何处：

```basic
IMPORT "storage/database.bas"
```

都只表示：

```text
<source-root>/storage/database.bas
```

OMBasic 0.1 不定义多级 import 搜索路径。

显式的 `./` 路径相对于当前 `.bas` 文件所在目录解析。例如，位于
`vendor/json/json.bas` 的 `IMPORT "./parser.bas"` 表示
`vendor/json/parser.bas`。两种形式最终都进入同一 canonical Module identity；
编译器不定义第二个 Source Root，也不搜索目录。

### 21.3 Import 直接使用文件路径
**OM-21.03 — Import 直接使用文件路径**


`IMPORT` 后必须写一个带引号的 `.bas` 文件路径。合法路径形式有两种：普通形式
相对于 Source Root 解析，例如 `IMPORT "vendor/json/json.bas"`；唯一前导 `./`
形式相对于当前 Module 所在目录解析，例如 `IMPORT "./parser.bas"`。两种路径都
必须位于同一 Source Root 内，并规范化为 Source-Root-relative canonical path。
`IMPORT` 只能出现在 Module 顶层，并且必须位于该文件其他声明或语句之前：

```basic
IMPORT "payment.bas"
IMPORT "storage/database.bas"
IMPORT "network/http.bas"
IMPORT "./parser.bas"
```

OMBasic 0.1 不使用：

```basic
IMPORT payment
IMPORT storage.database
```

也不允许省略 `.bas` 扩展名。

这样阅读者可以直接从源码中的 Import 定位到文件树中的目标文件，不需要学习第二套 Module 名称映射规则。

### 21.4 Import 路径规则
**OM-21.04 — Import 路径规则**


Import literal 必须满足：

1. 使用 Source-Root-relative 形式，或仅以一个 `./` 开头的
   Current-Module-relative 形式；
2. 必须以 `.bas` 结尾；
3. 必须使用 `/` 作为源码中的路径分隔符；
4. 不得以 `/` 开头；
5. 除唯一允许的前导 `./` 外，不得包含 `.` 路径段；任何位置的 `..`
   路径段都禁止；
6. 不得包含空路径段；
7. 编译器不得通过搜索目录、环境变量或隐式 fallback 猜测目标文件；
8. 每个目录名和文件名（不含 `.bas` 后缀）都必须符合普通名称 lower_snake_case 规则；
9. 一个规范 Import 路径必须唯一对应一个 `.bas` 文件，并且各路径段必须与文件系统中的实际大小写完全一致。

根入口路径也必须符合以上路径规则。编译器先规范化 source root，再解析每个相对路径。实现可以采用严格策略拒绝源码树中的所有符号链接；如果允许符号链接，解析后的目标仍必须位于规范化 source root 内。路径比较不得依赖当前工作目录或环境变量。

以下写法非法：

```basic
IMPORT "../payment.bas"
IMPORT "././payment.bas"
IMPORT "storage/../payment.bas"
IMPORT "/storage/database.bas"
IMPORT "storage/database"
```

### 21.5 Import 不是文本包含
**OM-21.05 — Import 不是文本包含**


虽然 `IMPORT` 使用类似传统 include 的文件路径，但其语义不是文本复制。

```basic
IMPORT "math.bas"
```

表示当前 Module 对 `math.bas` 建立静态依赖，并允许使用其公开符号。

同一个 `.bas` Module 在一次构建中只有一个身份。多个 Module Import 同一文件时，不得产生多份文本展开或多份独立的模块级定义。

### 21.6 导入后的限定名
**OM-21.06 — 导入后的限定名**


导入 Module 的默认限定名为文件名去掉 `.bas` 后的部分。

例如：

```basic
IMPORT "storage/database.bas"

connection = database.open()
```

其中限定名为：

```text
database
```

OMBasic 0.1 不支持 Import alias。

如果当前 Module 同时 Import 两个具有相同文件名的不同路径，例如：

```basic
IMPORT "sales/database.bas"
IMPORT "archive/database.bas"
```

则编译器必须报告名称冲突。0.1 要求作者通过更明确的文件命名或目录结构解决冲突，而不是为同一 Module 创建额外别名。

### 21.7 Import 无运行时副作用
**OM-21.07 — Import 无运行时副作用**


`IMPORT` 只建立静态依赖和符号可见性。

Import 不得因为加载 Module 而：

- 打开文件；
- 建立网络连接；
- Spawn Actor；
- 修改全局状态；
- 执行任意初始化代码。

### 21.8 Export
**OM-21.08 — Export**


默认符号是 Module-private。

公共 API 必须显式：

```basic
EXPORT FUNCTION add(a, b)
    RETURN a + b
END FUNCTION
```

Actor、Message、Record 也可以 Export。

### 21.9 Module 不拥有可变运行时状态
**OM-21.09 — Module 不拥有可变运行时状态**


Module 可以定义常量和纯声明，但不得持有长期 mutable global state。

长期可变状态应由 Actor 拥有。

### 21.10 构建、版本与分发不是源码级组织概念
**OM-21.10 — 构建、版本与分发不是源码级组织概念**


OMBasic 0.1 的源码级代码组织只有 Module。

版本、第三方依赖获取、发布产物、构建缓存等属于工具链职责，不引入 `Package`、`Project`、`Namespace`、`Crate`、`Assembly` 等源码级组织关键词。

工具链可以维护构建图和版本 metadata，但这些机制不得改变 `.bas` 文件路径与 Module 身份之间的一一对应关系。

### 21.11 Module 依赖图
**OM-21.11 — Module 依赖图**


OMBasic 0.1 的 Module Import graph 必须无循环。

例如以下依赖非法：

```text
a.bas -> b.bas -> c.bas -> a.bas
```

编译器必须在编译期报告循环 Import。

### 21.12 Module 接口 metadata
**OM-21.12 — Module 接口 metadata**


编译器应该为每个 Module 的公开 API 生成机器可读接口 metadata，其中至少可以包含：

- Export 符号；
- 推导类型；
- 泛型关系；
- Record schema；
- Message schema；
- Actor protocol；
- effect 信息；
- 可序列化信息。

编译器和 LSP 可以读取该 metadata 进行增量编译、补全和静态分析，但 metadata 不得改变源码中可见的 Import 语义。

### 21.13 Directory Facade 约定
**OM-21.13 — Directory Facade 约定**

目录不是 Module，也不因包含 `.bas` 文件而自动导出内容。源码库可以约定目录 `json/` 中的 `json/json.bas` 是其公共 Facade；调用方仍必须使用普通 `IMPORT` 显式导入该文件。编译器不得扫描目录、推断 Facade 或自动导出目录成员。

### 21.14 当前 Module 相对 Import
**OM-21.14 — 当前 Module 相对 Import**

`./` 开头的 Import 路径相对于包含该 Import 的 `.bas` 文件所在目录解析。解析结果仍须位于固定 Source Root 内，并遵守本节规定的路径大小写、扩展名和符号链接安全规则。其他 Import 路径仍相对于 Source Root 解析。两种形式都必须在进入 Module Graph 前转换为 Source-Root-relative canonical path；指向同一文件的路径必须使用同一个 Module identity。路径解析不得搜索目录或隐式回退。

### 21.15 显式 Re-export
**OM-21.15 — 显式 Re-export**

模块可在 Import 前缀中写 `EXPORT IMPORT "./parser.bas"`。该语句导入目标 Module，并把目标 Module 的公开声明按原符号名平铺加入当前 Module 的公开 API；它不导出目标 Module 的 Alias，不创建合成声明，也不改变原始符号身份和定义位置。只有目标 Module 自身公开的声明可被重导出。重导出可传递组合。Re-export Import 参与普通 Module 依赖图，所有循环仍须在编译期拒绝。

### 21.16 Re-export 可见性与冲突
**OM-21.16 — Re-export 可见性与冲突**

私有声明不得经由 Re-export 公开。相同原始符号通过多条路径到达时按一个符号处理；当前 Module 对同一名称公开不同原始符号时，编译器必须报告冲突并标注冲突来源。普通 `IMPORT` 仍只建立 Module Alias，不改变导入方的无限定名称查找。

### 21.17 Facade 公开面与 canonical identity
**OM-21.17 — Facade 公开面与 canonical identity**

Module 的公开面由其 `EXPORT` 声明和有效的 `EXPORT IMPORT` 共同确定。导入别名仍由目标文件名 stem 决定；Facade 不会改变 Module identity。公开接口 metadata 应反映 Facade 可见名称并保留原始声明位置，公开 API fingerprint 不应因私有实现内容变化而变化。

---

## 22. Effect 与工具可见性

编译器应该记录足够的 effect metadata，使 IDE 能向人类显示调用行为。

例如 hover 可以显示：

```text
process_order(order)

Kind: Action
Sends:
    Payment.Charge
    Inventory.Reserve
Mutates:
    current Actor state
Native:
    no
```

OMBasic 的目标不是让 effect 信息充满源码，而是让 effect 信息静态可获得并容易展示。

---

## 23. 明确缺失的语言特性
**OM-23.00 — 明确缺失的语言特性**


OMBasic 0.1 有意不提供：

```text
Class
Inheritance
Virtual dispatch
Operator overloading
Implicit conversion
Truthiness
User metaclass
Decorator rewriting
Dynamic attribute lookup
Monkey patching
General reflection
Eval
Runtime code generation
Implicit import side effects
Async/Await
Future/Promise
Coroutine
User-level exception unwinding
Shared mutable memory between Actors
```

这些缺失是设计约束，不应被视为待补齐的传统语言功能清单。

---

# 24. Native / C / C++ 互操作规范

## 24.1 总原则

OMBasic 官方 Native ABI **只以 C ABI 为基础**。

C++、Rust、Zig 或其他语言若能导出稳定 C ABI，可通过相同机制接入。

OMBasic 0.1 不直接承诺兼容任意 C++ ABI。

推荐结构：

```text
OMBasic
   ↓
C ABI
   ↓
C library
```

C++ 使用：

```text
OMBasic
   ↓
C ABI
   ↓
extern "C" wrapper
   ↓
C++ library
```

## 24.2 Native 是显式安全边界

进入 Native 世界必须在源码、接口文件或生成 metadata 中可见。

普通 OMBasic 语言保证不得被一个看似普通的 Function 静默绕过。

工具应该把 Native 调用标为显式 effect。

## 24.3 普通源码与 ABI 类型分离

普通 OMBasic 源码仍保持 annotation-free：

```basic
result = add(10, 20)
```

但 C ABI 不能靠推导猜测物理表示。

因此 Native interface 必须具有明确 ABI 类型。

推荐接口文件扩展名：

```text
.omi
```

例如：

```basic
NATIVE "mylib"

    FUNCTION add(a: CInt, b: CInt): CInt

    FUNCTION distance(
        x1: CDouble,
        y1: CDouble,
        x2: CDouble,
        y2: CDouble
    ): CDouble

END NATIVE
```

这里的类型标注属于 ABI 声明，不属于普通 OMBasic 业务语法。

## 24.4 从 C header 自动生成接口

工具链应该支持从 C header 生成 Native metadata，例如：

```text
ombasic bind mylib.h
```

对于：

```c
int add(int a, int b);
```

工具可以生成等价 `.omi`：

```basic
NATIVE "mylib"
    FUNCTION add(a: CInt, b: CInt): CInt
END NATIVE
```

自动生成不能消除人工确认 ownership、callback threading 和 blocking 行为的必要性。

## 24.5 C ABI 类型

Native interface 应提供明确的 C ABI 类型族，例如：

```text
CInt8
CInt16
CInt32
CInt64
CUInt8
CUInt16
CUInt32
CUInt64
CInt
CUInt
CLong
CULong
CLongLong
CULongLong
CFloat
CDouble
CSize
CBool
CPointer
CString
```

实现不得假设 C `long` 在所有平台具有固定宽度。

固定宽度类型和平台 ABI 类型必须在 metadata 中可区分。

## 24.6 String 与 CString

普通 OMBasic `String` 不是 C `char *` 的同义词。

OMBasic String 具有语言级长度和 Unicode 语义；C `char *` 通常依赖 NUL 终止和外部 ownership。

Native interface 使用 `CString` 时，编译器/runtime 可以执行明确、受规范约束的临时转换。

这种 ABI conversion 仅存在于明确 Native 边界，不构成普通 OMBasic 的隐式类型转换机制。

## 24.7 普通 Record 不保证 C layout

普通：

```basic
RECORD Point(x, y)
```

编译器可以对其做 scalar replacement、寄存器化、字段优化等，不承诺与某个 C struct 布局一致。

需要与 C struct 互操作时，必须使用 Native Record 或生成 ABI metadata，例如：

```basic
NATIVE RECORD C_Point
    x: CDouble
    y: CDouble
END NATIVE RECORD
```

Native Record 的 size、alignment、field offset 必须遵守目标平台 C ABI。

## 24.8 Pointer

`CPointer` 属于 Native 能力，不属于普通 OMBasic value model。

普通业务代码不得把任意 C pointer 当作可安全共享、可序列化或可跨 Actor 传输的普通对象。

Native pointer：

- 默认不得成为 Message payload；
- 不得跨 Node；
- 不得被 GC 当作普通 OMBasic object graph 自动追踪；
- 其 ownership 必须由 Native metadata 或 wrapper 明确。

## 24.9 Ownership 必须明确

Native 返回 pointer/buffer 时，接口必须能够说明其生命周期。

至少应区分：

```text
Borrowed
Owned
Static
```

Owned 值必须指定释放方式，概念示例：

```basic
FUNCTION create_name(): CString
    Returns Owned
    Free WITH free_name
END FUNCTION
```

Borrowed 值必须有可验证的 owner/lifetime 约束。

OMBasic 不得猜测“应该调用 free 还是其他释放函数”。

## 24.10 Native Handle

C/C++ 的可变对象应该作为 opaque handle 暴露，而不是伪装成 OMBasic Class。

C++：

```cpp
class Database;
```

推荐 wrapper：

```cpp
extern "C" {
    DatabaseHandle *db_create();
    void db_destroy(DatabaseHandle *db);
    int db_query(DatabaseHandle *db, const char *sql, QueryResult *out);
}
```

OMBasic 侧把 `DatabaseHandle *` 视为 NativeHandle / opaque pointer。

长期 Native mutable handle 最好由一个 Actor 独占。

推荐模型：

```text
Database Actor
    ↓ owns
Native DatabaseHandle
```

这样 Native 可变状态仍具有明确 owner。

## 24.11 C++ 直接 ABI 不属于 0.1

OMBasic 0.1 不直接映射：

```text
C++ class
virtual table
std::string
std::vector
STL iterator
template
RTTI
C++ exception
name mangling
```

这些接口必须通过 C-compatible wrapper 或外部生成层转换。

## 24.12 C++ Exception 不得跨边界

C++ exception 不得穿过 `extern "C"`/OMBasic ABI 边界。

C++ wrapper 必须捕获异常并转换为明确错误码、Result-compatible error 或 callback/message error。

OMBasic 侧继续使用显式错误模型。

## 24.13 Callback

C callback 可以通过 Native interface 声明。

但如果 callback 可能从任意 OS thread 进入 runtime，则外部线程不得直接执行 Actor handler 或直接修改 Actor state。

正确边界：

```text
C/C++ callback thread
        ↓
runtime trampoline
        ↓
thread-safe enqueue
        ↓
Actor mailbox
        ↓
Receive handler
```

因此 Native callback 进入 Actor 世界时应该转换成 Message。

## 24.14 Blocking Native Call

普通 Actor scheduler worker 不应该被不可控的 blocking Native call 长时间占用。

阻塞操作必须被接口标记为 blocking，或者由系统 Actor 包装到专门 blocking worker pool。

推荐：

```text
Actor
  ↓ SEND
File_System/System Actor
  ↓
blocking worker pool
  ↓
C API
  ↓
completion message
  ↓
Actor mailbox
```

这仍然不需要 `AWAIT`。

## 24.15 Native 与 Actor isolation

Native 代码不得成为绕过 Actor isolation 的隐式通道。

如果 Native library 内部共享状态，wrapper 必须明确其同步、ownership 与线程规则。

推荐把具有可变状态的 Native resource 封装在专属 Actor 内。

## 24.16 Native 与分布式

普通 Native pointer、handle、callback token 均是 node-local resource，默认不得跨节点序列化。

如果需要远程访问 Native resource，应发送 ActorRef，由拥有该 resource 的 Actor 执行操作。

## 24.17 ABI 版本与平台

Native interface metadata 应记录至少：

```text
target architecture
OS ABI
calling convention
library identity/version
struct layout information
ownership annotations
blocking/effect annotations
```

编译器必须在明显不兼容的目标上拒绝链接，而不是静默猜测布局。

## 24.18 Native 互操作硬规则

OMBasic 0.1 规定：

1. 官方 Native ABI 以 C ABI 为唯一基础。
2. C++ 通过 `extern "C"` 或等价 C wrapper 接入。
3. ABI 类型只存在于 Native interface/metadata，不污染普通业务源码。
4. 普通 Record 不保证 C struct layout。
5. Native pointer/handle 不属于普通可序列化值。
6. Native ownership 必须明确。
7. C++ exception 不得跨 ABI。
8. 外部线程不得直接执行 Actor handler。
9. Blocking Native call 不得无标记地阻塞 Actor scheduler。
10. Native mutable resource 应具有明确 owner，优先由单一 Actor 独占。

---

## 25. 标准库原则
**OM-25.00 — 标准库原则**


标准库应该优先提供小而直接的 API。

标准库不得依赖：

- 动态方法注入；
- 运行时 monkey patch；
- 隐式 import 初始化；
- operator hook；
- 隐式线程创建。

外部 I/O 应优先通过系统 Actor 暴露。

---

## 26. 程序入口
**OM-26.00 — 程序入口**


可执行程序应定义唯一入口：

```basic
ACTION main()
    print("Hello, OMBasic")
END ACTION
```

`main` 不得来自隐式执行的 Module 顶层代码。

---

## 27. 示例：普通程序
本节为非规范性说明。示例、草图或实现建议不创建额外语言规则；规范性行为以带 Rule ID 的条款为准。


```basic
FUNCTION add(a, b)
    RETURN a + b
END FUNCTION

ACTION main()
    result = add(10, 20)
    print(result)
END ACTION
```

编辑器可以显示：

```text
add(a : Int, b : Int) : Int
result : Int
```

但这些类型不进入 `.bas` 源码。

---

## 28. 示例：不可变 Record
本节为非规范性说明。示例、草图或实现建议不创建额外语言规则；规范性行为以带 Rule ID 的条款为准。


```basic
RECORD User(name, age)

FUNCTION birthday(user)
    RETURN user WITH age = user.age + 1
END FUNCTION
```

`birthday` 不修改原 `user`。

---

## 29. 示例：显式状态
本节为非规范性说明。示例、草图或实现建议不创建额外语言规则；规范性行为以带 Rule ID 的条款为准。


```basic
ACTION count_items(items)
    VAR count = 0

    FOR item IN items
        count = count + 1
    END FOR

    print(count)
END ACTION
```

阅读者只需寻找 `VAR` 即可快速定位局部 mutation。

---

## 30. 示例：Actor 异步工作流
本节为非规范性说明。示例、草图或实现建议不创建额外语言规则；规范性行为以带 Rule ID 的条款为准。


```basic
MESSAGE Start(id)
MESSAGE Find_User(id, reply)
MESSAGE User_Found(user)
MESSAGE Done(user)

ACTOR User_Service

    RECEIVE Start(id)
        SEND Database, Find_User(id, SELF)
    END RECEIVE

    RECEIVE User_Found(user)
        SEND Client, Done(user)
    END RECEIVE

END ACTOR
```

这里没有挂起的 call stack。

控制流由 Message 边界明确分段。

---

## 31. 示例：Native C 封装
本节为非规范性说明。示例、草图或实现建议不创建额外语言规则；规范性行为以带 Rule ID 的条款为准。


C header：

```c
int checksum(const unsigned char *data, size_t size);
```

Native interface 概念表示：

```basic
NATIVE "checksum"
    FUNCTION checksum(data: CPointer, size: CSize): CInt
END NATIVE
```

业务源码：

```basic
result = checksum(buffer, length)
```

IDE 应提示该调用属于 Native 边界。

---

## 32. 最小语法草图
本节为非规范性说明。示例、草图或实现建议不创建额外语言规则；规范性行为以带 Rule ID 的条款为准。


以下为非完整 EBNF，仅描述 0.1 主要结构。普通注释与文档注释在词法阶段按第 4.5 节处理，不进入下列语法产生式：

```text
program        = { import_decl } { declaration | statement } ;

import_decl     = "IMPORT" string_literal ;

declaration    = record_decl
               | message_decl
               | function_decl
               | action_decl
               | actor_decl ;

record_decl     = [ "EXPORT" ] "RECORD" entity_identifier "(" [ names ] ")" ;
message_decl    = [ "EXPORT" ] "MESSAGE" entity_identifier "(" [ names ] ")" ;

function_decl   = [ "EXPORT" ] "FUNCTION" ordinary_identifier "(" [ names ] ")"
                  block
                  "END" "FUNCTION" ;

action_decl     = [ "EXPORT" ] "ACTION" ordinary_identifier "(" [ names ] ")"
                  block
                  "END" "ACTION" ;

actor_decl      = [ "EXPORT" ] "ACTOR" entity_identifier
                  { actor_state | receive_decl }
                  "END" "ACTOR" ;

actor_state     = "VAR" ordinary_identifier "=" expression ;

receive_decl    = "RECEIVE" entity_identifier "(" [ names ] ")"
                  block
                  "END" "RECEIVE" ;

statement       = binding
               | assignment
               | if_stmt
               | while_stmt
               | for_stmt
               | select_stmt
               | return_stmt
               | send_stmt
               | spawn_stmt
               | try_stmt
               | call_stmt ;

binding         = ordinary_identifier "=" expression
               | "VAR" ordinary_identifier "=" expression ;

if_stmt         = "IF" expression "THEN" block
                  { "ELSE" "IF" expression "THEN" block }
                  [ "ELSE" block ]
                  "END" "IF" ;

while_stmt      = "WHILE" expression block "END" "WHILE" ;

for_stmt        = "FOR" ordinary_identifier "=" expression "TO" expression
                  [ "STEP" expression ] block "END" "FOR"
               | "FOR" ordinary_identifier "IN" expression block "END" "FOR" ;

send_stmt       = "SEND" expression "," expression ;

return_stmt     = "RETURN" [ expression ] ;

ordinary_identifier = lower_snake_case ;
entity_identifier   = Pascal_Snake_Case ;
names               = ordinary_identifier { "," ordinary_identifier } ;
```

Native `.omi` 使用独立 ABI grammar，不属于普通 `.bas` grammar。

---

## 33. 推荐实现架构
本节为非规范性说明。示例、草图或实现建议不创建额外语言规则；规范性行为以带 Rule ID 的条款为准。


OMBasic 0.1 的参考实现建议使用 C。

推荐编译管线：

```text
Source
  ↓
Lexer
  ↓
Parser
  ↓
AST
  ↓
Type inference
  ↓
Typed AST
  ↓
Bytecode compiler
  ↓
Bytecode VM
```

Actor runtime：

```text
Node
├── Scheduler
├── Worker Threads
├── Network Transport
├── Blocking Native Pool
│
├── Actor
│   ├── Mailbox
│   ├── VM State
│   ├── Temporary Arena
│   └── Persistent Heap
│
└── Actor ...
```

### 33.1 第一阶段内存策略

实现初期可以使用：

```text
AST              -> Arena
短命非逃逸对象   -> Stack / handler arena
长期 Actor 对象  -> Actor heap
```

0.1 原型允许 runtime 对 heap object 采用 program/Actor-lifetime allocation，再逐步加入 tracing GC，但语义必须为将来独立 GC 保留清晰 ownership 边界。

### 33.2 Receive 作为生命周期边界

因为没有 Await，Receive handler 不存在跨消息 continuation。

因此实现可以在 handler 结束后安全回收未逃逸的 temporary arena。

---

## 34. OMBasic 0.1 一致性要求
**OM-34.00 — OMBasic 0.1 一致性要求**


声称符合 OMBasic 0.1 的实现至少必须满足：

1. 普通源码支持 annotation-free static type inference。
2. 默认绑定不可变，重新赋值必须显式 `VAR`。
3. Record 和 Message 默认不可变。
4. 不支持 Class/Inheritance。
5. 不支持用户运算符重载。
6. 不支持普通隐式类型转换和 truthiness。
7. Function 具有纯函数语义。
8. 可恢复错误不依赖用户级异常 unwinding。
9. Actor 是隔离的 mutable-state owner。
10. Actor 之间不共享普通可变内存。
11. Receive run-to-completion。
12. 不存在 Async/Await/Future/Promise/Coroutine 语言语义。
13. 外部等待通过 Send/Receive 分段表达。
14. `.bas` 文件即 Module，文件路径即 Module 的唯一身份。
15. Import literal 使用 Source-Root-relative 形式，或仅带一个前导 `./` 的
    Current-Module-relative 形式。两者都必须规范化到唯一 Source Root 下的
    canonical `.bas` 路径；不得搜索、使用其他 `.`/`..` 路径段或省略扩展名。
16. Import 是静态依赖而非文本包含，且无运行时副作用。
17. Module Import graph 必须无循环。
18. Module 不拥有长期可变全局状态。
19. 分布式默认不承诺 exactly-once。
20. Native 官方 ABI 以 C ABI 为基础。
21. C++ 必须通过 C ABI wrapper 或等价明确桥接层。
22. Native pointer/handle 不得伪装成普通可序列化值。
23. 外部线程 callback 不得直接重入 Actor handler。
24. 语言关键字必须大小写敏感并使用本标准规定的全大写拼写。
25. 用户普通名称必须使用 `lower_snake_case`，`RECORD` / `MESSAGE` / `ACTOR` 名必须使用 `Pascal_Snake_Case`。
26. `.bas` 文件名和源码目录名必须使用 `lower_snake_case`。

---

## 35. 延后到后续版本的问题
**OM-35.00 — 延后到后续版本的问题**


OMBasic 0.1 不试图一次解决以下问题：

- 完整 persistent collection 优化；
- 分代 GC；
- shared immutable heap；
- exactly-once/effectively-once 高层协议；
- 自动 supervision tree DSL；
- schema evolution 的完整兼容规则；
- ABI 稳定插件系统；
- 直接 C++ ABI；
- GPU/SIMD 专用类型；
- 高级 effect type system；
- 宏与编译期元编程。

任何后续特性在加入前，都应首先回答：

> 它是否扩大了理解这一行代码所需的上下文范围？

如果答案是“是”，则应优先寻找更显式、更局部的替代设计。

---

## 36. 设计格言
本节为非规范性说明。示例、草图或实现建议不创建额外语言规则；规范性行为以带 Rule ID 的条款为准。


OMBasic 的核心不是“写得最少”，而是：

> **Code should be obvious.**

中文可表述为：

> **代码应该让行为显而易见。**

或者更完整地说：

> **值默认稳定，变化必须可见；状态必须有主人，控制流必须能看见；类型由编译器理解，代码由人理解。**

---

# 37. OMBasic 0.1 实现绑定

本章为 0.1 参考实现补足前文未定细节。若前文使用“推荐”“可以”或示例未给出完整语义，以本章作为参考实现的一致性定义。

## 37.1 词法与模块

**OM-37.01 — 词法与模块实现绑定**


普通名称的模式为 lower { lower | digit } { "_" lower { lower | digit } }；实体名称的模式为 upper { lower | digit } { "_" upper { lower | digit } }。lower、upper、digit 分别表示 ASCII 小写字母、大写字母、数字。不符合两种模式的用户标识符产生 E0102 诊断，不得自动改写。

第 5.7 节列出的关键字拼写按 ASCII 大小写不敏感方式保留。只有与列表完全一致的全大写拼写才生成关键字 token；同一拼写的其他大小写形式产生 E0101 诊断，不得作为标识符使用。此规则也适用于关键字虽未出现在当前产生式中的情形，并且优先于 identifier_candidate 的名称模式检查。词法分析器将以 ASCII 字母或下划线开头、后续仅含 ASCII 字母/数字/下划线的最长序列视为 identifier_candidate；它必须匹配 ordinary_identifier 或 entity_identifier，否则产生 E0102。以数字开头并紧接 ASCII 字母或下划线的数字序列是非法 token，产生 E0103。其他不支持的源字符、非法转义、非法 UTF-8 和单独 CR 也产生 E0103。

整数使用无分隔符的十进制数字，范围为 signed Int64。一元负号直接作用于整数字面量 9223372036854775808 时，该操作数表示 INT64_MIN；空白可以位于两个 token 之间，括号或其他运算符不能介入。任何其他超出 Int64 的整数字面量产生 E0104。整数加减乘除、取余和取负检查溢出；整数除法向零截断，除零和溢出产生运行时算术错误。浮点数使用 digit+ "." digit+ [ exponent ] 或 digit+ exponent，指数为 e/E、可选正负号和 digit+。浮点字面量必须转换为有限 binary64，否则产生 E0104；浮点运算遵循 IEEE-754。Int 与 Float 不隐式转换。

字符串为合法 UTF-8，不得包含未转义换行。允许的转义为反斜线后接反斜线、双引号、n、r、t、0，或 u{hex}；hex 为 1 至 6 个十六进制数字，Unicode 转义必须是有效 Unicode scalar value。插值字符串以 $" 开始；单个花括号包围表达式，双左/右花括号产生字面花括号。0.1 只插值 Int、Float、Bool、String、Null，格式化分别为十进制、binary64 最短往返格式、TRUE/FALSE、原字符串、NULL。

普通 .bas Module 仅包含顶层 IMPORT，随后为声明；IMPORT 必须在所有声明前。Module 顶层语句和初始化代码非法。源码换行结束简单语句；括号、方括号、花括号中的换行等同空白。分号不是分隔符。

参考实现采用严格 source-root 策略：source root 自身会先规范化；导入文件的每个路径段必须按请求大小写精确解析，POSIX 实现拒绝符号链接，Windows 实现拒绝 reparse point 或最终规范路径与请求路径不一致的别名。此策略禁止的范围比“符号链接目标仍在 source root 内”更严格，但不改变 OMBasic 源码中的 Import 语义。

词法产生式中的 lower、upper、digit 分别为 ASCII a-z、A-Z、0-9；hex 为十六进制数字：

~~~text
ordinary_identifier = lower { lower | digit } { "_" lower { lower | digit } } ;
entity_identifier   = upper { lower | digit } { "_" upper { lower | digit } } ;
ordinary            = ordinary_identifier ;
entity              = entity_identifier ;
integer_literal     = digit { digit } ;
identifier_candidate = (lower | upper | "_") { lower | upper | digit | "_" } ;
float_literal       = digit { digit } "." digit { digit } [ exponent ]
                    | digit { digit } exponent ;
exponent            = ("e" | "E") [ "+" | "-" ] digit { digit } ;
string_literal      = '"' { string_char | escape } '"' ;
interpolated_string = '$"' { string_char | escape | "{{" | "}}" | interpolation } '"' ;
interpolation       = "{" expression "}" ;
escape              = "\\" | "\"" | "\n" | "\r" | "\t" | "\0"
                    | "\u{" hex { hex } "}" ;
hex                 = digit | "a"…"f" | "A"…"F" ;
newline             = LF | CR LF ;
whitespace          = SPACE | TAB ;
~~~

float_literal 中的 exponent 与小数点至少出现一个。Unicode escape 中 hex 数量为 1 至 6。string_char 是合法 UTF-8 scalar value，但不包括双引号、反斜线、LF、CR；插值字符串的花括号按上面特殊规则处理。逻辑换行是 LF 或 CRLF，单独 CR 非法。SPACE 仅指 U+0020，TAB 仅指 U+0009；它们在 token 间忽略。其他空白码点不属于空白。普通注释按第 4.5 节处理；注释内容不产生 token，但其结尾逻辑换行仍产生 NEWLINE。文档注释与其后的声明关联，不产生语法 token。双字符运算符优先识别为 <>、<=、>=；其他运算符和标点为 + - * / % = < > . ( ) [ ] { } , :。数字后立即跟随 ASCII 字母或下划线是非法数字 token，而不是相邻的两个 token。

## 37.2 核心语法

**OM-37.02 — 核心语法实现绑定**


下列产生式补足第 32 节的语法草图。NEWLINE 是上文定义的 newline token；line_end 表示一个或多个连续换行。分号不属于语法。简单语句以 line_end 结束；复合语句由对应的 END 子句结束并以 line_end 分隔后续同层语句。解析 block 时，遇到当前层级的 ELSE、CASE 或 END 即结束该 block。括号、方括号、花括号以及插值表达式内部的逻辑换行作为空白，不产生 NEWLINE。文件末尾 EOF 可以满足最后一个 line_end，且 EOF 只能出现一次并必须结束 program。

~~~text
program       = { NEWLINE } { import_decl line_end }
                { declaration line_end } ;
line_end      = NEWLINE { NEWLINE } | EOF ;
import_decl   = "IMPORT" string_literal ;
declaration   = [ "EXPORT" ] ( record_decl | message_decl | function_decl
                             | action_decl | actor_decl ) ;
record_decl   = "RECORD" entity "(" [ names ] ")" ;
message_decl  = "MESSAGE" entity "(" [ names ] ")" ;
function_decl = "FUNCTION" ordinary "(" [ names ] ")" NEWLINE block
                "END" "FUNCTION" ;
action_decl   = "ACTION" ordinary "(" [ names ] ")" NEWLINE block
                "END" "ACTION" ;
actor_decl    = "ACTOR" entity line_end
                { actor_state line_end } { receive_decl line_end }
                "END" "ACTOR" ;
actor_state   = "VAR" ordinary "=" expression ;
receive_decl  = "RECEIVE" entity "(" [ names ] ")" line_end block
                "END" "RECEIVE" ;
names         = ordinary { "," ordinary } ;
block         = { NEWLINE | simple_statement line_end | compound_statement } ;

simple_statement = binding_or_assignment | return_stmt | "BREAK" | "CONTINUE"
                 | send_stmt | watch_stmt | "STOP" | call_stmt ;
binding_or_assignment = [ "VAR" ] ordinary "=" expression ;
return_stmt   = "RETURN" [ expression ] ;
send_stmt     = "SEND" expression "," expression ;
watch_stmt    = "WATCH" expression ;
call_stmt     = call_expression ;
compound_statement = if_stmt | while_stmt | for_stmt | select_stmt ;
if_stmt       = "IF" expression "THEN" line_end block
                { "ELSE" "IF" expression "THEN" line_end block }
                [ "ELSE" line_end block ] "END" "IF" line_end ;
while_stmt    = "WHILE" expression line_end block "END" "WHILE" line_end ;
for_stmt      = "FOR" ordinary "=" expression "TO" expression
                [ "STEP" expression ] line_end block "END" "FOR" line_end
              | "FOR" ordinary "IN" expression line_end block
                "END" "FOR" line_end ;
select_stmt   = "SELECT" expression line_end
                { "CASE" case_pattern line_end block }
                [ "CASE" "ELSE" line_end block ] "END" "SELECT" line_end ;
case_pattern  = expression | "Ok" "(" ordinary ")"
                             | "Error" "(" ordinary ")" ;

expression    = or_expression ;
or_expression = and_expression { "OR" and_expression } ;
and_expression = compare_expression { "AND" compare_expression } ;
compare_expression = additive_expression
                     [ compare_operator additive_expression ] ;
compare_operator = "=" | "<>" | "<" | ">" | "<=" | ">=" ;
additive_expression = multiplicative_expression
                      { ("+" | "-") multiplicative_expression } ;
multiplicative_expression = unary_expression
                            { ("*" | "/" | "%") unary_expression } ;
unary_expression = ("-" | "NOT" | "TRY") unary_expression
                 | "SPAWN" entity [ "AT" postfix_expression ]
                 | postfix_expression ;
postfix_expression = primary_expression
                     { "." ordinary | "(" [ arguments ] ")"
                     | "[" expression "]" | "WITH" field_updates } ;
primary_expression = integer_literal | float_literal | string_literal
                   | interpolated_string | "TRUE" | "FALSE" | "NULL" | "SELF"
                   | ordinary | entity | "(" expression ")"
                   | list_literal | map_literal ;
arguments     = expression { "," expression } ;
list_literal  = "[" [ arguments ] "]" ;
map_literal   = "{" [ expression ":" expression
                      { "," expression ":" expression } ] "}" ;
field_updates = ordinary "=" expression { "," ordinary "=" expression } ;
call_expression = primary_expression { "." ordinary | "[" expression "]" }
                  "(" [ arguments ] ")"
                  { "." ordinary | "[" expression "]"
                  | "(" [ arguments ] ")" | "WITH" field_updates } ;
~~~

postfix_expression 的 [ expression ] 表示 List/Map 索引。List 索引从零开始；负索引、非 Int 索引或越界产生运行时索引错误。Map 索引必须使用其 key 类型；缺失键产生运行时索引错误。Map 按字面插入顺序迭代，重复的已求值键产生运行时错误；编译器能证明常量键重复时应在编译期拒绝。遍历 List 按元素顺序；遍历 Map 产生键。

括号、方括号、花括号和插值表达式中的换行等同空白。表达式优先级由高到低为：调用/字段/括号，前缀负号与 NOT，乘除取余，加减，比较，AND，OR；比较运算不得链式连接。

binding_or_assignment 有统一语法，由名称解析决定含义：不可见名称创建不可变绑定；已可见的可变绑定可重新赋值；已可见的不可变绑定赋值报错。VAR 总是创建新的可变绑定。赋值只能写本地绑定。当前可见词法路径上的名称不得再次声明；兄弟块可以复用名称。导入限定名和内建名不得遮蔽。

IF/WHILE 条件必须为 Bool。FOR 数值范围两端包含在内；起点、终点、步长各计算一次。默认步长为 1，步长必须非零；正步长递增，负步长递减。循环变量是循环体内不可变 Int。集合循环变量不可变。Map 键仅支持 Int、Bool、String；List/Map 不可变，空集合需要期望类型。

相等比较支持 Int、Float、Bool、String、Null、ActorRef identity 和 payload 可比较时的 Result。Float 相等遵循 IEEE-754。顺序比较只支持 Int、Float 和按 Unicode scalar value 字典序比较的 String。List、Map、Record 不定义相等。

SELECT 只计算一次 scrutinee，CASE 按顺序匹配，无 fallthrough；CASE ELSE 可选且只能位于最后。CASE 表达式必须与 scrutinee 同型且为纯表达式。Result 可用 Ok(name) 或 Error(name) 解构，名称只在该 arm 中可见。WITH 复制 Record，至少更新一个存在且不重复的字段，字段值必须同型。

## 37.3 类型、Actor 与入口绑定

**OM-37.03 — 类型、Actor 与入口实现绑定**


Null 是独立单例类型；0.1 没有 nullable 类型，Null 不能赋给其他类型。Result<T,E> 是内建 tagged union，Ok(value) 与 Error(error) 从上下文推导。Function、Action 返回类型由 RETURN 推导；无值返回为 Unit。TRY 接收 Result<T,E>，在 Ok 时产出 T，在 Error 时通过相同 E 的外围 Result 返回通道传播。Receive handler 返回 Unit；TRY 在 Receive 中传播错误会使当前 Actor 标记为 FAILED 并停止处理消息，观察者只收到失败原因，不收到原始错误值。

#### 标准内建 nominal 值

内建类型包括 Unit、Result、Node_Ref、Actor_Id、Actor_Down_Reason；内建 Result 构造器为 Ok 与 Error。SELF 仅能在 Actor handler 中使用，表示当前 ActorRef。

Actor_Down_Reason 是标准内建 nominal type，只有以下三个标准值：

| 源代码值 | 规范含义 |
|---|---|
| `Actor_Down_Reason.normal` | Runtime 已知 Actor 正常终止。 |
| `Actor_Down_Reason.failed` | Runtime 已知 Actor 因 handler、initializer 或所包含的 Runtime fault 失败终止。 |
| `Actor_Down_Reason.node_unreachable` | 本地节点无法确认远端 Actor 状态；这不证明远端 Actor 已终止。 |

三个值必须通过限定成员语法引用。它们不是全局变量、关键字或裸 enum member，也不能由上下文自动推导。Runtime 的内部整数表示不属于语言源代码语义。

内建系统 Message `Actor_Down(actor_id, reason)` 对所有 Actor 协议可接收；`actor_id` 类型为 Actor_Id，`reason` 类型为 Actor_Down_Reason。用户不能重新声明或遮蔽这些内建名称及 Actor_Down。STOP 仅能在 Actor 执行上下文中使用，正常终止当前 Actor；Actor 收到 STOP 后不再处理 mailbox 中的新 Message。

Actor_Down_Reason.node_unreachable 表示本地节点无法确认远端 Actor 的状态，不证明该 Actor 已经终止。网络分区、连接丢失或远端节点故障都可能导致这种不确定状态；远端 Actor 可能仍在运行。

Actor 构造没有参数。Actor state 初始化表达式必须纯，且仅能引用先前 state 和纯模块声明；每个实例单独求值。SPAWN Actor 在本地创建；SPAWN Actor AT node 要求 Node_Ref。WATCH 只能在 Actor 中使用，重复 watch 幂等。Actor_Down 到达 watcher 的 mailbox，遵守普通 Message 的 run-to-completion 顺序。

CLI 指定的根源码是入口 Module。它必须恰好定义一个无参数、返回 Unit 的 ACTION main()；导入 Module 不得定义 main。IMPORT 和模块加载没有运行时副作用。

## 37.4 Native metadata 绑定

**OM-37.04 — Native metadata 实现绑定**


.omi 恰好有一个 NATIVE block，使用独立于 .bas 的元数据 token 模式。元数据关键字必须精确匹配产生式中的大写拼写；abi_type 中的 CVoid、CInt8 等拼写是 .omi 专用终结符，不要求符合 OMBasic 实体名称模式。每个 Native function 必须恰有一个 SYMBOL、一个 EFFECT 和一个 BLOCKING 或 NONBLOCKING 注解。EFFECT 为 PURE，或 NATIVE、FILE_IO、NETWORK_IO、CLOCK 的非空组合。指针参数必须注明 BORROWED、OWNED 或 STATIC 中恰好一个；指针返回必须恰有一个 RETURNS BORROWED FROM 参数、RETURNS STATIC 或 RETURNS OWNED WITH 释放函数声明。Owned 返回必须有释放函数。Native 未明确证明 PURE 时视为有副作用。Foreign-thread callback 只能通过 Runtime trampoline 入队，不得直接执行 Actor handler。

~~~text
native_file = "NATIVE" string_literal NEWLINE
              { native_decl NEWLINE } "END" "NATIVE" ;
native_decl = native_function | native_record ;
native_function = "FUNCTION" ordinary "(" [ native_parameters ] ")"
                  ":" native_type NEWLINE { native_annotation NEWLINE }
                  "END" "FUNCTION" ;
native_parameters = native_parameter { "," native_parameter } ;
native_parameter = ordinary ":" native_type
                   [ "BORROWED" | "OWNED" | "STATIC" ]
                 | ordinary ":" callback_type ;
native_type = abi_type | entity ;
callback_type = "CALLBACK" "(" [ callback_parameters ] ")" "->" abi_type ;
callback_parameters = abi_type { "," abi_type } ;
native_annotation = "SYMBOL" string_literal
                  | "EFFECT" ("PURE" | native_effect { "," native_effect })
                  | "BLOCKING" | "NONBLOCKING"
                  | "RETURNS" "BORROWED" "FROM" ordinary
                  | "RETURNS" "STATIC"
                  | "RETURNS" "OWNED" "WITH" ordinary
                  | "CALLBACK" ordinary "FOREIGN_THREAD" "MESSAGE" entity ;
native_effect = "NATIVE" | "FILE_IO" | "NETWORK_IO" | "CLOCK" ;
native_record = "NATIVE" "RECORD" entity NEWLINE
                native_record_field { native_record_field }
                "END" "NATIVE" "RECORD" ;
native_record_field = ordinary ":" abi_type
                      [ "BORROWED" | "OWNED" | "STATIC" ] NEWLINE ;
abi_type = "CVoid" | "CInt8" | "CInt16" | "CInt32" | "CInt64"
         | "CUInt8" | "CUInt16" | "CUInt32" | "CUInt64"
         | "CInt" | "CUInt" | "CLong" | "CULong"
         | "CLongLong" | "CULongLong" | "CFloat" | "CDouble"
         | "CSize" | "CBool" | "CPointer" | "CString" ;
~~~

native_parameter 也可以写为 ordinary ":" callback_type。callback_parameters 为以逗号分隔的 abi_type。callback_type 参数必须恰好有一个 CALLBACK ordinary FOREIGN_THREAD MESSAGE entity 注解；该注解必须引用声明过的 callback 参数。Callback 参数不得使用 BORROWED、OWNED 或 STATIC 注解。Callback 的 ABI 参数顺序和类型必须与指定 Message 的字段数及字段类型兼容；CPointer、CString 不得出现在该 Message 中。0.1 callback 只在对应 Native 函数调用期间有效，Native 函数不得保存它以供调用返回后使用；Runtime trampoline 在外部线程复制字段并向注册 Actor 入队，绝不直接执行 Actor handler。CVoid 只可用于 Native function 或 callback 的返回类型，不能作为参数、Record 字段或普通 OMBasic 值类型。

abi_type 名称按上表精确匹配；CVoid、CInt8 等不是用户标识符，且只在 .omi 中有效。每个 NATIVE block 头部的 string_literal 是动态库路径。Native annotations 不得重复；不适用于声明种类的 annotation 是错误。Native Record 的 CPointer、CString 字段必须恰有一个 BORROWED、OWNED、STATIC 标注，非指针字段不得带生命周期标注；具体释放责任由生成的 binding contract 承担。

`native_type` 中的 entity 名称必须解析到同一 NATIVE block 的 Native Record，声明可位于函数前后。函数参数和返回值中的 Native Record 表示按值传递的 C struct，不需要 `RETURNS` ownership annotation。Callback 签名和 Native Record 字段仍只能使用 `abi_type`，因此 0.1 不支持嵌套 Native Record。按值 marshalling 复制结构体字段字节；若字段包含指针，只复制指针值，不复制或管理其指向的内存，调用方与 Native wrapper 必须遵守相应的生命周期约定。

Native Record 至少包含一个字段，以确保它对应 ISO C 中有效的非空 struct。`.omi` 元数据诊断使用 E0801–E0808：分别表示语法错误、名称形式错误、必需/重复注解错误、ABI 类型错误、ownership 错误、callback 绑定错误、重复 Native 声明/符号，以及库路径或 effect 错误。
