# OMBasic High-Performance Query v0.1 开发计划

> **文档类型：** Compiler / Language Feature Development Plan  
> **版本：** 1.0-draft  
> **日期：** 2026-10-30  
> **目标版本：** Query v0.1  
> **适用项目：** OMBasic  
> **仓库：** https://github.com/risol/OMBasic  
> **核心目标：** 实现一套高性能、静态类型、同步、纯计算的内存 Query 语法；首阶段只覆盖单 `List<T>` 数据源、`WHERE`、`ORDER BY`、`TAKE`、`SELECT`，并支持 loop fusion、stable sort 与 Top-K 优化。

---

# 1. 执行摘要

OMBasic Query v0.1 的目标不是复制 C# LINQ 的完整运行时抽象，也不是在语言中嵌入 SQL。

本阶段要实现的是：

```basic
result = QUERY user IN users
    WHERE user.active
    WHERE user.age >= 18

    ORDER BY
        user.score DESCENDING,
        user.id ASCENDING

    TAKE 100

    SELECT User_View(
        user.id,
        user.name,
        user.score
    )
END QUERY
```

编译器应当将上述 Query 识别为一个完整的声明式计算区域，进行静态类型检查、纯度检查和专门优化，最后直接 Lower 成 MIR / LLVM 本地代码。

对于不包含排序的 Query，目标执行形态接近手写循环：

```text
one source scan
+ fused predicates
+ optional early TAKE exit
+ direct projection
+ one result construction
```

首阶段不引入：

- Lazy Iterator；
- `IEnumerable<T>` 风格的运行时 pipeline；
- `IQueryable<T>`；
- Runtime Expression Tree；
- Query Runtime AST；
- Dynamic Query Builder；
- SQL Provider；
- Database Actor；
- JOIN；
- GROUP BY；
- Aggregate；
- DISTINCT；
- SKIP；
- Query 自动并行；
- Query 自动异步；
- Future / Promise / async / await。

Query 与 Actor 的关系明确为：

> **Actor 管并发、状态所有权和异步；Query 管当前执行上下文中的同步纯数据计算。**

Query 不会改变 Actor Scheduler，也不会成为另一套调度系统。

---

# 2. v0.1 范围冻结

## 2.1 支持的语法

首版只支持：

```text
QUERY
WHERE
ORDER BY
TAKE
SELECT
END QUERY
```

数据源只支持：

```text
List<T>
```

Query 结果固定为：

```text
List<R>
```

Query 是：

```text
eager
synchronous
non-yielding
side-effect-free
```

## 2.2 六个实施目标

本计划严格限定到以下六步：

1. Query Syntax；
2. Typed HIR；
3. Temporary Query IR；
4. In-memory `List<T>` execution；
5. Loop Fusion；
6. Stable ORDER + Top-K。

## 2.3 明确不做

v0.1 不支持：

```text
JOIN
GROUP BY
HAVING
SUM
AVG
MIN aggregate
MAX aggregate
COUNT aggregate
DISTINCT
SKIP
UNION
INTERSECT
EXCEPT
nested FROM
LET
INTO
dynamic Query<T>
lazy Query<T>
database source
SQL translation
network source
actor source
stream source
Map source
String source
Range source
parallel query
```

未来可以扩展，但不得为了未来功能提前污染 v0.1 的运行时模型。

---

# 3. 核心语言语义

## 3.1 Query 是表达式

Query 可以出现在普通表达式允许的位置：

```basic
result = QUERY item IN items
    SELECT item
END QUERY
```

例如：

```basic
RETURN QUERY item IN items
    WHERE item.active
    SELECT item
END QUERY
```

前提是普通语言语法允许该位置使用表达式。

## 3.2 Query 是 eager

执行：

```basic
result = QUERY item IN items
    WHERE item.active
    SELECT item
END QUERY
```

完成后：

```text
result : List<T>
```

已经完整 materialize。

不存在延迟求值。

因此：

```basic
result = QUERY item IN items
    WHERE item.active
    SELECT item
END QUERY
```

之后即使原 source binding 被重新赋值，也不重新执行 Query。

## 3.3 Query 是同步的

Query 执行期间：

- 不 yield；
- 不 suspend；
- 不切换 continuation；
- 不等待 Message；
- 不自动进入 Worker Pool；
- 不自动创建线程；
- 不自动创建 Actor。

它与普通：

```basic
FOR item IN items
```

一样属于当前执行上下文中的同步 CPU 工作。

## 3.4 Query 是纯计算

Query clause 只能包含 Pure expression。

允许：

```basic
WHERE user.age >= 18
WHERE normalize_score(user.score) >= minimum_score
SELECT User_View(user.id, user.name)
```

前提：

```text
normalize_score = pure FUNCTION
User_View construction = pure
```

禁止：

```text
SEND
SPAWN
STOP
WATCH
ACTION call
Native effectful call
I/O
Actor state mutation
global mutable state mutation
```

## 3.5 Query 不直接读取其他 Actor 的状态

禁止：

```basic
QUERY user IN user_store_actor
```

如果 `user_store_actor` 是 ActorRef。

Query source v0.1 必须是当前执行上下文已经拥有的：

```text
List<T>
```

如果数据归另一个 Actor：

```text
caller
  ↓ SEND query request
data owner actor
  ↓ local QUERY
  ↓ SEND result
caller
```

遵循：

> **Move computation to the data owner, not actor-owned state to arbitrary readers.**

---

# 4. 推荐语法

## 4.1 基本形式

```basic
result = QUERY item IN items
    SELECT item
END QUERY
```

## 4.2 WHERE

```basic
result = QUERY user IN users
    WHERE user.active
    WHERE user.age >= minimum_age
    SELECT user
END QUERY
```

允许：

```text
WHERE 0..N
```

多个 WHERE 按源码顺序执行。

语义上类似：

```text
predicate1 AND predicate2 AND ...
```

但编译器不得在 v0.1 任意重新排序 predicate。

原因：

即使表达式无副作用，表达式仍可能产生：

- checked integer overflow；
- division by zero；
- index fault；
- 其他语言定义的 Runtime Fault。

重新排序可能改变首先暴露的 fault。

因此：

```text
Fusion allowed
Reordering forbidden
```

## 4.3 ORDER BY

```basic
result = QUERY user IN users
    ORDER BY user.name ASCENDING
    SELECT user
END QUERY
```

多 Key：

```basic
result = QUERY user IN users
    ORDER BY
        user.score DESCENDING,
        user.id ASCENDING
    SELECT user
END QUERY
```

v0.1 建议强制方向显式：

```text
ASCENDING
DESCENDING
```

不允许省略。

这样避免：

```text
ORDER BY x
```

到底默认是什么需要额外记忆。

## 4.4 TAKE

```basic
result = QUERY user IN users
    WHERE user.active
    TAKE 10
    SELECT user
END QUERY
```

`TAKE` 必须出现在：

```text
ORDER BY 之后
SELECT 之前
```

如果没有 ORDER：

```text
source-order TAKE
```

如果存在 ORDER：

```text
ordered TAKE
```

## 4.5 SELECT

SELECT 必须恰好出现一次。

支持：

```basic
SELECT user
```

```basic
SELECT user.name
```

```basic
SELECT user.price * user.quantity
```

```basic
SELECT User_View(
    user.id,
    user.name
)
```

结果类型：

```text
List<type_of(select_expression)>
```

---

# 5. Grammar

建议概念 EBNF：

```text
query_expression
    := QUERY identifier IN expression newline
       query_where_clause*
       query_order_clause?
       query_take_clause?
       query_select_clause
       END QUERY

query_where_clause
    := WHERE expression newline

query_order_clause
    := ORDER BY query_order_key
       (COMMA query_order_key)* newline

query_order_key
    := expression (ASCENDING | DESCENDING)

query_take_clause
    := TAKE expression newline

query_select_clause
    := SELECT expression newline
```

实际 parser 必须适配 OMBasic 当前 newline / token / block 规则，不要求机械复制上述 EBNF。

## 5.1 固定 Clause 顺序

合法：

```text
QUERY
WHERE*
ORDER BY?
TAKE?
SELECT
END QUERY
```

非法：

```basic
QUERY x IN xs
    SELECT x
    WHERE x > 0
END QUERY
```

非法：

```basic
QUERY x IN xs
    TAKE 10
    ORDER BY x ASCENDING
    SELECT x
END QUERY
```

固定顺序的价值：

- Parser 简单；
- Formatter 简单；
- LSP 简单；
- Diagnostics 明确；
- Query optimizer 不需要处理任意 clause sequence；
- 用户代码可快速扫描。

---

# 6. Keyword 策略

新增：

```text
QUERY
WHERE
ORDER
BY
TAKE
ASCENDING
DESCENDING
```

复用已有：

```text
IN
SELECT
END
```

不引入：

```text
PROJECT
YIELD
LIMIT
FROM
```

原因：

### 不用 PROJECT

虽然 Query IR 中叫 Projection，但用户语法：

```basic
PROJECT ...
```

过于编译器术语化。

### 不用 YIELD

容易让用户联想到：

```text
Generator
Coroutine
lazy iteration
```

与 Query v0.1 eager 模型冲突。

### 不用 LIMIT

`TAKE` 更接近通用集合语义，不绑定 SQL 方言。

### 不用 FROM

OMBasic 已有：

```basic
FOR item IN items
```

因此：

```basic
QUERY item IN items
```

与现有语言心智模型更一致。

---

# 7. 静态类型规则

## 7.1 Source

```basic
QUERY item IN source
```

要求：

```text
type(source) = List<T>
```

则：

```text
type(item) = T
```

否则编译错误。

例如：

```basic
QUERY c IN "hello"
```

v0.1 应报不支持的 source type，而不是偷偷把 String 当 iterable。

## 7.2 WHERE

每个：

```basic
WHERE expression
```

要求：

```text
type(expression) = Bool
```

不做 truthy 转换。

## 7.3 TAKE

要求：

```text
type(expression) = Int
```

建议语义：

```text
TAKE <= 0
```

返回空 List，而不是 Runtime Fault。

理由：

- 更适合集合操作；
- Top-K K=0 可直接优化为空；
- 避免引入新的 fault 类别。

如果项目希望负数是程序错误，可通过 ADR 改成 fault；但必须在实现前冻结。

本计划推荐：

```text
K <= 0 => empty result
```

## 7.4 ORDER key

v0.1 只允许语言已经定义确定比较语义的类型。

建议初始允许：

```text
Int
String
Bool（可选，不强烈需要）
```

Float 是否允许，应取决于语言当前对：

```text
NaN
-0.0
Infinity
```

是否有稳定 total ordering。

如果没有，v0.1 不开放 Float sort key。

Record / List / Map 不允许直接 ORDER。

## 7.5 SELECT

任意 Pure expression：

```text
T -> R
```

Query 类型：

```text
List<R>
```

---

# 8. Scope 与 Name Resolution

## 8.1 Range variable

```basic
QUERY user IN users
```

创建一个 Query-local immutable binding：

```text
user : element_type(users)
```

作用域覆盖：

```text
WHERE
ORDER BY
TAKE?     // TAKE 一般不需要 user，但 scope 规则应明确
SELECT
```

建议 TAKE 也允许读取外部变量，但不允许读取 range variable：

```basic
TAKE user.limit
```

应拒绝。

原因：

TAKE 是 Query-level cardinality，不是 per-row expression。

## 8.2 Source expression scope

Source 在 range variable 建立之前解析：

```basic
QUERY user IN users
```

`users` 中不能引用这个刚声明的 `user`。

## 8.3 外部 lexical binding

允许：

```basic
minimum_age = 18

result = QUERY user IN users
    WHERE user.age >= minimum_age
    SELECT user
END QUERY
```

由于 Query 立即执行且不逃逸：

- 不需要 closure object；
- 不需要 capture record；
- 不需要 runtime expression tree。

Compiler 只需正常引用 outer binding。

---

# 9. Effect Rules

建议将 Query 纳入现有 effect analysis。

每个 Query 内表达式必须满足：

```text
effects == Pure
```

至少检查：

```text
source expression
WHERE expressions
ORDER expressions
TAKE expression
SELECT expression
```

但 source expression 是否允许读取当前 Actor state，需要区别：

读取 Actor state 本身可以是当前 handler 内的值读取，不意味着 Query 内有副作用。

建议规则：

```text
read current lexical / actor-owned value: allowed
mutate actor state: impossible inside Query expression
effectful calls: forbidden
```

如果当前 effect model 对 Actor state read 也打 effect 标志，应添加 Query-specific 可接受 effect mask，而不是简单要求 raw `effects == 0`。

目标语义是：

> Query 内不得产生 externally observable side effects，也不得挂起或与 Actor Runtime 通信。

---

# 10. Actor 集成规范

## 10.1 正常模式

```basic
MESSAGE Find_Users(minimum_age, reply)
MESSAGE Find_Users_Completed(users)

ACTOR User_Store(users)

    RECEIVE Find_Users(minimum_age, reply)

        result = QUERY user IN users
            WHERE user.age >= minimum_age
            WHERE user.active
            SELECT user
        END QUERY

        SEND reply, Find_Users_Completed(result)

    END RECEIVE

END ACTOR
```

Query 在当前 Actor handler 中同步执行。

## 10.2 Scheduler 不感知 Query

不得新增：

```text
QUERY_RUNNING
QUERY_YIELD
QUERY_RESUME
QUERY_TASK
```

Actor Scheduler 只看到：

```text
handler currently executing native code
```

Query 与普通 CPU loop 没有调度语义差异。

## 10.3 Query 不隐式并行

禁止 compiler 自动：

```text
split source into N worker threads
```

理由：

Actor concurrency 与 Query hidden parallelism 叠加会造成：

```text
oversubscription
unpredictable latency
complex shutdown
harder TSan reasoning
```

未来若需要 parallel analytics，应通过显式 Actor 架构解决。

## 10.4 长 Query 的责任

大型同步 Query 会占用当前 Actor handler。

这属于 Actor responsiveness 问题，不通过 hidden async 修复。

正确方案：

```text
small/medium actor state:
    query in owner actor

large analytics workload:
    dedicated query/data actor
```

例如：

```text
business actor
    ↓ message
analytics actor owning data
    ↓ local QUERY
    ↓ result message
business actor
```

---

# 11. 编译管线

Query v0.1 采用：

```text
Source
  ↓
Lexer / Parser
  ↓
AST
  ↓
Name Resolution
  ↓
Type Inference
  ↓
Effect / Semantic Analysis
  ↓
HIR
  ↓
Temporary Query IR
  ↓
Query Optimizer
  ↓
Query Physical Plan
  ↓
MIR
  ↓
LLVM
  ↓
Native
```

关键原则：

> **QIR 不转回 AST。**

禁止：

```text
AST
 ↓
HIR
 ↓
QIR
 ↓
synthetic AST
 ↓
HIR again
 ↓
MIR
```

理由：

- AST 是 source syntax representation；
- HIR 已经拥有 Symbol / Type / Effect 等语义身份；
- synthetic AST 会迫使重新 name resolution / type inference；
- source span 与 diagnostics 变复杂；
- generic specialization identity 容易丢失；
- 无性能收益。

---

# 12. AST 设计

建议增加：

```text
OM_AST_QUERY
OM_AST_QUERY_WHERE
OM_AST_QUERY_ORDER
OM_AST_QUERY_ORDER_KEY
OM_AST_QUERY_TAKE
OM_AST_QUERY_SELECT
```

也可以用单个 Query node + positional child convention，但不推荐把所有 clause 都塞成无类型 children。

显式 clause kind 更利于：

- diagnostics；
- formatter；
- AST dump；
- tooling；
- future extension。

## 12.1 Query AST 布局

概念：

```text
QUERY
├── binding identifier
├── source expression
├── WHERE
│   └── expression
├── WHERE
│   └── expression
├── ORDER
│   ├── ORDER_KEY
│   │   ├── expression
│   │   └── direction
│   └── ORDER_KEY
│       ├── expression
│       └── direction
├── TAKE
│   └── expression
└── SELECT
    └── expression
```

方向可作为：

```text
flags
```

而不是伪造 expression child。

## 12.2 Source span

每个 clause 保存自己的 span。

这样错误可以指向：

```basic
WHERE user.name
^^^^^
```

而不是整个 QUERY block。

---

# 13. Semantic Analysis

需要处理：

1. Source 类型必须是 `List<T>`；
2. 创建 range variable symbol；
3. Range variable immutable；
4. WHERE 必须 Bool；
5. TAKE 必须 Int；
6. ORDER key 必须属于可排序类型；
7. SELECT 推断结果类型；
8. Query 内 effect restrictions；
9. Clause 顺序由 parser 保证；
10. Query result type = `List<R>`。

## 13.1 Diagnostic 建议

可分配新的 Query diagnostic family，例如：

```text
E12xx
```

具体编号需避开仓库当前已占用范围。

建议错误语义：

```text
QUERY_SOURCE_NOT_LIST
QUERY_WHERE_NOT_BOOL
QUERY_TAKE_NOT_INT
QUERY_ORDER_TYPE_UNSUPPORTED
QUERY_EFFECT_NOT_ALLOWED
QUERY_RANGE_VARIABLE_ASSIGN
QUERY_SELECT_MISSING
QUERY_CLAUSE_ORDER
QUERY_DUPLICATE_ORDER
QUERY_DUPLICATE_TAKE
```

Diagnostics 必须：

- 指向最小相关 span；
- 告诉用户实际类型；
- 告诉用户期望类型；
- 不暴露 QIR / MIR 内部术语。

---

# 14. HIR 设计

当前 HIR 已经承担：

- typed nodes；
- resolved symbols；
- source node identity；
- effect metadata；
- source span。

因此 Query 应进入 HIR，而不是绕过 HIR 直接从 AST 建 QIR。

建议增加：

```text
OM_HIR_QUERY
OM_HIR_QUERY_WHERE
OM_HIR_QUERY_ORDER
OM_HIR_QUERY_ORDER_KEY
OM_HIR_QUERY_TAKE
OM_HIR_QUERY_SELECT
```

如果希望减少 HIR node kind，也可：

```text
OM_HIR_QUERY
```

配合 child roles，但显式 kind 更容易 dump / verify。

## 14.1 HIR Query 必须已经完全 typed

例如：

```text
OM_HIR_QUERY
source_type      = List<User>
binding_type     = User
result_type      = List<User_View>

WHERE #1         Bool
WHERE #2         Bool
ORDER key        Int
TAKE             Int
SELECT            User_View
```

QIR 不负责基础类型推导。

## 14.2 HIR Verifier

应加入：

- source must be List；
- where Bool；
- take Int；
- select type valid；
- result List projection；
- range binding symbol valid；
- query effects within allowed mask；
- order direction valid。

---

# 15. Temporary Query IR

v0.1 不建立大型独立：

```text
Om_Query_Project
```

系统。

建议 QIR 作为：

```text
HIR -> MIR lowering
```

中的临时计划。

未来 JOIN / SQL Provider 出现后，再考虑提升成正式：

```text
src/ir/query/
include/ombasic/ir/query.h
```

## 15.1 Logical operators

只需要：

```c
typedef enum Om_Query_Op_Kind {
    OM_QUERY_SCAN = 0,
    OM_QUERY_FILTER,
    OM_QUERY_ORDER,
    OM_QUERY_TAKE,
    OM_QUERY_PROJECT
} Om_Query_Op_Kind;
```

## 15.2 Expression 不复制

QIR 不创建：

```text
QueryBinaryExpression
QueryFieldExpression
QueryCallExpression
```

普通 expression 继续引用 HIR node：

```c
typedef struct Om_Query_Filter {
    Om_Hir_Node_Id predicate;
} Om_Query_Filter;
```

```c
typedef struct Om_Query_Order_Key {
    Om_Hir_Node_Id key_expression;
    Om_Query_Order_Direction direction;
} Om_Query_Order_Key;
```

这样：

- expression typing 只做一次；
- symbol resolution 只做一次；
- source span 保留；
- function specialization 仍走现有路径；
- QIR 只关注关系型 operator。

## 15.3 Logical plan 示例

源码：

```basic
QUERY user IN users
    WHERE user.active
    WHERE user.age >= 18
    ORDER BY
        user.score DESCENDING,
        user.id ASCENDING
    TAKE 10
    SELECT User_View(user.id, user.name)
END QUERY
```

QIR：

```text
Scan(users, binding=user)
Filter(hir:user.active)
Filter(hir:user.age >= 18)
Order(
    hir:user.score DESC,
    hir:user.id ASC
)
Take(hir:10)
Project(hir:User_View(...))
```

---

# 16. Query Optimizer v0.1

Optimizer 保持小而确定。

## 16.1 Pass 1 — Validation

确认 QIR operator 顺序：

```text
Scan
Filter*
Order?
Take?
Project
```

任何其他顺序内部视为 compiler invariant failure。

## 16.2 Pass 2 — Constant TAKE

如果 TAKE 是 compile-time Int constant：

```text
K <= 0
```

直接 Physical Empty Result。

例如：

```basic
TAKE 0
```

无需扫描 source。

## 16.3 Pass 3 — Filter Fusion

逻辑：

```text
Filter A
Filter B
Filter C
```

物理上进入同一个 scan loop。

注意：

```text
fused != reordered
```

执行顺序保持：

```text
A -> B -> C
```

## 16.4 Pass 4 — TAKE Early Exit

无 ORDER：

```text
Scan
Filter*
Take K
Project
```

降为：

```text
FusedScan
  when selected_count == K:
      break
```

如果 K 为运行时值，也可以 early exit。

## 16.5 Pass 5 — ORDER Materialization

有 ORDER，无 TAKE：

```text
Scan
Filter*
Order
Project
```

物理计划：

```text
CandidateCollection
StableSort
Project
```

Projection 尽量放在排序后。

原因：

如果 projection 构造较大 Record，而排序只需要原 row 的小 key：

```text
sort source row reference/index
then project survivors
```

通常更省分配。

## 16.6 Pass 6 — ORDER + TAKE -> Top-K

```text
Order
Take K
```

改为：

```text
TopK(order_keys, K)
```

目标复杂度：

```text
O(N log K)
```

而不是：

```text
O(N log N)
```

内存目标：

```text
O(K)
```

对于 K 接近 N 的情况，可加入 heuristic：

```text
if K is large relative to N:
    full stable sort may be cheaper
```

但 v0.1 可以先统一 Top-K，再通过 benchmark 决定 heuristic。

---

# 17. Physical Plan

建议物理策略只需要四类：

```text
EMPTY
FUSED_SCAN
STABLE_SORT
TOP_K
```

组合：

### A. 无 ORDER

```text
FUSED_SCAN(
    filters,
    take,
    project
)
```

### B. ORDER，无 TAKE

```text
SCAN_FILTER_TO_CANDIDATES
STABLE_SORT
PROJECT
```

### C. ORDER + TAKE

```text
TOP_K_SCAN
FINAL_STABLE_ORDER_OF_K
PROJECT
```

### D. TAKE <= 0 constant

```text
EMPTY
```

无需把 Physical Plan 做成通用数据库 optimizer。

---

# 18. MIR Lowering

QIR / Physical Plan 最终直接生成 MIR CFG。

## 18.1 Fused Scan CFG

概念：

```text
entry
  |
  | source_length
  | i = 0
  | selected = 0
  | result = create
  v
loop_header
  |
  | i < source_length ?
  +------ no ------> finish
  |
 yes
  v
load_item
  |
  v
filter_1
  | false -> increment
  v
filter_2
  | false -> increment
  v
project
  |
append_result
  |
selected++
  |
take_reached?
  | yes -> finish
  v
increment
  |
i++
  |
  +-------------> loop_header

finish
  |
query_result
```

## 18.2 复用现有 MIR expression lowerer

WHERE / ORDER / SELECT 的普通 expression：

```text
Field Read
Integer Compare
String operations
Pure function call
Record construct
```

继续使用现有 HIR -> MIR expression lowering。

不要在 Query lowerer 重写一套 expression compiler。

## 18.3 Source evaluation exactly once

非常重要：

```basic
QUERY x IN make_list()
```

如果未来允许 source 是 pure call，`make_list()` 必须只求值一次。

不能在：

```text
length
get
loop
```

中重复执行 source expression。

Query lowerer 应先：

```text
source_value = lower_expression(source)
```

后续全部使用该 MIR value。

---

# 19. Result List 构造策略

当前 backend 已有内部 runtime list append 路径，因此 v0.1 应优先复用现有 Runtime List append 能力，而不是马上公开新的 Builder 类型。

原则：

```text
BASIC public model:
    immutable List<T>

compiler/runtime internal implementation:
    may mutate newly-created unaliased result list during construction
```

## 19.1 不允许公开 MutableList

Query 不应该迫使语言新增：

```text
MutableList<T>
ListBuilder<T>
```

公共类型。

## 19.2 推荐第一版

增加一个明确的 MIR 层内部 append 能力，或复用当前已有的内部 runtime append emission。

候选：

```text
OM_MIR_LIST_APPEND_INTERNAL
```

特点：

- 只允许 compiler-generated code 使用；
- append target 必须是当前 query 新建、未发布的 list；
- append 完成后普通语言仍看到不可变 List；
- verifier 可限制该 opcode 的来源。

如果当前 `OM_MIR_LIST_CREATE` + backend 内部 append 路径已经能安全表达，则不必新增 opcode。

## 19.3 Builder 优化作为后续性能选择

如果 benchmark 显示逐元素 Runtime append：

- realloc 过多；
- capacity growth 不理想；
- GC rooting 成本偏高；

再引入 compiler-internal：

```text
LIST_BUILDER_CREATE
LIST_BUILDER_APPEND
LIST_BUILDER_FINISH
```

但这不是 v0.1 语义前置条件。

---

# 20. GC 与 Safepoint

Query 可能：

- 扫描 GC-managed List；
- 构造 Record；
- append String / Record reference；
- 调用 Pure Function；
- 排序过程中暂存元素。

因此必须验证：

- source List 在整个 Query 生命周期 rooted；
- current item 在可能分配的 projection call 前保持 live；
- result list rooted；
- candidate storage 中 managed references 可被 GC trace；
- Top-K heap 中 managed references 可被 GC trace；
- stable sort temporary storage 不产生未注册 managed pointer。

禁止：

```text
malloc raw array of managed pointers
```

却不告诉 GC。

如果 Runtime List / managed object 已经是最安全的 candidate storage，v0.1 宁可先使用它，再优化 raw buffer。

---

# 21. ORDER 语义

## 21.1 Stable Sort

语言规范明确：

> `ORDER BY` 必须稳定。

若所有排序 key 相等：

```text
source order preserved
```

例如：

```text
source:
A score=10
B score=10

ORDER BY score DESCENDING

result:
A
B
```

## 21.2 Multi-key

```basic
ORDER BY
    user.score DESCENDING,
    user.id ASCENDING
```

比较顺序：

1. score；
2. 如果 score 相等，比较 id；
3. 如果全部相等，保留原 source position。

## 21.3 Source position tie-break

Top-K 必须与 full stable sort 产生完全一致的结果。

因此 Top-K entry 应保存：

```text
source_index
```

当全部 user keys 相等时：

```text
source_index ascending
```

作为最后 tie-break。

这是实现 stable Top-K 的关键。

---

# 22. Top-K 算法

## 22.1 基本方案

对：

```text
ORDER BY key DESC
TAKE K
```

维护一个大小最多 K 的 heap。

需要保留“当前最差的入选元素”在 heap root，以便新 candidate 快速比较。

每个 entry：

```text
row/reference
order keys or recomputable row
source_index
```

## 22.2 最终输出

Heap 内部顺序不是最终查询顺序。

扫描完成后：

```text
sort K entries using exact query comparator
```

然后 projection。

复杂度：

```text
scan: O(N log K)
final: O(K log K)
memory: O(K)
```

## 22.3 K=1

可以专门优化为：

```text
single best candidate
```

但不是 v0.1 必须。

## 22.4 Runtime K

如果：

```basic
TAKE limit
```

limit 是运行时变量：

- `limit <= 0` -> empty；
- 否则动态分配 K capacity；
- 必须做 size / allocation checked arithmetic；
- 超过语言 / runtime resource limit 时应产生明确 fault / allocation failure policy。

---

# 23. ORDER Key 求值次数

必须冻结。

推荐：

> 每个 candidate 的每个 ORDER key 最多求值一次。

原因：

即使 Function 是 pure：

```basic
ORDER BY expensive_normalize(user.name) ASCENDING
```

排序 comparator 若重复调用该表达式，可能造成：

```text
O(N log N * expression cost)
```

建议 candidate entry 缓存 key。

但这会增加 storage。

v0.1 可分：

### Primitive cheap field key

允许 comparator 直接 field read。

### General pure expression key

建议 precompute once。

长期统一成 key materialization 更容易保证性能。

---

# 24. SELECT 延迟 Projection

对于：

```basic
QUERY user IN users
    ORDER BY user.score DESCENDING
    TAKE 10
    SELECT Huge_View(...)
END QUERY
```

不要先为每个 user 构造 `Huge_View` 再排序。

执行：

```text
scan raw source rows
↓
Top-K raw candidates
↓
final order
↓
project only K rows
```

无 ORDER 时：

```text
filter
↓
project immediately
```

因为无需保存 candidate。

这项优化应当是 v0.1 固定物理语义。

---

# 25. Constant Folding

Query optimizer 不必自己重新实现通用 constant folding。

如果现有 HIR / MIR pipeline 已经会优化：

```text
100 + 20
```

继续复用。

Query-specific constant logic只需要：

```text
TAKE compile-time constant
```

以及 future query operator metadata。

不要建立第二套 arithmetic optimizer。

---

# 26. AST / HIR / QIR Dump

为开发和测试加入可观察输出。

## 26.1 AST dump

Query 示例：

```text
QUERY
  binding user
  source users
  WHERE ...
  ORDER ...
  TAKE ...
  SELECT ...
```

## 26.2 HIR dump

包含：

```text
type ids
symbol ids
effects
query result type
```

## 26.3 QIR debug dump

即使 QIR 是 temporary，也建议提供 debug-only dump：

```text
query #3
  scan %users
  filter hir#123
  filter hir#130
  order hir#145 desc
  take hir#150
  project hir#162
```

## 26.4 Physical plan dump

例如：

```text
QUERY PLAN
  TOP_K k=10
  filters=2
  order_keys=2
  stable=true
  projection=hir#162
```

这对性能回归测试极其有用。

可以由：

```text
--dump-query-plan
```

或已有 compiler dump infrastructure 接入。

若项目不希望新增 CLI flag，测试构建中通过内部 API dump 即可。

---

# 27. Diagnostics 设计

建议用户错误示例。

## 27.1 Source 不是 List

```basic
QUERY c IN "hello"
```

错误：

```text
query source must be List<T>; found String
```

## 27.2 WHERE 不是 Bool

```basic
WHERE user.age
```

错误：

```text
WHERE expression must be Bool; found Int
```

## 27.3 TAKE 不是 Int

```basic
TAKE "10"
```

错误：

```text
TAKE expression must be Int; found String
```

## 27.4 ORDER 不支持

```basic
ORDER BY user.tags ASCENDING
```

如果 `tags : List<String>`：

```text
ORDER BY key type List<String> is not orderable in Query v0.1
```

## 27.5 Effectful function

```basic
WHERE write_log(user.name)
```

错误：

```text
Query expressions must be pure; call has side effects
```

## 27.6 Assign range variable

如果语法层能出现：

```text
user = ...
```

应拒绝：

```text
query range variable 'user' is immutable
```

---

# 28. Parser 测试

必须覆盖：

### Positive

```text
minimal query
one WHERE
multiple WHERE
ORDER one key
ORDER multiple keys
TAKE
ORDER + TAKE
Record projection
multiline projection
query inside RETURN
query inside function call argument if grammar permits
```

### Negative

```text
missing IN
missing SELECT
missing END QUERY
duplicate ORDER
duplicate TAKE
WHERE after SELECT
ORDER after TAKE
TAKE after SELECT
missing ASCENDING/DESCENDING
trailing comma in ORDER
invalid range binding
```

### Recovery

Parser error recovery 必须确保 Query block 中一个错误不会吞掉整个后续 module。

---

# 29. Semantic / Type Tests

覆盖：

```text
List<Int> -> List<Int>
List<Record> -> List<FieldType>
WHERE Bool valid
WHERE Int invalid
TAKE Int valid
TAKE Float invalid
SELECT arbitrary result type
outer lexical capture
record field lookup
pure direct function
pure indirect function if supported
effectful action forbidden
actor SEND impossible/forbidden
actor state read allowed
source evaluated once
```

---

# 30. Runtime / Execution Tests

## 30.1 Filter

输入：

```text
[1,2,3,4,5]
```

Query：

```text
WHERE x > 2
```

结果：

```text
[3,4,5]
```

## 30.2 Multiple filters

确认顺序和 fault behavior。

## 30.3 TAKE

```text
TAKE 0
TAKE -1
TAKE 1
TAKE N
TAKE > length
```

## 30.4 Projection

```text
Int -> String
Record -> field
Record -> Record
```

## 30.5 Order stability

大量重复 key：

```text
key same, source ids increasing
```

结果 id 顺序必须不变。

## 30.6 Multi-key

验证：

```text
primary desc
secondary asc
stable fallback
```

## 30.7 Top-K equivalence

对随机输入：

```text
TopK query result
==
full stable sort then take K
```

建议 property-based testing。

---

# 31. Actor Tests

Query 不需要 Actor Runtime 特殊实现，但必须验证集成。

## 31.1 Query Actor State

Actor state：

```text
List<User>
```

RECEIVE 内 Query 后 SEND result。

## 31.2 Two Actor Isolation

两个 Actor 各持有不同 List，发送相同 Query request，确认结果不串。

## 31.3 Mailbox Serialization

同 Actor 连续收到：

```text
Update
Query
Update
Query
```

结果必须符合 handler 顺序。

Query 不得看到未来 state。

## 31.4 Large Query Responsiveness Documentation

测试不必承诺 Query 自动 yield。

文档必须说明：

> 大型 Query 会占用当前 handler；如需隔离长 CPU 工作，应使用专用 Actor。

---

# 32. GC Tests

需要强制 GC pressure。

场景：

- source 是 List<Record(String, ...)>；
- WHERE 调用 Pure Function 并分配；
- SELECT 构造新 Record；
- result 大于 nursery；
- ORDER candidates 包含 managed String；
- Top-K 中触发 GC。

检查：

```text
no use-after-free
no missing roots
no corrupted list
no stale managed pointers
```

ASan + GC stress 双覆盖。

---

# 33. Sanitizer 测试

## 33.1 ASan / UBSan

重点：

- result construction；
- order temporary storage；
- Top-K heap；
- source index arithmetic；
- capacity growth；
- comparator；
- GC interop。

## 33.2 TSan

Query 自身不并行，但在 Actor 环境下验证：

- multiple actors query concurrently；
- runtime list internals；
- no shared temporary global state；
- debug query dump 不使用 unsafe global mutable buffer。

QIR optimizer 必须是 compiler-side ordinary state，不影响 runtime TSan。

---

# 34. Fuzzing

建议增加：

```text
query_parser_fuzzer
```

输入任意 source bytes，验证：

```text
no crash
no infinite parse
bounded diagnostics
```

也可增加 compiler semantic fuzz：

生成结构化 Query AST：

```text
random List element types
random WHERE expressions
random ORDER keys
random TAKE
random SELECT
```

目标：

```text
compiler never crashes
diagnostics stable
```

---

# 35. Performance Benchmark

Query 的存在必须有明确性能目标。

## 35.1 Baseline

手写 OMBasic：

```basic
FOR user IN users
    IF ...
        ...
    END IF
END FOR
```

Query：

```basic
QUERY user IN users
    WHERE ...
    SELECT ...
END QUERY
```

## 35.2 Benchmark Cases

### Case A — filter only

```text
N = 1K / 100K / 10M
selectivity = 10% / 50% / 90%
```

### Case B — two filters + projection

### Case C — TAKE 10 early exit

### Case D — stable sort all

### Case E — ORDER + TAKE 10

### Case F — ORDER + TAKE 1000

### Case G — large String sort keys

## 35.3 Acceptance Targets

无 ORDER Query：

```text
runtime <= 1.15x equivalent optimized hand-written loop
```

目标最好接近：

```text
1.00x - 1.05x
```

不应存在与 operator 数量成比例的 iterator allocation。

内存：

```text
no intermediate List per WHERE/SELECT operator
```

ORDER + TAKE：

必须证明在：

```text
N >> K
```

时显著优于 full sort + slice。

例如：

```text
N = 1,000,000
K = 10
```

Top-K 应有明显时间和 peak memory 优势。

---

# 36. LLVM 检查

为核心 benchmark 增加 IR inspection 或 debug dump 检查。

无 ORDER Query 应看到近似：

```text
single loop
conditional branches
result append
```

不应看到：

```text
call filter
call filter
call select
iterator_next
query_node_eval
```

即：

> Query abstraction should disappear before code generation.

---

# 37. Formatter / Tooling

如果仓库已有 formatter / tooling token tables，需要同步新增关键字：

```text
QUERY
WHERE
ORDER
BY
TAKE
ASCENDING
DESCENDING
```

必须检查：

- syntax highlighter；
- binding generator reserved words；
- LSP tokenization；
- formatter；
- docs syntax examples；
- keyword validation。

所有生成绑定工具中的保留词集合必须同步，否则外部 binding 可能生成与 Query keyword 冲突的名称。

---

# 38. 文档更新

至少新增：

```text
docs/language/query.md
```

内容：

1. Query 是什么；
2. Syntax；
3. Eager semantics；
4. Pure semantics；
5. Actor integration；
6. Performance model；
7. Stable ordering；
8. TAKE；
9. Limitations；
10. Examples。

并更新：

```text
OMBasic-Language-Standard.md
docs/development/implementation_status.md
docs/development/diagnostics.md
```

如果项目使用 ADR，建议：

```text
ADR-00xx-query-expression-and-in-memory-query-lowering.md
```

---

# 39. 推荐 ADR 决策

ADR 至少冻结：

## Decision 1

Query v0.1 是：

```text
eager
```

不是 lazy。

## Decision 2

source 仅：

```text
List<T>
```

## Decision 3

Query 内只允许 Pure expression。

## Decision 4

Query 不具有 Actor / async / scheduling semantics。

## Decision 5

QIR 是 HIR -> MIR 中间 temporary plan。

## Decision 6

QIR 不 Lower 回 AST。

## Decision 7

ORDER stable。

## Decision 8

ORDER + TAKE 使用 Top-K physical plan。

## Decision 9

Query 默认不并行。

## Decision 10

SQL / database 不属于 v0.1。

---

# 40. 文件改动建议

实际路径需以实施时仓库结构为准，预计涉及：

## Lexer / Syntax

```text
include/ombasic/syntax/...
src/syntax/lexer/...
src/syntax/parser/...
include/ombasic/syntax/ast.h
```

新增 token / AST kinds / parser logic。

## Name Resolution / Type

```text
src/semantic/name_resolver/
src/semantic/type_inference/
src/semantic/effect/
```

增加 Query scope、binding、type、effect rules。

## HIR

```text
include/ombasic/ir/hir.h
src/ir/hir/
```

新增 HIR Query kinds、lowering、verification、dump。

## MIR Lowering

```text
src/ir/mir/lower_hir.c
```

或拆：

```text
src/ir/mir/lower_query.c
```

更推荐拆文件，避免 `lower_hir.c` 继续膨胀。

## Temporary QIR

建议首版：

```text
src/ir/mir/query_plan.c
src/ir/mir/query_plan.h
```

作为 lowering internal module。

未来再移动到：

```text
src/ir/query/
```

## MIR / Backend

可能需要：

```text
include/ombasic/ir/mir.h
src/backend/llvm/control_flow.c
src/backend/llvm/runtime_calls.c
```

用于内部 List append / sorting / Top-K helper。

## Runtime

如果实现通用 helper：

```text
src/runtime/collections/
src/runtime/query/
```

但不要创建用户可见 Query Runtime object。

---

# 41. Runtime Helper 策略

推荐最小化 Runtime 新增。

## 必要候选

```text
stable_sort
top_k
internal list append
```

是否全部 Runtime 实现取决于 MIR 表达成本。

### Fused scan

必须由 compiler 生成 MIR / LLVM。

不要 Runtime：

```c
runtime_query_filter(...)
```

否则失去 specialization / inlining 优势。

### Stable sort / Top-K

可以使用 Runtime generic helper，但要考虑 element representation 与 comparator callback 成本。

更高性能路线：

- compiler 生成 comparator；
- runtime 提供 algorithm skeleton；
- comparator 为 specialized native function。

或者 compiler 生成全部逻辑。

v0.1 建议先以正确性和明确 ABI 为主，再 benchmark。

---

# 42. Comparator 设计

ORDER 是实现难点之一。

## 42.1 不开放用户 comparator

v0.1 禁止：

```basic
ORDER BY user USING compare_user
```

## 42.2 Compiler-generated comparator

Compiler 根据 order keys 生成 specialized comparator。

概念：

```text
compare(a, b):
    compare key1
    if unequal return
    compare key2
    if unequal return
    compare source_index
```

这样：

- multi-key 一次编码；
- stable tie-break 明确；
- 不需要动态 comparator object。

## 42.3 String Comparison

必须复用语言现有 String ordering。

Query 不定义独立 String collation。

不要首版引入：

```text
locale
case-insensitive collation
Unicode collation
database collation
```

---

# 43. Fault Semantics

Query optimization 不得改变语言可观察 fault 顺序，除明确被规范允许。

## 无 ORDER

保持：

```text
source order
WHERE source order
SELECT when row survives
```

## ORDER

ORDER key evaluation timing需要规范。

推荐：

```text
for every row surviving WHERE:
    evaluate ORDER keys
```

然后排序。

SELECT 在最终顺序确定后执行。

因此：

```text
ORDER key fault occurs before any SELECT projection for ordered queries
```

这需要写进规范，避免未来 optimizer 改变行为。

---

# 44. Determinism

相同输入与相同外部 lexical values：

```text
same Query
=> same ordered result
```

ORDER stable 是 deterministic 的关键。

无 ORDER：

```text
preserve source order
```

TAKE：

```text
take first surviving rows in source order
```

Top-K：

结果必须与：

```text
stable full sort + TAKE K
```

bit-for-bit / value-for-value 等价。

---

# 45. Empty Result 类型问题

当前语言曾存在 empty List contextual typing 的限制。

Query 不应依赖源码表达：

```basic
[]
```

来构造空结果。

因为 Query 的 SELECT 类型已知：

```text
R
```

所以 compiler internal result construction 应直接知道：

```text
List<R>
```

这也是专门 Query lowering 的优势。

因此：

```basic
TAKE 0
```

或所有 rows 被 filter 掉时，仍能生成：

```text
empty List<R>
```

无需用户写 typed empty List。

---

# 46. Query 与 Collections 的边界

Query 不是 Collections API 的替代品。

Collections 继续负责：

```text
length
contains
index_of
keys
get_or
...
```

Query 负责：

```text
declarative selection
filtering
ordering
projection
limited result
```

不要为了所有 List 操作不断增加 Query keyword。

未来是否加入：

```text
DISTINCT
SKIP
```

必须看它们是否属于高价值关系型 operator，而不是仅因为 LINQ 有。

---

# 47. Query 与 Function Values

v0.1 Query clause 允许调用 Pure Function：

```basic
WHERE is_adult(user)
```

但 compiler 不应把整个 WHERE 自动转换成 function pointer。

它直接 Lower 原 HIR expression。

这样避免：

```text
indirect call
closure allocation
predicate object
```

如果原源码明确通过 Function value 间接调用，则保留现有语言语义和开销。

---

# 48. Query 与 Generic Specialization

Query 所在 Function 若是 generic specialization：

```text
query lowering happens after types are concrete enough
```

或者至少 QIR reference 的 HIR expression 已经有 TypeId。

必须验证：

- generic record element；
- projection generic helper；
- generic pure function call；
- List<T> source specialization。

不要让 QIR 自己做第二次泛型实例化。

---

# 49. 实施阶段

## Phase 1 — Syntax Only

目标：

```text
parse + AST dump
```

任务：

- [x] lexer keywords；
- [x] parser Query expression；
- [x] AST kinds；
- [x] clause order diagnostics；
- [x] parser tests；
- [x] formatter / tooling keyword updates。

完成标准：

```basic
QUERY ...
END QUERY
```

可稳定 parse / dump，但尚不能 codegen。

---

## Phase 2 — Semantic + Typed HIR

任务：

- [x] Query-local range symbol；
- [x] List<T> source checking；
- [x] WHERE Bool；
- [x] TAKE Int；
- [x] ORDER key type；
- [x] SELECT result；
- [x] effect restrictions；
- [x] HIR nodes；
- [x] HIR verifier；
- [x] HIR dump；
- [x] diagnostics tests。

完成标准：

正确 Query：

```text
fully typed HIR
```

错误 Query：

```text
high-quality diagnostics
```

---

## Phase 3 — Temporary QIR

任务：

- [x] Query Plan structs；
- [x] HIR Query -> QIR；
- [x] QIR verifier；
- [x] debug dump；
- [x] physical plan selection；
- [x] no AST round-trip。

完成标准：

每个合法 Query 都能打印 deterministic logical / physical plan。

---

## Phase 4 — Scan / WHERE / SELECT

暂不支持 ORDER / TAKE codegen。

任务：

- [x] source evaluate once；
- [x] List length/get；
- [x] loop CFG；
- [x] multiple filters；
- [x] projection；
- [x] internal result append；
- [x] result `List<R>`；
- [x] GC roots；
- [x] execution tests。

完成标准：

```text
QUERY + WHERE* + SELECT
```

产生正确 native binary。

---

## Phase 5 — Loop Fusion + TAKE

任务：

- [x] filters fused；
- [x] no intermediate Lists；
- [x] TAKE early exit；
- [x] TAKE <= 0；
- [x] outer lexical values；
- [x] benchmark against hand loop；
- [x] LLVM inspection tests。

完成标准：

无 ORDER query 接近手写 loop 性能。

---

## Phase 6 — Stable ORDER + Top-K

任务：

- [x] order key type rules；
- [x] multi-key comparator；
- [x] source index stability；
- [x] full stable sort；
- [x] ORDER + TAKE detection；
- [x] Top-K heap；
- [x] final K stable sort；
- [x] projection delay；
- [x] GC-safe candidates；
- [x] property equivalence tests；
- [x] performance benchmarks。

完成标准：

```text
ORDER
ORDER + TAKE
```

正确、稳定、可预测，并且 `N >> K` 时 Top-K 明显优于 full sort。

---

# 50. 推荐 PR 拆分

## PR 1

```text
syntax: add query expression grammar
```

## PR 2

```text
semantic: type and effect check query expressions
```

## PR 3

```text
hir: add typed query representation
```

## PR 4

```text
mir: add temporary query plan and fused list scan lowering
```

## PR 5

```text
query: add take early-exit and performance benchmarks
```

## PR 6

```text
query: add stable order lowering
```

## PR 7

```text
query: optimize order plus take with top-k
```

## PR 8

```text
docs: freeze query v0.1 semantics and actor integration
```

也可以把 docs 分散随各 PR 同步更新；不建议最后一次性补文档。

---

# 51. CI Gates

每个 PR 至少通过：

```text
Linux Debug
Linux Release
Windows Release
unit tests
parser tests
semantic tests
HIR verifier
MIR verifier
conformance
```

涉及 runtime list / sort / Top-K 后增加：

```text
ASan + UBSan
TSan
GC stress
```

最终 Query v0.1 合并 gate：

```text
all query conformance
all existing language conformance
no actor regression
no collections regression
Release benchmark sanity
```

---

# 52. Performance Regression Gate

建议加入非严格时间型结构检查，减少 CI 噪声。

例如 QIR / physical-plan tests：

```text
WHERE + WHERE + SELECT
=> FUSED_SCAN
```

```text
ORDER + TAKE
=> TOP_K
```

```text
TAKE 0
=> EMPTY
```

以及 MIR dump：

```text
no intermediate list nodes between filters
```

实际 wall-clock benchmark 可以 nightly / manual 跑，不一定阻塞每次 CI。

---

# 53. Security / Robustness

虽然 Query 是内存计算，也需要防止：

- size arithmetic overflow；
- K 转 capacity overflow；
- malicious huge TAKE；
- comparator inconsistency bug；
- Top-K heap out-of-bounds；
- allocation failure；
- recursion via pure helper；
- large String key memory amplification。

所有 allocation size 计算使用 checked arithmetic。

Compiler-generated internal operations不得通过 assertion 处理用户可触发错误。

---

# 54. Source Compatibility

新增 uppercase keyword 可能影响已有用户标识符。

需要搜索：

```text
QUERY
WHERE
ORDER
BY
TAKE
ASCENDING
DESCENDING
```

是否当前可作为 identifier 使用。

如果 OMBasic keyword 大小写严格，并且普通命名风格是 lower_snake_case，实际冲突风险较低。

仍需要：

- language standard changelog；
- binding generator reserved names；
- migration note。

---

# 55. Future SQL Compatibility

v0.1 不实现 SQL，但设计不得阻塞未来。

当前 QIR operator：

```text
Scan
Filter
Order
Take
Project
```

未来天然可映射：

```text
Scan    -> FROM
Filter  -> WHERE
Order   -> ORDER BY
Take    -> LIMIT / FETCH
Project -> SELECT
```

因此 v0.1 的 temporary QIR shape 有未来价值。

但不要现在加入：

```text
Table<T>
ColumnRef
SQL Parameter
Provider
Expression Tree
```

等数据库阶段再引入。

---

# 56. Future Actor + Database Compatibility

未来数据库查询正确模型应是：

```text
pure query description
       ↓
Database Actor request
       ↓
SQL execution
       ↓
Completion Message
```

而不是：

```basic
rows = QUERY row IN remote_database
```

并在表达式内部隐式 I/O。

因此必须长期保持：

> Query syntax ≠ implicit asynchronous execution.

v0.1 的 `List<T>` eager Query 与该原则完全兼容。

---

# 57. Definition of Done

Query v0.1 只有在以下全部成立时完成。

## Syntax

- [x] `QUERY x IN list`；
- [x] `WHERE*`；
- [x] `ORDER BY`；
- [x] `TAKE`；
- [x] `SELECT`；
- [x] `END QUERY`；
- [x] formatter / reserved keyword updates。

## Semantics

- [x] source 仅 List；
- [x] typed range variable；
- [x] WHERE Bool；
- [x] TAKE Int；
- [x] ORDER typed；
- [x] SELECT result typed；
- [x] Query Pure；
- [x] eager；
- [x] synchronous；
- [x] non-yielding。

## Compiler

- [x] AST；
- [x] semantic；
- [x] HIR；
- [x] temporary QIR；
- [x] QIR verifier；
- [x] physical plan；
- [x] MIR lowering；
- [x] no QIR -> AST；
- [x] LLVM codegen。

## Performance

- [x] Filter fusion；
- [x] no intermediate List per clause；
- [x] TAKE early exit；
- [x] stable sort；
- [x] Top-K；
- [x] projection delay after ordered selection；
- [x] source expression evaluated once。

## Actor

- [x] Query works inside RECEIVE；
- [x] no Scheduler modifications；
- [x] no implicit parallelism；
- [x] no ActorRef source；
- [x] docs explain long-query responsiveness。

## Quality

- [x] Linux；
- [x] Windows；
- [x] ASan / UBSan；
- [x] TSan；
- [x] GC stress；
- [x] parser fuzz；
- [x] conformance；
- [x] benchmarks；
- [x] docs；
- [x] ADR。

---

# 58. 最终推荐架构

最终 v0.1 架构：

```text
                        OMBasic Source
                              │
                              ▼
                        ┌───────────┐
                        │    AST    │
                        └─────┬─────┘
                              │
                       semantic/type/effect
                              │
                              ▼
                        ┌───────────┐
                        │    HIR    │
                        │  typed    │
                        │ resolved  │
                        └─────┬─────┘
                              │
                         query node only
                              │
                              ▼
                     ┌─────────────────┐
                     │ Temporary QIR   │
                     │ Scan            │
                     │ Filter          │
                     │ Order           │
                     │ Take            │
                     │ Project         │
                     └────────┬────────┘
                              │
                         optimize
                              │
              ┌───────────────┼──────────────┐
              │               │              │
              ▼               ▼              ▼
         Fused Scan       Stable Sort      Top-K
              │               │              │
              └───────────────┼──────────────┘
                              │
                              ▼
                        ┌───────────┐
                        │    MIR    │
                        │ CFG/SSA   │
                        └─────┬─────┘
                              │
                              ▼
                            LLVM
                              │
                              ▼
                         Native Code
```

Actor 路径：

```text
Message
   ↓
Actor RECEIVE
   ↓
synchronous QUERY
   ↓
List<R>
   ↓
SEND result
```

Query 永远不成为：

```text
scheduler
task
future
promise
iterator runtime
actor runtime extension
```

---

# 59. 最重要的实现原则

整个开发过程中优先保护以下十条：

1. **QIR 不回 AST。**
2. **Query 不修改 Actor Scheduler。**
3. **Query 不引入 async/lazy runtime。**
4. **Query 默认不并行。**
5. **Query expression 必须 Pure。**
6. **无 ORDER 时生成 fused loop。**
7. **ORDER 必须 stable。**
8. **ORDER + TAKE 必须具备 Top-K 路线。**
9. **SELECT 在 ordered query 中尽量延迟到最终入选结果。**
10. **Query abstraction 在最终 native code 中应基本消失。**

---

# 60. 建议首个实现里程碑

最先实现一个最小闭环：

```basic
RECORD User(id, age, active)

FUNCTION active_adults(users)
    RETURN QUERY user IN users
        WHERE user.active
        WHERE user.age >= 18
        SELECT user.id
    END QUERY
END FUNCTION
```

要求：

```text
Parser ✓
Type inference ✓
HIR ✓
QIR ✓
Fused MIR loop ✓
Native AOT ✓
List<Int> result ✓
No intermediate List ✓
```

然后第二个闭环：

```basic
RETURN QUERY user IN users
    WHERE user.active
    TAKE 10
    SELECT user
END QUERY
```

确认 early exit。

最后第三个闭环：

```basic
RETURN QUERY user IN users
    WHERE user.active
    ORDER BY user.score DESCENDING
    TAKE 10
    SELECT user
END QUERY
```

确认：

```text
Top-K
stable result
GC safety
benchmark advantage
```

完成这三个闭环后，Query v0.1 的核心架构就基本成立。

---

# 61. 当前仓库基线说明

本计划基于 OMBasic 当前 `master` 的编译器架构：

```text
AST
→ semantic/type inference
→ HIR
→ MIR
→ LLVM
```

当前 HIR 已携带：

```text
source node
source span
type id
symbol id
effect metadata
```

当前 MIR 已提供：

```text
CFG blocks
block parameters
conditional branches
List create
List length
List get
field load
compare
direct/indirect call
```

LLVM backend 当前也已经存在内部 Runtime List append 路径，因此 Query v0.1 应优先利用现有 List 构造能力，再根据 benchmark 判断是否值得引入专用 compiler-internal List Builder。

本计划中的 Query AST / HIR / MIR enum 名称属于建议，实际命名应遵循仓库现有 C 命名约定与诊断编号分配规则。

---

# 62. 参考

- OMBasic repository  
  https://github.com/risol/OMBasic

- OMBasic Language Standard  
  https://github.com/risol/OMBasic/blob/master/OMBasic-Language-Standard.md

- HIR interface  
  https://github.com/risol/OMBasic/blob/master/include/ombasic/ir/hir.h

- MIR interface  
  https://github.com/risol/OMBasic/blob/master/include/ombasic/ir/mir.h

- LLVM backend control flow  
  https://github.com/risol/OMBasic/blob/master/src/backend/llvm/control_flow.c

---

## 结论

OMBasic Query v0.1 应被实现成：

> **一个由编译器完全理解、完全静态类型、同步、纯、eager 的 List 查询表达式。**

它不是 LINQ Runtime，不是 SQL Runtime，也不是 Actor 扩展。

它的核心价值是：

```text
declarative source syntax
        +
typed compiler IR
        +
whole-query optimization
        +
native AOT
```

让普通：

```basic
QUERY
    WHERE
    ORDER BY
    TAKE
    SELECT
END QUERY
```

最终变成：

```text
fused loop
stable sort
or Top-K
```

而不是一层层运行时 iterator。

这应当是 Query v0.1 的唯一成功标准。
