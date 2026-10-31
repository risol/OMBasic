# OMBasic Immutable Collections + Internal Transient Storage 完整开发计划

> **文档类型：** Language Semantics / Compiler / Runtime / GC / Query Optimization Development Plan<br>
> **版本：** 1.0<br>
> **日期：** 2026-10-31<br>
> **项目：** OMBasic<br>
> **仓库基线：** `master`；Query v0.1 已完成<br>
> **核心主题：** 保持用户层不可变集合语义，在 Compiler/Runtime 内部引入 transient mutable storage、builder、uniqueness、copy-on-write 与 storage reuse<br>
> **关键约束：** No Async Functions / Actor-only asynchronous interaction<br>
> **公开语言结论：** 不新增用户可见 `Array<T>`

---

# 1. 执行摘要

OMBasic 的公开数据哲学继续保持：

```text
List<T>     immutable
Map<K,V>    immutable
String      immutable
Buffer      immutable
Record      value-oriented
```

Actor 模型保持：

```text
FUNCTION = 纯计算
ACTION   = 显式同步副作用
ACTOR    = 并发状态所有者
MESSAGE  = Actor 间唯一用户级异步交互载体
```

本计划明确**不引入**：

```text
public Array<T>
public MutableList<T>
public MutableMap<K,V>
public ListBuilder<T>
public BufferBuilder
public StringBuilder
lock
mutex
shared mutable memory
async/await
Future/Promise
```

而是在 Compiler / Runtime 内部引入：

```text
Mutable backing array
Transient builder
Unique-storage detection
Copy-on-write
Last-use optimization
Storage stealing / reuse
Bulk update path
Internal mutable scratch storage
```

核心原则：

> **不可变性是 OMBasic 的用户可观察语义，不是 Runtime 必须每次复制整份数据的实现要求。**

用户仍然写：

```basic
result = QUERY item IN items
    WHERE item.active
    SELECT item.name
END QUERY
```

公开语义仍然是：

```text
items  : List<Item>
result : List<String>
```

Compiler/Runtime 内部则可以：

```text
allocate transient storage
append in place
grow capacity
seal
publish as immutable List<String>
```

用户永远看不到：

```text
Array
capacity
pointer
builder state
mutable slot
ownership bit
```

---

# 2. 本计划取代的旧方向

以下方向明确废弃，不再作为实施依据：

```text
public Array<T>
ACTION directly mutating Array<T>
RECEIVE directly mutating Array<T>
Actor state storing public Array<T>
QUERY ... IN Array<T>
READS_LOCAL_MUTABLE as a user-visible language effect
array_snapshot(...)
```

原因：

1. 会把 Runtime 实现细节提升为语言概念；
2. 会扩大 FUNCTION / ACTION / Actor 的公开语义边界；
3. 会给 Query 增加不必要的 source 类型复杂度；
4. 会让用户理解 ownership / local mutable 的成本上升；
5. 会削弱 OMBasic 当前不可变值模型的一致性；
6. 很多性能目标可以通过内部 transient mutation / uniqueness 获得；
7. Actor 已经提供很强的 ownership 信息，可以供 optimizer 使用，而无需公开 mutable collection。

---

# 3. 最终公开模型

用户层：

```text
Int
Float
Bool
String
Buffer
Record
List<T>
Map<K,V>
Result<T,E>
```

Compiler / Runtime 内部：

```text
List backing storage
Map construction storage
String builder storage
Buffer builder storage
Query result builder
Transient arrays
Unique ownership metadata
Share state
Capacity
Mutable construction state
```

分层：

```text
┌────────────────────────────────────┐
│          OMBasic Source            │
│                                    │
│ List<T> / Map<K,V> / String        │
│ immutable value semantics          │
└────────────────┬───────────────────┘
                 │
                 ▼
┌────────────────────────────────────┐
│       Compiler / Runtime IR        │
│                                    │
│ unique? transient? published?      │
│ last-use? shared?                  │
└────────────────┬───────────────────┘
                 │
                 ▼
┌────────────────────────────────────┐
│       Internal Storage Layer       │
│                                    │
│ mutable backing arrays             │
│ builders / reserve / append        │
│ copy-on-write / steal / reuse      │
└────────────────────────────────────┘
```

---

# 4. 语义与实现必须分离

语言规范只保证：

```text
List is immutable
Map is immutable
String is immutable
Buffer is immutable
```

语言规范不保证：

```text
每次更新一定分配新 backing store
每次 concat 一定复制全部元素
每个 List value 都有独立 backing storage
Query result 每 append 一定创建新 List
```

只要用户观察不到差异，Runtime 可以：

```text
share
reuse
mutate transient storage
steal storage
copy lazily
```

---

# 5. 可观察不可变性

示例：

```basic
a = [1, 2, 3]
b = list_set(a, 1, 9)
```

必须满足：

```text
a == [1, 2, 3]
b == [1, 9, 3]
```

用户不能观察：

```text
a 的 backing storage 是否被复制
b 是否复用了旧 storage
a 是否在此之后已 dead
```

如果 compiler 证明：

```text
a 在 list_set 后不可再观察
```

那么内部可以：

```text
mutate a.storage[1] = 9
return same storage as b
```

公开语义仍然完全 immutable。

---

# 6. 三层优化模型

按风险从低到高实施。

## Layer 1 — Construction-only Transient Mutation

仅对尚未发布的新值使用 mutable storage：

```text
Query result
List construction
map/filter result
JSON parser output
String formatting output
Buffer encoding output
```

特点：

```text
no alias yet
not externally observable
lowest risk
high immediate value
```

这是第一优先级。

## Layer 2 — Runtime Uniqueness / Copy-on-Write

已存在 immutable value 在更新时：

```text
unique storage -> reuse/mutate
shared storage -> copy then mutate
```

用户仍看到 immutable semantics。

## Layer 3 — Compiler Last-Use / Consume Optimization

Compiler 在 MIR/SSA 层证明：

```text
old value has no future observable use
```

于是允许：

```text
consume + mutate + reuse
```

这是性能最高、也最需要 verifier 的阶段。

---

# 7. Internal Builder 状态机

建议 Runtime 内部统一概念：

```text
CREATED
   ↓
MUTABLE
   ↓
SEALED
```

允许：

```text
CREATED/MUTABLE:
    reserve
    append
    set internal slot
    grow
```

禁止：

```text
SEALED:
    append
    mutable set
    capacity mutation
```

Debug Runtime 对非法状态转换进行验证。

---

# 8. Builder 不进入用户类型系统

不要新增用户类型：

```text
ListBuilder<T>
BufferBuilder
StringBuilder
```

Compiler IR / Runtime C API 可以有：

```text
Om_Runtime_List_Builder
Om_Transient_List
Om_Runtime_Buffer_Builder
Om_Runtime_String_Builder
```

但：

```text
parser 不认识
name resolver 不认识
source type syntax 不认识
Message schema 不认识
Native .omi 不认识
```

---

# 9. List Runtime Representation

建议把 List 的逻辑 value 与 storage 分开：

```text
List Value
    ↓
Backing Storage
```

概念：

```text
ListValue {
    storage
    length
}

ListStorage {
    capacity
    element_count
    state/share metadata
    elements[]
}
```

v1 不需要公开：

```text
slice
offset
borrow
```

---

# 10. Transient vs Published

内部 storage 明确区分：

```text
TRANSIENT
PUBLISHED
```

规则：

```text
TRANSIENT:
    mutable
    one internal owner
    not visible to user

PUBLISHED:
    immutable
    normal List semantics
```

进一步可以：

```text
TRANSIENT
   ↓ seal
PUBLISHED_UNIQUE
   ↓ share/publish
PUBLISHED_SHARED
```

不要把：

```text
unique
```

和：

```text
transient
```

混为一谈。

---

# 11. Query v0.1 公开语义保持不变

Query 继续：

```basic
result = QUERY item IN items
    WHERE ...
    ORDER BY ...
    TAKE ...
    SELECT ...
END QUERY
```

公开规则继续：

```text
source = List<T>
result = List<R>
eager
synchronous
non-yielding
side-effect-free
```

不新增：

```text
QUERY ARRAY
MUTABLE QUERY
QUERY BUILDER
QUERY SNAPSHOT
```

---

# 12. Query 的内部优化目标

Query 优化重点：

```text
1. source List direct scan
2. no iterator object
3. no intermediate Lists
4. internal transient result builder
5. capacity planning
6. append in place before seal
7. Top-K candidate transient storage
8. stable sort over internal mutable candidate storage
9. seal final immutable List<R>
```

Query 不需要 public `Array<T>`。

---

# 13. Query Result Builder

Query result 构造正式化为内部 builder contract：

```text
create transient list storage
for each surviving row:
    project
    append in place
seal once
return immutable List<R>
```

禁止退化为：

```text
List concat
↓
new List
↓
List concat
↓
new List
```

---

# 14. Query Fused Scan

无 ORDER：

```text
Scan
Filter*
Take?
Project
```

物理：

```text
source length once
source get by index
filters fused
projection
builder append
early stop on TAKE
seal
```

目标：

```text
zero intermediate List
zero iterator object
one result-storage growth chain
```

---

# 15. Query ORDER

有 ORDER：

```text
Scan
Filter*
Order
Take?
Project
```

候选使用内部 transient storage：

```text
candidate {
    source_index
    key(s)
    source row/value
}
```

排序允许：

```text
mutable internal stable sort
```

因为 candidate storage 从未暴露为 public value。

---

# 16. Query Top-K

`ORDER BY ... TAKE K`：

```text
bounded transient heap
O(N log K)
O(K) storage
```

最后：

```text
stable-order selected K
project
append into final builder
seal
```

heap mutation 完全属于 Compiler/Runtime 内部。

---

# 17. Query Source 为什么继续只用 List

即使 Runtime 的 List backing storage 实际是 contiguous array：

```text
QUERY source type
    remains List<T>
```

Compiler physical lowering可直接：

```text
read storage[index]
```

无需把 `Array<T>` 暴露给用户。

优势：

```text
Query syntax稳定
type system简单
API稳定
未来 SQL backend更容易统一
```

---

# 18. Immutable List Update API

为了支持 TUI / Editor /算法，同时保持不可变语义，后续建议增加 pure API：

```text
list_set
set_range
fill_range
replace_range
append
prepend
concat
insert
remove_at
```

具体名称按 stdlib 命名规范冻结。

公开语义始终：

```text
old List unchanged
new List returned
```

---

# 19. list_set 的实现策略

概念：

```basic
next = list_set(current, index, value)
```

必须保证：

```text
current remains logically unchanged
next contains replacement
```

内部可：

```text
if storage is consumable:
    mutate/reuse
else:
    copy + mutate
```

用户不可观察。

---

# 20. 为什么不能只看 refcount == 1

`refcount == 1` 不一定代表：

```text
old value no longer observable
```

可能还有：

```text
stack alias
SSA alias
record embedding
Message publish
Native borrow
GC root
```

因此安全 consume 需要明确 contract，而不是简单 refcount hack。

---

# 21. Uniqueness 定义

内部概念：

> **Unique/Consumable Value：Compiler/Runtime 能证明当前 backing storage 没有其他可观察别名，且当前操作可以消费旧 value。**

条件至少：

```text
SSA last-use
+
storage uniqueness
+
no escape
+
no published alias
```

---

# 22. COPY vs CONSUME

建议 MIR / Runtime 区分：

```text
LIST_SET_COPY
LIST_SET_CONSUME
```

语义：

```text
LIST_SET_COPY:
    preserve old observable value

LIST_SET_CONSUME:
    old value is dead after operation
    storage may be reused
```

Source language不暴露这种区别。

---

# 23. SSA Last-Use

MIR 是 SSA-like CFG，适合做：

```text
last-use analysis
```

例如：

```basic
cells = list_set(cells, i, new_cell)
```

如果旧 `cells` SSA value：

```text
after this operation has no other observable use
```

则成为 consume candidate。

但仍需检查 storage 是否被其他 value 共享。

---

# 24. Actor State 为什么特别适合优化

Actor state：

```text
single logical owner
handlers never overlap
```

用户仍然存：

```text
List<Cell>
```

例如：

```basic
RECEIVE Set_Cell(index, cell)
    cells = list_set(cells, index, cell)
END RECEIVE
```

如果旧 `cells` 没有已发布 alias：

```text
runtime/compiler may reuse backing storage
```

而用户语义仍然是 immutable List replacement。

---

# 25. Actor Boundary

一旦：

```basic
SEND renderer, Frame(cells)
```

必须保守认为：

```text
cells storage may be shared/published
```

后续：

```basic
cells = list_set(cells, ...)
```

不能直接覆盖与已发送 Message 共享的 storage。

最保守策略：

```text
SEND => storage shared/published
future update => COW
```

---

# 26. Message Transfer 与 Sharing

如果 Runtime 让 immutable List 跨 Actor共享 backing storage，这是安全的，前提：

```text
published storage never mutates unless COW/consume proof proves no observer
```

因此 Message transfer需要和 uniqueness metadata集成。

---

# 27. Copy-on-Write

Runtime storage 可维护：

```text
UNIQUE
SHARED
```

或等价 metadata。

更新：

```text
UNIQUE + consumable
    -> reuse

SHARED
    -> allocate copy
       mutate copy
       publish new immutable value
```

GC 负责生命周期，COW metadata只负责 mutation safety。

---

# 28. Runtime Defensive Fallback

即使 MIR 发出：

```text
CONSUME
```

Runtime 也建议防御：

```text
if storage is uniquely reusable:
    mutate/reuse
else:
    copy
```

Debug 记录：

```text
consume_fallback
```

避免 compiler proof bug破坏 immutable semantics。

---

# 29. Map 策略

Map 不应第一阶段就做 aggressive COW。

先只做：

```text
mutable construction builder
seal immutable Map
```

原因：

```text
hash invariants
bucket relocation
growth
ordering semantics
```

更复杂。

---

# 30. String 策略

String 对用户继续 immutable。

内部：

```text
StringBuilder
UTF-8 byte storage
reserve
append
seal
```

用于：

```text
interpolation
join
replace
JSON stringify
HTTP formatting
URI formatting
diagnostics formatting
```

不新增 public `StringBuilder`。

---

# 31. Buffer 策略

Buffer 对用户继续 immutable。

内部：

```text
BufferBuilder
mutable byte storage
reserve
append byte
append Buffer
seal
```

用于：

```text
encoding
compression
terminal command construction
network packet construction
binary file assembly
```

---

# 32. Builder 统一原则

内部可以有：

```text
TransientList
TransientMap
TransientBuffer
TransientString
```

共享设计原则：

```text
reserve
grow
seal
state verification
checked size arithmetic
GC root protocol
```

但不强制共享同一个底层 C struct。

---

# 33. HIR

HIR 应继续表达公开不可变语义。

不要为了内部优化加入用户语义节点：

```text
OM_HIR_MUTABLE_ARRAY
OM_HIR_PUBLIC_BUILDER
```

Query HIR继续保持现有结构。

---

# 34. QIR

QIR 保持逻辑层：

```text
SCAN
FILTER
ORDER
TAKE
PROJECT
```

可在 physical planning信息中记录：

```text
result_storage_strategy
candidate_storage_strategy
```

但不要污染逻辑 Query operator。

---

# 35. MIR

Transient / consume / reuse 最适合显式出现在 MIR。

建议：

```text
OM_MIR_LIST_BUILDER_CREATE
OM_MIR_LIST_BUILDER_RESERVE
OM_MIR_LIST_BUILDER_APPEND
OM_MIR_LIST_BUILDER_SEAL
```

未来：

```text
OM_MIR_LIST_SET_COPY
OM_MIR_LIST_SET_CONSUME
```

如果当前已有内部 append path，可先正式化其 verifier/runtime contract，不必为了命名重构。

---

# 36. MIR Builder Verifier

必须保证：

```text
builder cannot escape
builder cannot be returned
builder cannot enter Message
builder cannot be passed to arbitrary user call
builder cannot mutate after seal
builder type never appears in source type table
```

---

# 37. MIR Consume Verifier

必须保证：

```text
consume operand has no future MIR use
consume operation is type-correct
ownership proof attached
no shared/published state known at compile time
runtime fallback available
```

---

# 38. Backend

LLVM backend只执行 MIR已经决定的策略。

不要在 LLVM backend：

```text
重新猜 uniqueness
重新做 alias analysis
决定是否 COW
```

backend职责：

```text
emit runtime call / inline approved operation
```

---

# 39. Runtime ABI

内部 API建议：

```text
om_runtime_list_builder_create
om_runtime_list_builder_reserve
om_runtime_list_builder_append
om_runtime_list_builder_seal

om_runtime_list_update_copy
om_runtime_list_update_consume
```

这些不是 public SDK ABI，也不进入 `.omi`。

---

# 40. Capacity Planning

Query / builder 可按 shape reserve。

例：

```text
TAKE K:
    reserve min(K, source_length)

no WHERE and no TAKE:
    reserve source_length

unknown filter selectivity:
    small initial capacity + geometric growth
```

所有 arithmetic：

```text
checked
resource-limited
```

---

# 41. Growth Policy

建议 geometric growth。

具体倍率：

```text
implementation detail
```

由 benchmark决定，不进入语言规范。

必须防止：

```text
integer overflow
capacity overflow
resource-limit bypass
```

---

# 42. Seal

`seal(builder)`：

```text
mutable construction ends
backing storage becomes immutable published value
builder handle invalid
```

默认不强制：

```text
shrink-to-fit
```

避免额外 copy。

---

# 43. GC

Builder可能包含 managed values：

```text
String
Record
List
Map
Result
```

GC扫描：

```text
0 .. initialized_length - 1
```

不能扫描：

```text
unused capacity
```

为有效 references。

---

# 44. GC Rooting

以下操作可能分配/触发GC：

```text
builder create
builder grow
append managed value
seal
COW copy
Top-K candidate growth
stable sort helper allocations
```

必须 root：

```text
builder
source
current projected value
candidate storage
```

---

# 45. Write Barrier

即使当前 GC不需要复杂 barrier，也应把 mutation集中在：

```text
runtime helper / MIR opcode
```

为未来 generational/incremental GC留插入点。

---

# 46. List Literal

```basic
values = [a, b, c, d]
```

可内部：

```text
allocate exact transient storage
write slots
seal
```

避免 repeated immutable append。

---

# 47. concat

公开：

```text
concat(a,b) -> immutable List
```

首版：

```text
allocate exact result
bulk copy a
bulk copy b
seal
```

未来如果 a consumable且 capacity足够：

```text
reuse a storage
append b
```

---

# 48. reverse

公开 immutable。

首版：

```text
allocate exact result
write reversed
seal
```

未来：

```text
unique consumed storage
-> in-place reverse
```

---

# 49. map/filter

公开仍是 pure Function。

内部：

```text
builder
```

与 Query result construction复用同一 storage基础设施。

---

# 50. TUI 设计

用户层不需要 public Array。

Frame：

```text
List<Cell>
```

状态更新：

```text
list_set
set_range
fill_range
```

Compiler/Runtime通过：

```text
COW
consume
bulk update
```

恢复性能。

---

# 51. TUI 批量更新

为避免大量单点 API overhead，建议后续提供 pure bulk operations：

```text
set_range
fill_range
replace_range
```

先做简单、可优化的 bulk API，不急于设计复杂“update list”结构。

---

# 52. Frame Double Buffer

用户层：

```text
current : List<Cell>
previous: List<Cell>
```

diff是只读纯计算。

current更新走 immutable update API。

Terminal Actor最终接收：

```text
immutable Buffer
```

---

# 53. Editor

文本主体仍优先：

```text
Piece Table / Rope
```

这些结构可以继续使用 immutable Records / Lists。

内部构造和重平衡可以受益于 transient builder，但无需公开 mutable node。

---

# 54. Query 与 TUI 的统一哲学

TUI：

```text
immutable state values
+ optimized state replacement
```

Query：

```text
immutable source
+ mutable internal construction
+ immutable result
```

共同原则：

```text
public immutable
internal mutable when unobservable
```

---

# 55. Native FFI

如果 immutable List通过 Native：

```text
borrowed pointer
captured pointer
foreign callback
```

可能影响 uniqueness。

v1 保守：

```text
Native exposure => not consumable
```

未来 `.omi` 如果增加：

```text
NONCAPTURING
BORROWED_FOR_CALL
```

再优化。

---

# 56. Debug Storage Plan

Compiler开发模式可以增加：

```text
--dump-storage-plan
```

示例：

```text
%12 List<Cell> [published, unique]
%18 list_set consume(%12)
%19 List<Cell> [reused-storage]
```

这是 developer tooling，不是用户语义。

---

# 57. Runtime Counters

Debug/benchmark build可记录：

```text
builder_create
builder_grow
builder_seal
list_update_copy
list_update_reuse
cow_copy
consume_success
consume_fallback
query_builder_append
```

用于确认优化真正发生。

---

# 58. Reference Model

测试保留一个：

```text
always-copy immutable implementation
```

作为 oracle。

优化实现必须与 reference model在所有可观察行为上完全一致。

---

# 59. Differential Testing

随机操作：

```text
create
alias
concat
set
set_range
reverse
record embed
message publish
```

同时运行：

```text
always-copy reference
optimized implementation
```

比较所有 observable values。

---

# 60. Alias Correctness

必须覆盖：

```basic
a = [1,2,3]
b = a
c = list_set(a, 0, 9)
```

要求：

```text
b == [1,2,3]
c == [9,2,3]
```

Record alias：

```text
snapshot = Frame(a)
a2 = list_set(a,...)
snapshot.cells remains old
```

Message publish：

```text
SEND other, Snapshot(a)
a2 = list_set(a,...)
receiver observes old
```

---

# 61. Sanitizers

ASan/UBSan重点：

```text
capacity arithmetic
grow/realloc
bulk copy
seal
COW copy
consume update
Top-K heap
stable sort storage
```

TSan重点：

```text
shared immutable storage
share metadata
Actor worker migration
Message transfer
COW transition
```

---

# 62. Query Tests

覆盖：

```text
WHERE
multiple WHERE
TAKE
ORDER
ORDER + TAKE
multi-key
projection faults
order-key faults
empty source
large source
managed values
```

并确认：

```text
result unchanged
stable ordering unchanged
fault timing unchanged
```

---

# 63. Query MIR / LLVM Smoke

无 ORDER fused Query：

必须看到：

```text
single source scan
internal result builder/append path
seal
```

不得看到：

```text
per-element immutable List reconstruction
List concat chain
```

---

# 64. Query Benchmark

至少比较：

```text
fused Query vs hand immutable-concat loop
fused Query vs optimal builder-style native loop
Top-K vs full stable sort
```

指标：

```text
time
allocations
bytes allocated
peak memory
GC cycles
```

---

# 65. TUI Benchmark

模拟：

```text
120x40 Cell list
1000 point updates/frame
60 frames
```

比较：

```text
always-copy list_set
COW unique reuse
consume optimized replacement
```

目标：

> 证明 immutable API + internal optimization 能否达到真实 TUI 所需性能。

---

# 66. Actor Benchmark

Actor state：

```text
List<Cell>
```

高频：

```text
cells = list_set(cells, index, cell)
```

测：

```text
messages/sec
copy count
reuse count
allocations
GC cycles
```

---

# 67. Phase 0 — ADR / Semantics Freeze

- [x] public collections remain immutable；
- [x] no public Array；
- [x] define transient/published；
- [x] define builder seal；
- [x] storage identity unobservable；
- [x] Query source/result unchanged；
- [x] consume optimization cannot change semantics；
- [x] runtime fallback preserves correctness。

完成标准：

```text
language semantics and internal storage semantics are clearly separated
```

---

# 68. Phase 1 — Internal List Builder

- [x] builder runtime object；
- [x] create；
- [x] reserve；
- [x] append；
- [x] grow；
- [x] seal；
- [x] managed element tracing；
- [x] checked capacity；
- [x] tests。

---

# 69. Phase 2 — MIR Builder Contract

- [x] formalize builder operations；
- [x] verifier；
- [x] no-escape invariant；
- [x] no append after seal；
- [x] GC roots；
- [x] deterministic dumps。

---

# 70. Phase 3 — Query Integration

- [x] fused scan uses builder；
- [x] TAKE reserve；
- [x] no intermediate Lists；
- [x] ORDER candidate transient storage；
- [x] Top-K transient heap；
- [x] final builder；
- [x] seal；
- [x] LLVM smoke；
- [x] benchmark。

公开 Query 仍：

```text
List<T> -> List<R>
```

---

# 71. Phase 4 — List Construction / Transforms

- [x] List literal exact construction；
- [x] concat bulk copy；
- [x] reverse exact result；
- [x] map/filter builder；
- [x] take/drop efficient implementation；
- [x] tests。

---

# 72. Phase 5 — Buffer Builder

- [x] internal byte builder；
- [x] reserve/grow；
- [x] append byte；
- [x] append Buffer；
- [x] seal Buffer；
- [x] Encoding integration；
- [x] Terminal/Network candidates。

当前 Encoding 的 `buffer_from_bytes` 已复用通用 transient `List<Int>` 构造，
并在构造前验证字节范围。新增 `string_utf8_bytes` Runtime intrinsic 使用
精确 reserve 的 transient `List<Int>` 逐字节构造；`utf8_encode` seal 该
List 后发布 `Buffer` Record。HTTP 请求序列化复用 UTF-8 bytes 结果，并用
List concat bulk append body Buffer 的 bytes。Runtime 为这些 `List<Int>`
使用紧凑 1-byte backing storage；byte append、concat、slice、reverse、clone、
GC trace 和 immutable updates 都支持该表示。`buffer_from_bytes` 在验证后
压紧输入，UTF-8 和 hex 编解码也直接使用 compact byte List。公开 `Buffer`
仍由 `Record(List<Int>)` 表示。

---

# 73. Phase 6 — String Builder

- [x] 可增长 UTF-8 String builder；
- [x] append String；
- [x] append scalar（检查 Unicode scalar 范围）；
- [x] interpolation integration；
- [x] join/replace；
- [x] JSON stringify；
- [x] URI/HTTP formatting；
- [x] seal String。

String join 和 interpolation 会一次计算长度并分配最终 String；可复用的
Runtime UTF-8 builder 支持 String 与 Unicode scalar append，并在 seal 时
生成不可变 String。JSON compact 预分配不大于输入长度的输出区，再桥接为不可变 String。URI 使用
多段 interpolation；HTTP 请求头先通过 Query 规范化，再一次 join 完成
请求文本。`text.replace` 由 MIR `string-replace` intrinsic 调用 Runtime：
扫描非重叠 UTF-8 匹配、精确计算输出字节数并一次分配最终 String，不再
先创建中间 `List<String>`。

---

# 74. Phase 7 — Immutable Update APIs

新增必要 pure APIs：

```text
list_set
set_range
fill_range
replace_range
```

第一版先保证：

```text
copy-based correctness
```

不把 API 正确性绑定到 uniqueness优化。

实施状态：

- [x] `list_set(values, index, replacement)` uses copy + transient builder;
- [x] local alias case keeps the old List unchanged;
- [x] `set_range`, `fill_range`, and `replace_range` bulk helpers use one
  copy plus transient construction and preserve the input List.

---

# 75. Phase 8 — Runtime COW

- [x] atomic storage ownership state；
- [x] List/Record/Message/Map publish/share hook；
- [x] shared storage copy fallback；
- [x] defensive consume fallback；
- [x] Message integration；
- [x] GC interaction, including repeated List references while cloning；
- [x] Native calls remain outside eligible producer/use proofs；
- [x] Linux TSan List/Record/Map tests。

---

# 76. Phase 9 — MIR Last-Use Consume

- [x] total SSA use counting across instructions, terminators, and CFG edges；
- [x] fresh-producer, same-block, non-block-parameter alias gate；
- [x] `list-set-consume` opcode and LLVM Runtime call；
- [x] verifier recomputes and checks the ownership proof；
- [x] Runtime copy fallback for shared or contended storage；
- [x] branch tests keep aliased values on the copy path；
- [x] loop tests keep block-parameter values on the copy path；
- [x] Actor-state replacement optimization。

Receive handler 仅在 state List 字段只有一次直接读取、MIR last-use proof
通过且字段当前仍指向被 consume 的 List 时复用 storage。专用 Actor-state
初始化/替换 setter 保持该唯一引用的 ownership；存在重复字段引用、共享
或 proof 不成立时仍标记 shared 并走 copy fallback。Actor-state conformance
与 MIR golden 覆盖该路径。

---

# 77. Phase 10 — Advanced Ownership Analysis

后续可选：

```text
Function no-capture summary
return alias summary
record embedding propagation
loop-carried ownership
interprocedural consume
```

不是第一版 blocker。

---

# 78. 推荐 PR 拆分

## PR 1

```text
architecture: define immutable values and internal transient storage
```

## PR 2

```text
runtime: add internal List builder and seal semantics
```

## PR 3

```text
mir: formalize internal List builder operations and verification
```

## PR 4

```text
query: lower fused Query results through transient List builder
```

## PR 5

```text
query: move stable sort and Top-K candidates to transient storage
```

## PR 6

```text
collections: optimize List literal and concat construction
```

## PR 7

```text
runtime: add internal Buffer builder
```

## PR 8

```text
runtime: add internal String builder
```

## PR 9

```text
stdlib: add immutable list_set and bulk update helpers
```

## PR 10

```text
runtime: add conservative List storage uniqueness and copy-on-write
```

## PR 11

```text
mir: add last-use consume optimization for immutable List updates
```

## PR 12

```text
actor: optimize safe Actor-state List replacement with storage reuse
```

## PR 13

```text
tests: add differential ownership, GC, sanitizer and TSan coverage
```

## PR 14

```text
bench: add Query, TUI-style and COW storage benchmarks
```

## PR 15

```text
docs: document immutable semantics and transient storage architecture
```

---

# 79. ADR

建议新增：

```text
ADR-00xx-immutable-values-and-internal-transient-storage.md
```

已新增并接受 `docs/architecture/adr/ADR-0043-immutable-values-and-transient-storage.md`。

冻结：

1. public collections immutable；
2. no public Array；
3. internal mutable construction allowed；
4. storage identity unobservable；
5. transient vs published；
6. Query source/result unchanged；
7. builder internal-only；
8. COW is implementation detail；
9. consume requires proof；
10. runtime fallback preserves semantics；
11. Actor boundary cannot expose mutable storage；
12. no ownership syntax；
13. no locks/shared mutable collection。

---

# 80. Language Standard 建议条款

## OM-xx.01 — Immutable Collection Semantics

```text
List, Map, String, and Buffer values are immutable at the language level.
```

## OM-xx.02 — Representation Freedom

```text
An implementation may share, reuse, copy, or transiently mutate internal storage provided no program can observe a violation of immutable value semantics.
```

## OM-xx.03 — Storage Identity

```text
The identity or address of collection backing storage is not observable by OMBasic programs.
```

## OM-xx.04 — Message Safety

```text
Internal transient storage cannot cross a Message or Actor boundary.
Only published immutable values participate in Message transfer.
```

## OM-xx.05 — Query

```text
Query consumes immutable List values and produces immutable List values.
Internal mutable storage used during Query execution is not part of Query semantics.
```

---

# 81. Documentation

建议新增：

```text
docs/architecture/immutable-values-and-transient-storage.md
docs/development/collection-storage-optimization.md
```

更新：

```text
OMBasic-Language-Standard.md
docs/language/query.md
docs/stdlib/collections.md
docs/development/implementation_status.md
docs/development/testing_strategy.md
```

---

# 82. CI Gate

基础：

```text
Linux Release
Windows Release
CTest
spec audit
architecture checks
```

Runtime优化阶段：

```text
ASan + UBSan
TSan
GC stress
```

Ownership/COW：

```text
differential reference tests
random alias/publish tests
```

Query：

```text
QIR/MIR tests
LLVM smoke
Query conformance
stable sort equivalence
Top-K equivalence
benchmarks
```

---

# 83. 风险：错误原地修改

最大风险：

```text
storage actually shared
compiler incorrectly mutates
```

会破坏不可变语义。

缓解：

```text
conservative v1
runtime fallback
debug assertions
reference model
differential tests
fuzz
TSan
```

---

# 84. 风险：GC 与 unused capacity

Builder：

```text
capacity > length
```

时，GC不能把未初始化 slot 当成 managed reference。

必须明确：

```text
initialized length
```

---

# 85. 风险：优化复杂度过高

不要第一阶段做：

```text
whole-program alias analysis
borrow checker
region inference
```

先做 Builder。

Builder风险最低、收益最高。

---

# 86. 风险：Map 过早 COW

Map第一轮只做：

```text
construction builder
```

不做复杂 in-place immutable update。

---

# 87. 风险：Query 被 Storage 细节污染

Query逻辑层保持：

```text
Scan / Filter / Order / Take / Project
```

Builder只存在 physical lowering。

未来 SQL backend仍可复用同一 QIR。

---

# 88. 兼容性

这是实现增强。

已有 OMBasic source：

```text
无需修改
```

现有：

```text
List
Map
String
Buffer
Query
Actor
Message
```

公开语义：

```text
不改变
```

---

# 89. Definition of Done — Public Semantics

- [x] 不新增 public Array；
- [x] List immutable；
- [x] Map immutable；
- [x] String immutable；
- [x] Buffer immutable；
- [x] Query仍 `List<T> -> List<R>`；
- [x] Message只看到 immutable values；
- [x] 无 ownership syntax；
- [x] 无 lock/mutex/shared mutable collection。

---

# 90. Definition of Done — Internal Builder

- [x] List transient builder；
- [x] reserve；
- [x] append；
- [x] grow；
- [x] seal；
- [x] GC trace；
- [x] state verification；
- [x] no escape；
- [x] sanitizer coverage。

最新代码的 Linux ASan/UBSan 聚焦 Runtime、String、conformance 与静态
conformance 测试 4/4 通过；TSan List/Record/Map 聚焦测试 3/3 通过。完整
Release CTest 分别为 Windows 39/39、Linux 40/40。Docker TSan 使用 `seccomp=unconfined` 与
`setarch x86_64 -R` 解决容器地址随机化冲突；LLVM 使用本机预装发行版挂载。

---

# 91. Definition of Done — Query

- [x] fused scan uses transient result construction；
- [x] no intermediate Lists；
- [x] TAKE reserve/early exit；
- [x] stable sort uses transient candidate storage；
- [x] Top-K uses bounded mutable heap；
- [x] final result immutable List；
- [x] Query syntax unchanged；
- [x] source remains List；
- [x] fault timing unchanged；
- [x] LLVM smoke；
- [x] benchmark。

---

# 92. Definition of Done — Update Optimization

- [x] copy-based immutable update correct；
- [x] storage ownership metadata；
- [x] COW shared path；
- [x] consume unique path；
- [x] runtime fallback；
- [x] local alias test；
- [x] Record alias test；
- [x] Message publish test；
- [x] Actor-state replacement test；
- [x] differential reference model across copy and consume update sequences；
- [x] Runtime copy-versus-consume microbenchmark with checksum comparison；
- [x] TSan List/Record/Map tests。

---

# 93. 推荐实施顺序

```text
1. ADR
2. Internal List Builder
3. Query result builder formalization
4. Query ORDER / Top-K transient candidate storage
5. List literal / concat builder path
6. Buffer Builder
7. String Builder
8. immutable list_set / set_range
9. Runtime COW
10. MIR last-use consume
11. Actor-state replacement reuse
12. advanced alias analysis
```

不要先做复杂 uniqueness，再补 builder。

---

# 94. TUI 结论

TUI 不要求 public mutable Array。

用户继续：

```text
List<Cell>
```

通过 pure immutable update API 表达变化。

Compiler/Runtime内部通过：

```text
transient storage
COW
consume
bulk update
```

恢复性能。

只有真实 benchmark证明：

```text
immutable API + internal optimization
```

仍无法满足需求时，才重新评估 public mutable local storage。

---

# 95. Query 结论

Query 不需要 `Array<T>` source。

真正需要的是：

```text
efficient List scan
efficient result builder
efficient candidate storage
efficient Top-K heap
```

因此：

```text
Query Language
    remains stable

Query Engine
    becomes more mutable internally
```

---

# 96. Actor 结论

Actor提供：

```text
state owner explicit
handlers non-overlapping
```

这应该成为 optimizer 信息，而不是用户内存模型。

正确方式：

```text
Actor state stores immutable List
↓
state replacement
↓
compiler/runtime proves old storage consumable
↓
internal storage reuse
```

---

# 97. 最终架构

```text
                    OMBasic Program

             List / Map / String / Buffer
                   immutable values
                         │
                         │
        ┌────────────────┴────────────────┐
        │                                 │
      Query                         Immutable Update
        │                                 │
        ▼                                 ▼
  HIR / QIR / MIR                    HIR / MIR
        │                                 │
        └────────────────┬────────────────┘
                         ▼
             Ownership / Storage Planner
                         │
          ┌──────────────┼──────────────┐
          │              │              │
     Transient       Copy-on-Write    Consume
      Builder           Copy           Reuse
          │              │              │
          └──────────────┼──────────────┘
                         ▼
               Immutable Published Value
                         │
                       SEND
                         │
                    Other Actor
```

---

# 98. 最终原则

> **OMBasic exposes immutable value semantics. The compiler and runtime are free to use mutable transient storage, copy-on-write, uniqueness analysis, and storage reuse whenever those optimizations are unobservable. Internal mutability is an implementation technique, not a user programming model.**

中文：

> **OMBasic 对用户提供不可变值语义。只要程序无法观察到语义差异，Compiler 和 Runtime 可以自由使用内部可变临时存储、Copy-on-Write、唯一性分析和存储复用。内部可变性是实现技术，不是用户编程模型。**

覆盖：

```text
Query
Collections
TUI
Editor
JSON
Encoding
String processing
Buffer construction
Actor state evolution
```

并继续保持：

```text
No shared mutable memory
No public mutable collections
No locks
No async functions
Actor-only asynchronous interaction
```

---

# 99. 最小第一阶段验收

第一阶段只要求做到：

```text
Internal List Builder
+
Query integration
+
GC safety
+
LLVM smoke
+
benchmark
```

验收 Query：

```basic
result = QUERY item IN items
    WHERE item.enabled
    TAKE 100
    SELECT item.name
END QUERY
```

公开：

```text
items  : List<Item>
result : List<String>
```

内部：

```text
scan immutable source
↓
transient result storage
↓
append in place
↓
seal
↓
immutable List<String>
```

不能产生：

```text
public Array
public Builder
implicit language ownership
```

---

# 100. 最终实施判断

在完成：

```text
P0 Internal List Builder
P0 Query Integration
P1 Buffer/String Builder
P2 Immutable Update + COW
P3 Consume/Actor-state reuse
```

之前，不建议再次引入用户可见 mutable collection。

这条路线优先证明：

> **OMBasic 是否能够在保持极简不可变语言语义的同时，通过 Compiler/Runtime 的内部可变实现获得足够高的性能。**

只有真实应用和 benchmark证明答案是否定的，才重新打开 public `Array<T>` 设计议题。
