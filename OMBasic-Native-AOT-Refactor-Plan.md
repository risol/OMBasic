# OMBasic Native AOT 全面重构计划

**项目：** OMBasic（Oh My BASIC）Reference Compiler & Runtime<br>
**目标架构：** Native AOT only<br>
**编译器实现语言：** C23<br>
**机器码后端：** LLVM 23.x（开发基线：LLVM 23.1.4）<br>
**链接器：** LLD / 平台系统链接器<br>
**架构方法：** Explicit Architecture<br>
**兼容策略：** 不保留旧 Bytecode / OMBC / VM / Runtime API 兼容性<br>
**源码基线：** `risol/OMBasic` master，审查基线 commit `ecbbacc5fb235c4a706a9c4eeb31bc7b41dec299`<br>
**文档日期：** 2026-10-28

---

# 0. 决策摘要

本次重构不做“双后端迁移”，也不保留 VM 作为 fallback。

最终唯一执行路线：

```text
.bas source
    ↓
Source / Module Loader
    ↓
Lexer / Parser
    ↓
AST
    ↓
Name Resolution
    ↓
Type Inference
    ↓
Effects / Actor Protocol / Serializable Analysis
    ↓
Typed HIR
    ↓
Specialization / Monomorphization
    ↓
Native MIR
    ↓
LLVM IR
    ↓
LLVM TargetMachine
    ↓
.o / .obj
    ↓
LLD / platform linker
    ↓
Native executable
```

运行时：

```text
Native executable
    │
    ├── OMBasic native functions
    ├── OMBasic native Actor handlers
    ├── generated program/type/message metadata
    │
    └── libombasic_runtime
          ├── memory / GC
          ├── Actor registry
          ├── mailbox
          ├── M:N worker scheduler
          ├── system Actors
          ├── Native callback ingress
          └── distributed Actor transport
```

明确删除：

```text
OMBC
Bytecode format
Bytecode builder
Bytecode emitter
Bytecode verifier
VM operand stack
VM call frame
VM instruction dispatcher
VM instruction quantum
VM public API
VM runtime values
VM-native callback API
VM-distribution integration
libffi runtime dispatch path
旧 Bytecode / VM ADR
旧 Bytecode / VM tests
```

不生成 C，不经过：

```text
OMBasic -> generated C -> clang/gcc
```

LLVM 仅作为编译器内部 Native Code Generation backend：

```text
OMBasic MIR -> LLVM IR -> machine code
```

最终程序运行时不需要 LLVM，也不存在 OMBasic VM。

---

# 1. 为什么现在应该做破坏性重构

当前仓库已经完成了完整 VM 路线，因此 VM 概念已经扩散到多个 capability：

- `src/bytecode/` 包含 builder / emitter / format / verifier；
- `include/ombasic/bytecode/` 暴露 OMBC public API；
- `src/runtime/vm/vm.c` 当前约 250 KB，已经同时承担执行器、Actor、调度、Native、distribution 等大量职责；
- `include/ombasic/runtime/vm.h` 是当前 Runtime 主要公共 API；
- `src/runtime/value/value.c` 以 `Om_VM_Value` 为中心；
- `src/runtime/heap/heap.c` 以 VM Object/Heap 为中心；
- `src/runtime/distribution/vm_distribution.c` 直接从 OMBC schema 和 VM value 做网络转换；
- `src/compiler/native_binding/` 同时依赖 VM、Bytecode、Semantic、OMI；
- `src/ffi/native_call.c` 使用 libffi 在运行时执行动态 C ABI 调用；
- CLI `run/build` 以 bytecode image 与 VM 为运行模型；
- Actor scheduler 使用 VM instruction quantum 保存/恢复执行上下文；
- System Actor 使用 `LOAD_SYSTEM_ACTOR` bytecode opcode；
- distribution program/interface fingerprint 当前存储在 OMBC image 中。

因此本次重构必须按 **architecture replacement** 处理，而不是新增：

```text
backend/native/
```

然后继续让 Native backend 依赖旧 VM。

正确做法：

> 先定义新的 Native Compiler / Runtime ABI，再把 VM/OMBC 整体删除，之后让保留下来的前端与语义层接入 HIR/MIR/AOT。

---

# 2. 本次重构的硬性原则

## 2.1 Native AOT 是唯一 backend

不保留：

- VM backend；
- bytecode backend；
- interpreter；
- JIT；
- generated-C backend。

## 2.2 不保留旧 artifact 兼容

不读取：

```text
*.ombc
OMBC v1
OMBC v2
OMBC v3
```

不提供：

```text
legacy bytecode loader
bytecode converter
old runtime compatibility shim
Om_VM alias
om_vm_* compatibility functions
```

Git 历史已经保存旧实现，不需要在主分支继续携带旧代码。

## 2.3 Runtime 仍然存在，但 Runtime ≠ VM

Native AOT 后仍然需要：

- GC；
- Actor；
- scheduler；
- mailbox；
- system Actor；
- Native callback ingress；
- distribution；
- runtime error handling。

但是 Runtime 不再负责解释语言指令。

## 2.4 LLVM 只负责低层代码生成

LLVM 不拥有 OMBasic 语言语义。

以下语义必须在 OMBasic HIR/MIR 中已经明确：

- `TRY`；
- `WITH`；
- `SEND`；
- `SPAWN`；
- `WATCH`；
- Actor state；
- Result propagation；
- Function purity；
- Message protocol；
- serialization；
- runtime checks。

LLVM 只看到：

```text
values
basic blocks
branches
calls
loads/stores
aggregates
runtime ABI calls
```

## 2.5 不机械移植 VM

尤其禁止：

> 把每一个 bytecode opcode 改成一个 MIR opcode，然后继续模拟 VM。

Native MIR 应表达程序语义，而不是旧 Bytecode instruction set。

## 2.6 Actor handler 真正 run-to-completion

Native worker 调用一个 native handler：

```text
handler begins
    ↓
ordinary native call stack
    ↓
handler returns
```

期间不通过 VM quantum、fiber、coroutine 或 continuation 暂停并迁移 handler。

同一个 Actor 的下一条 Message 只有当前 handler 返回后才能执行。

## 2.7 BLOCKING Native 不得隐式 suspend Actor

新的实现删除当前：

```text
save VM stack/frame
release worker
native completes
resume VM continuation
```

Actor `RECEIVE` 中直接调用 `.omi` 标记 `BLOCKING` 的 Native function 应产生编译错误。

阻塞工作通过：

```text
System Actor / Native Worker Actor
SEND request
handler ends
completion Message later arrives
```

完成。

---

# 3. 当前代码保留 / 删除 / 重写矩阵

## 3.1 原则上保留

这些 capability 与 VM 无关，已有实现价值高，应保留并继续测试：

```text
src/foundation/
src/source/
src/syntax/
src/module_graph/
src/semantic/
```

对应 public headers 同样保留：

```text
include/ombasic/foundation/
include/ombasic/source/
include/ombasic/syntax/
include/ombasic/module_graph/
include/ombasic/semantic/
```

其中 semantic 后续增加 HIR lowering 所需 query API，但不应引入 LLVM dependency。

### 保留的重要现有能力

- checked allocation；
- Arena；
- Vector；
- UTF-8；
- typed IDs；
- source span；
- strict import path；
- immutable syntax-only AST；
- name resolution；
- HM-style type inference；
- effect analysis；
- Actor protocol analysis；
- serializability analysis；
- diagnostic codes；
- formatter；
- LSP 对前端/semantic 的复用思想。

---

## 3.2 直接删除

### Bytecode capability

完整删除：

```text
src/bytecode/
include/ombasic/bytecode/
tests/bytecode/
```

包括：

```text
builder
emitter
format
verifier
OMBC encoder
OMBC decoder
bytecode source maps
bytecode schemas
opcode definitions
bytecode mutation tests
bytecode image fuzz tests
```

### VM implementation

完整删除：

```text
src/runtime/vm/
src/runtime/vm_internal.h
include/ombasic/runtime/vm.h
```

不要尝试拆解 `vm.c` 继续使用。

只有行为测试和已验证算法思想可以作为重写参考。

### 当前 Compiler/VM binding

删除：

```text
src/compiler/native_binding/
include/ombasic/compiler/native_binding.h
```

原因：它的职责建立在：

```text
Om_VM + Om_Bytecode_Image + Semantic + OMI
```

四方耦合之上，Native AOT 下不应存在。

### VM distribution integration

删除：

```text
src/runtime/distribution/vm_distribution.c
```

重新建立：

```text
runtime/distribution/actor_distribution.c
```

### VM Value implementation

删除：

```text
src/runtime/value/value.c
```

Native AOT 不以 universal `Om_VM_Value` 作为语言执行数据模型。

### VM Heap implementation

删除：

```text
src/runtime/heap/heap.c
```

算法思想可以参考，但 API / 类型必须以新的 native object + GC root model 重写。

---

## 3.3 FFI：部分保留概念，删除 Runtime 动态调用模型

当前：

```text
src/ffi/abi_type.c
src/ffi/c_string.c
src/ffi/dynamic_library.c
src/ffi/native_call.c
src/ffi/omi_parser.c
src/ffi/omi_project_loader/
```

目标：

### 删除

```text
src/ffi/native_call.c
src/ffi/dynamic_library.c   # 第一版 Native AOT 使用链接期符号解析
libffi dependency
```

### 重写/迁移

```text
omi_parser.c
omi_project_loader/
```

迁移至：

```text
src/native/metadata/
```

### 重写

`abi_type.c` 不再根据“当前 compiler process ABI”计算，而改成：

```text
src/target/c_abi/
```

根据目标 triple 计算 ABI。

### 保留行为，重写 API

`c_string.c` 迁移为：

```text
src/runtime/native/c_string.c
```

用于生成代码与 C ABI 之间的显式 CString 转换。

---

## 3.4 Distribution：保留 transport/protocol，删除 VM value/schema coupling

原则上可保留：

```text
src/distribution/node_id.c
src/distribution/protocol.c
src/distribution/transport.c
src/distribution/handshake.c
src/distribution/connection_handshake.c
```

但重新审查 API ownership。

当前 `serializer.c` 如果依赖通用 VM-like value tree，则重写为：

```text
AOT type descriptor driven serialization
```

新的 Runtime distribution 不读取：

```text
Om_Bytecode_Schema
Om_VM_Value
Om_Bytecode_Image
```

而读取：

```text
Om_Program_Descriptor
Om_Type_Descriptor
Om_Message_Descriptor
Om_Actor_Descriptor
```

---

## 3.5 System Actor：算法可保留，Runtime 边界重写

现有：

```text
blocking_pool.c/.h
timer_service.c/.h
```

这两部分逻辑本身不依赖字节码概念，可保留算法和测试。

但所有：

```text
Om_VM
Om_VM_Value
VM mailbox
VM callback
bytecode system actor enum
```

必须移除。

---

# 4. 新的顶层架构

```text
                              ┌──────────────────────┐
                              │      OMBasic CLI     │
                              └──────────┬───────────┘
                                         │
                              ┌──────────▼───────────┐
                              │   Compiler Pipeline  │
                              └──────────┬───────────┘
                                         │
          ┌──────────────────────────────┼──────────────────────────────┐
          │                              │                              │
          ▼                              ▼                              ▼
    Frontend/Semantic                   HIR                            Target
          │                              │                              │
          └──────────────────────────────▼──────────────────────────────┘
                                         MIR
                                          │
                                          ▼
                                   LLVM Codegen
                                          │
                                          ▼
                                      Object File
                                          │
                                          ▼
                                        Linker
                                          │
                                          ▼
                                  Native Executable
                                          │
                                          ▼
                                libombasic_runtime
```

---

# 5. 新仓库结构

推荐最终结构：

```text
OMBasic/
├── CMakeLists.txt
├── cmake/
│   ├── LLVM.cmake
│   ├── LLD.cmake
│   ├── Sanitizers.cmake
│   ├── Warnings.cmake
│   └── ArchitectureChecks.cmake
│
├── OMBasic-Language-Standard.md
├── OMBasic-Native-AOT-Development-Plan.md
│
├── docs/
│   ├── architecture/
│   │   ├── overview.md
│   │   ├── dependency_rules.md
│   │   └── adr/
│   ├── compiler/
│   │   ├── hir.md
│   │   ├── mir.md
│   │   ├── specialization.md
│   │   ├── llvm_backend.md
│   │   ├── native_abi.md
│   │   └── linking.md
│   ├── runtime/
│   │   ├── runtime_abi.md
│   │   ├── object_model.md
│   │   ├── gc.md
│   │   ├── actor.md
│   │   ├── scheduler.md
│   │   ├── system_actors.md
│   │   └── native_callbacks.md
│   ├── distribution/
│   │   └── protocol.md
│   └── development/
│       ├── c23_safety_profile.md
│       ├── testing_strategy.md
│       ├── fuzzing.md
│       └── implementation_status.md
│
├── include/ombasic/
│   ├── foundation/
│   ├── source/
│   ├── syntax/
│   ├── module_graph/
│   ├── semantic/
│   ├── native/
│   │   └── metadata.h
│   ├── ir/
│   │   ├── hir.h
│   │   ├── mir.h
│   │   └── ids.h
│   ├── target/
│   │   ├── target.h
│   │   └── c_abi.h
│   ├── backend/
│   │   └── llvm_backend.h
│   ├── linker/
│   │   └── linker.h
│   ├── compiler/
│   │   ├── session.h
│   │   ├── pipeline.h
│   │   └── result.h
│   ├── runtime/
│   │   ├── abi.h
│   │   ├── program.h
│   │   ├── type_descriptor.h
│   │   ├── actor.h
│   │   └── native_ingress.h
│   └── tooling/
│
├── src/
│   ├── foundation/
│   ├── source/
│   ├── syntax/
│   ├── module_graph/
│   ├── semantic/
│   │
│   ├── native/
│   │   └── metadata/
│   │       ├── omi_parser.c
│   │       └── omi_project.c
│   │
│   ├── ir/
│   │   ├── hir/
│   │   │   ├── hir.c
│   │   │   ├── hir_builder.c
│   │   │   ├── hir_verify.c
│   │   │   └── hir_dump.c
│   │   ├── specialize/
│   │   │   └── specialize.c
│   │   └── mir/
│   │       ├── mir.c
│   │       ├── mir_builder.c
│   │       ├── mir_verify.c
│   │       └── mir_dump.c
│   │
│   ├── target/
│   │   ├── target.c
│   │   └── c_abi/
│   │       ├── c_abi.c
│   │       ├── sysv_x64.c
│   │       ├── win64_x64.c
│   │       ├── aapcs64.c
│   │       └── darwin_arm64.c
│   │
│   ├── backend/
│   │   └── llvm/
│   │       ├── llvm_context.c
│   │       ├── llvm_types.c
│   │       ├── llvm_functions.c
│   │       ├── llvm_runtime.c
│   │       ├── llvm_codegen.c
│   │       ├── llvm_debug.c
│   │       └── llvm_object.c
│   │
│   ├── linker/
│   │   ├── linker.c
│   │   ├── lld_posix.c
│   │   └── lld_windows.c
│   │
│   ├── compiler/
│   │   ├── session/
│   │   ├── pipeline/
│   │   └── diagnostics/
│   │
│   ├── runtime/
│   │   ├── core/
│   │   ├── data/
│   │   │   ├── string.c
│   │   │   ├── list.c
│   │   │   └── map.c
│   │   ├── gc/
│   │   │   ├── heap.c
│   │   │   ├── roots.c
│   │   │   └── trace.c
│   │   ├── actor/
│   │   │   ├── actor.c
│   │   │   ├── mailbox.c
│   │   │   └── registry.c
│   │   ├── scheduler/
│   │   │   └── scheduler.c
│   │   ├── system_actor/
│   │   │   ├── console.c
│   │   │   ├── file_system.c
│   │   │   ├── timer.c
│   │   │   ├── blocking_pool.c
│   │   │   └── timer_service.c
│   │   ├── native/
│   │   │   ├── c_string.c
│   │   │   └── callback_ingress.c
│   │   └── distribution/
│   │       ├── peer.c
│   │       ├── routing.c
│   │       └── actor_distribution.c
│   │
│   ├── distribution/
│   │   ├── node_id.c
│   │   ├── protocol.c
│   │   ├── transport.c
│   │   ├── handshake.c
│   │   └── serializer.c
│   │
│   └── tooling/
│       ├── main.c
│       ├── check.c
│       ├── build.c
│       ├── run.c
│       ├── formatter.c
│       └── diagnostics_json.c
│
├── runtime/
│   └── cmake/
│       └── runtime-targets.cmake
│
├── tools/
│   ├── architecture_check/
│   ├── ombasic_bind.py
│   └── ombasic_lsp.py
│
└── tests/
    ├── foundation/
    ├── source/
    ├── syntax/
    ├── module_graph/
    ├── semantic/
    ├── hir/
    ├── mir/
    ├── backend/
    ├── native/
    ├── runtime/
    ├── actor/
    ├── distribution/
    ├── tooling/
    ├── conformance/
    ├── compile_fail/
    └── native_integration/
```

---

# 6. Explicit Architecture 新依赖规则

编译器方向：

```text
foundation
   ↓
source
   ↓
syntax
   ↓
module_graph
   ↓
semantic
   ↓
hir
   ↓
specialization
   ↓
mir
   ↓
target
   ↓
backend_llvm
   ↓
compiler
   ↓
tooling_cli
```

Native metadata：

```text
foundation -> source -> native_metadata
                         ↓
                     semantic
```

Runtime：

```text
runtime_abi
    ↓
foundation -> runtime_core
                 ↓
             runtime_gc
                 ↓
             runtime_data
                 ↓
             runtime_actor
                 ↓
           runtime_scheduler
              ↓        ↓
     runtime_system   runtime_distribution
```

低层 distribution：

```text
foundation -> distribution_protocol/transport
                         ↓
                runtime_distribution
```

### 绝对禁止

```text
runtime -> syntax
runtime -> semantic
runtime -> HIR/MIR
runtime -> LLVM
runtime -> compiler
LLVM backend -> runtime implementation internals
semantic -> LLVM
HIR -> LLVM
MIR -> LLVM headers
```

LLVM backend 只能依赖 Runtime ABI contract：

```text
include/ombasic/runtime/*.h
```

不能 include：

```text
src/runtime/**/internal.h
```

---

# 7. Runtime ABI：新的核心边界

Native AOT 最大的架构变化不是 LLVM，而是建立一个清晰的：

> **Generated Native Code ↔ Runtime ABI**

这个 ABI 替代旧：

```text
Bytecode ↔ VM
```

## 7.1 所有生成的 OMBasic 函数使用隐藏 Context

概念签名：

```c
Result om_function(
    Om_Execution_Context *context,
    ...user parameters...
);
```

即使是 `FUNCTION` 也携带 context，因为纯函数仍可能：

- 构造 String；
- 构造 immutable Record；
- 构造 List/Map；
- 触发 GC allocation。

Effect system 保证它不会通过 context 做非法副作用。

## 7.2 Context 不等于 VM

`Om_Execution_Context` 只包含执行当前 native function 所需：

```text
runtime pointer
current Actor (nullable for root Action)
current heap
gc root chain
runtime fault boundary
optional source context
```

它没有：

```text
instruction pointer
operand stack
bytecode image
VM frame
opcode
instruction quantum
```

---

# 8. Native 数据表示

## 8.1 Scalar

```text
Int      -> i64
Float    -> double
Bool     -> i1 in SSA; i8 when stored if needed
Unit     -> no semantic payload / internal i8
Null     -> singleton / no payload
Actor_Id -> u64
```

## 8.2 Managed values

第一版统一 heap managed：

```text
String
Record
Message
List
Map
```

LLVM 层表示为 pointer。

不要第一版同时实现：

- stack Record；
- scalar replacement；
- escape analysis；
- shared immutable heap。

这些放到优化阶段。

## 8.3 Object header

```c
typedef struct Om_Object_Header {
    const Om_Type_Descriptor *type;
    struct Om_Object_Header *gc_next;
    uint32_t gc_flags;
} Om_Object_Header;
```

第一版 GC non-moving，因此 native pointer 在 collection 前后稳定。

## 8.4 Record

每个 concrete Record 编译成 native layout：

```text
header
field_0
field_1
...
```

因为 OMBasic 不承诺 C struct ABI，所以内部布局可由 compiler 控制。

## 8.5 Message

Message object：

```text
header
message descriptor pointer
payload fields
```

Message 永远 immutable。

## 8.6 List

Runtime generic representation：

```text
Om_List
  length
  element_type_descriptor
  backing storage
```

元素按 descriptor 的：

```text
size
alignment
trace
copy
equality
hash (when allowed)
```

操作。

这不是 `Any` 类型；类型仍由 compiler 静态确定。

## 8.7 Map

Map 同样使用：

```text
key descriptor
value descriptor
```

不引入语言级 dynamic value。

## 8.8 Result

每个具体：

```text
Result<T,E>
```

在 HIR specialization 后变为 concrete native aggregate。

第一版推荐：

```text
{ tag, ok_payload, error_payload }
```

允许浪费少量空间，优先正确性和简单 ABI。

## 8.9 Function reference

当前语言没有 class/vtable。

如果 Function 是一等值：

```text
Om_Function_Ref
  function pointer
  signature descriptor/id
```

当前没有 lambda closure 时 environment = none。

---

# 9. Type Descriptor 系统

Native Runtime 不再依赖 OMBC schema。

Compiler 为程序生成静态 descriptors。

## 9.1 `Om_Type_Descriptor`

至少：

```text
kind
size
alignment
fingerprint
field metadata / element metadata
trace callback
copy callback
equal callback
hash callback
serialize callback or structural metadata
```

## 9.2 `Om_Message_Descriptor`

```text
canonical name
schema fingerprint
payload size/alignment
field descriptors
copy function
trace function
encode/decode information
```

## 9.3 `Om_Actor_Descriptor`

```text
canonical actor name
protocol fingerprint
state size/alignment
state initializer
state tracer
handler table
```

Handler table：

```text
message fingerprint -> native function pointer
```

## 9.4 `Om_Program_Descriptor`

替代旧 OMBC image：

```text
program fingerprint
interface fingerprint
type descriptors
message descriptors
actor descriptors
source metadata
entry Action pointer
```

这是链接进 executable 的静态数据，不是运行时加载文件。

---

# 10. HIR 设计

AST 不直接生成 LLVM。

必须增加 OMBasic 自己的 **Typed HIR**。

## 10.1 HIR 目标

HIR 解决：

- parser 语法细节；
- name resolution；
- inferred type；
- nominal identity；
- effects；
- Actor protocol；
- serialization capability；
- source span。

HIR 中不得存在 unresolved identifier。

## 10.2 HIR Entity

```text
Hir_Module
Hir_Function
Hir_Action
Hir_Record
Hir_Message
Hir_Actor
Hir_Receive
```

## 10.3 HIR Expression

至少：

```text
Literal
Local
Call
IndirectCall
FieldRead
RecordConstruct
RecordWith
MessageConstruct
ListConstruct
MapConstruct
Index
Unary
Binary
ResultOk
ResultError
Try
Spawn
Send
Watch
SystemActor
```

## 10.4 HIR Statement

```text
Bind
AssignVar
ActorStateAssign
If
While
ForRange
ForEach
Select
Return
Stop
ExpressionStatement
```

## 10.5 HIR 必须显式记录

```text
Type_Id
Symbol_Id
Source Span
Effect summary
```

Actor operations额外记录：

```text
Actor protocol ID
Message schema ID
```

## 10.6 HIR verifier

验证：

- every value has concrete/inferable type；
- no unresolved symbol；
- Function contains no forbidden effects；
- SEND protocol valid；
- Message serializable；
- actor state access only in legal context；
- TRY has Result input；
- block terminators structurally valid。

---

# 11. Generic specialization / Monomorphization

当前 type inference 支持 HM 风格 polymorphism。

Bytecode VM 可以天然容纳较多 erased behavior；Native AOT 必须明确 specialization。

## 11.1 Instance key

```text
Symbol_Id
+
Concrete type arguments
```

得到：

```text
Function_Instance_Id
```

## 11.2 Worklist

roots：

- `ACTION main()`；
- Actor state initializers；
- Receive handlers；
- Native callback trampolines；
- runtime-required generated helpers。

从 roots 遍历 call graph：

```text
call generic function
→ instantiate concrete types
→ enqueue new instance
```

## 11.3 禁止无限 specialization

0.1 应拒绝真正 polymorphic recursion。

如果 specialization graph 不收敛：

```text
compile diagnostic
```

而不是无限生成代码。

## 11.4 Generic collections

List/Map runtime implementation不需要每种 T 生成一份 C runtime function。

Compiler 将 concrete `Om_Type_Descriptor*` 传给 generic runtime container helpers。

---

# 12. MIR 设计

MIR 是 Native backend 的主要 contract。

## 12.1 MIR 不是 stack machine

禁止：

```text
PUSH
POP
LOAD_CONST opcode
STORE_LOCAL opcode
```

使用：

```text
Value_Id
Basic_Block_Id
```

## 12.2 CFG

每个 function：

```text
entry block
basic blocks
block parameters
terminators
```

推荐使用 block parameters，而不是自己先实现复杂 Phi construction API。

例如：

```text
block0:
    v1 = ...
    cond_br v1, block1, block2

block1:
    v2 = ...
    br block3(v2)

block2:
    v3 = ...
    br block3(v3)

block3(v4):
    ret v4
```

LLVM lowering 时 block parameter 转成 PHI。

## 12.3 MIR operation

### Scalar

```text
ConstInt
ConstFloat
ConstBool
IntAddChecked
IntSubChecked
IntMulChecked
IntDivChecked
FloatAdd
Compare
```

### Memory/Object

```text
AllocObject
LoadField
StoreInitField
CopyManaged
ListCreate
ListGet
MapCreate
MapGet
```

### Result

```text
MakeOk
MakeError
ResultTag
ResultOkPayload
ResultErrorPayload
```

### Runtime

```text
RuntimeStringCreate
RuntimeActorSpawn
RuntimeActorSend
RuntimeActorWatch
RuntimeActorStop
RuntimeSystemActor
RuntimeFault
```

### Native

```text
NativeCall
NativeCallbackAddress
CStringConvert
```

### Control

```text
Br
CondBr
Switch
Return
Unreachable
```

## 12.4 MIR verifier

必须检查：

- block has exactly one terminator；
- values defined before use；
- block args match；
- type match；
- no unresolved generic；
- no illegal runtime intrinsic in pure Function；
- no `BLOCKING NativeCall` in Actor handler；
- no VM/bytecode concepts。

---

# 13. LLVM Backend

## 13.1 LLVM version

开发基线：

```text
LLVM 23.1.4
```

第一阶段只支持 LLVM major 23。

不写：

```text
LLVM 18 compatibility
LLVM 19 compatibility
LLVM 20 compatibility
...
```

不考虑旧 backend compatibility。

## 13.2 使用 LLVM C API

OMBasic compiler 源码继续使用 C23。

主要接口：

```text
llvm-c/Core.h
llvm-c/Analysis.h
llvm-c/Target.h
llvm-c/TargetMachine.h
llvm-c/Transforms/PassBuilder.h
llvm-c/DebugInfo.h
```

不在 compiler source 中引入 C++ LLVM API。

由于 LLVM library 自身为 C++ 实现，CMake 可为最终 linker 启用 C++ linker runtime，但 OMBasic compiler source 保持 C23。

## 13.3 Target initialization

启动：

```text
LLVMInitializeNativeTarget
LLVMInitializeNativeAsmPrinter
LLVMInitializeNativeAsmParser (only when needed)
```

第一版只支持 host target。

之后再支持：

```text
--target <triple>
```

## 13.4 LLVM type lowering

示例：

```text
Int              -> i64
Float            -> double
Bool SSA         -> i1
Managed object   -> ptr
ActorRef         -> runtime ABI struct
Result<T,E>      -> concrete LLVM struct
Record           -> named LLVM struct payload/object layout
Function         -> typed function pointer
```

## 13.5 Checked integer arithmetic

使用 LLVM overflow intrinsics：

```text
llvm.sadd.with.overflow.i64
llvm.ssub.with.overflow.i64
llvm.smul.with.overflow.i64
```

发生 overflow：

```text
call om_runtime_fault(...)
unreachable
```

保持 OMBasic checked Int64 语义。

## 13.6 Division checks

编译器显式插入：

- divisor == 0；
- `INT64_MIN / -1` overflow。

## 13.7 LLVM module verification

每次 codegen 后必须：

```text
LLVMVerifyModule
```

失败属于 compiler bug，不是用户 diagnostic。

## 13.8 Optimization

开发：

```text
-O0
```

Release 默认：

```text
-O2
```

LLVM pass pipeline 通过新 PassBuilder C API。

不要第一版加入自定义 LLVM pass。

## 13.9 Object emission

使用 TargetMachine 直接输出：

```text
.o
.obj
```

不输出 C。

可调试输出：

```text
--emit=llvm-ir
--emit=asm
--emit=obj
```

---

# 14. Native Linker

## 14.1 默认产物

```text
ombasic build src/main.bas
```

输出 native executable。

## 14.2 Link inputs

```text
program object
libombasic_runtime
Native libraries from .omi / build config
platform libraries
```

## 14.3 第一版 compiler/linker 组合

标准开发组合：

| 平台 | 编译器驱动 | 实际链接器 | 目标格式 / ABI | 平台工具链依赖 |
| --- | --- | --- | --- | --- |
| Windows x64 | `clang-cl.exe` | `lld-link.exe` | COFF / MSVC | MSVC C++ Build Tools 与 Windows SDK，提供头文件、UCRT 和系统库 |
| Linux x64 | `clang` | `ld.lld`，由 `clang -fuse-ld=lld` 选择 | ELF / Linux system ABI | 目标主机的 libc、启动文件和系统开发库 |

两平台都使用 CMake + Ninja。Windows 的 C 编译和链接调用由 `clang-cl.exe`
驱动，最终由 `lld-link.exe` 产生 MSVC ABI 的 COFF 可执行文件；需要 MSVC
C++ Build Tools 和 Windows SDK，但不需要 MinGW GCC，也不把 `cl.exe` 作为
C 编译器。Linux 由 `clang` 驱动并通过 `-fuse-ld=lld` 调用 `ld.lld`，生成
使用 Linux system ABI 的 ELF 可执行文件。

通过 Clang driver 调用 LLD，让 driver 负责平台 ABI 参数、启动文件和
系统运行库。这不构成 generated-C backend；OMBasic 仍直接生成 LLVM
object 并链接成 native executable。macOS 使用 `clang` 配合
`ld64.lld` 或配置的平台链接器。

## 14.4 Linker invocation

不要拼接 shell command string。

使用 argv vector + process spawn API，避免 quoting/injection 问题。

## 14.5 Runtime link model

第一版：

```text
static runtime
```

即：

```text
libombasic_runtime.a
ombasic_runtime.lib
```

这样 executable 不依赖单独安装的 OMBasic runtime shared library。

---

# 15. Native Executable Entry Point

Compiler 自动生成平台 entry：

```text
main / platform equivalent
```

概念：

```c
int main(int argc, char **argv)
{
    return om_runtime_start(
        &om_program_descriptor,
        om_user_main,
        argc,
        argv);
}
```

实际由 LLVM IR 生成，不生成 C 文件。

用户：

```basic
ACTION main()
    ...
END ACTION
```

编译为：

```text
om_user_main(Om_Execution_Context*)
```

---

# 16. Native GC 设计

VM 消失后，GC 最大变化是：

> roots 不再来自 VM operand stack / VM frames。

## 16.1 第一版采用 Shadow Root Stack

不立即使用 LLVM statepoint / stackmap GC。

生成代码显式维护：

```text
Om_Gc_Root_Frame
```

## 16.2 Root frame

概念：

```c
typedef struct Om_Gc_Root_Frame {
    struct Om_Gc_Root_Frame *previous;
    const Om_Gc_Frame_Descriptor *descriptor;
    void **slots;
} Om_Gc_Root_Frame;
```

函数入口：

```text
push root frame
```

所有 return path：

```text
pop root frame
```

## 16.3 Root policy

第一版为了简单：

- managed parameters 是 roots；
- managed local 在整个 function lifetime 可保持 root；
- 不先做精确 liveness shrink。

这可能多保留一些对象，但安全、清晰。

## 16.4 Actor heap

每 Actor：

```text
Om_Actor_Heap
object list
gc threshold
root context
```

仍采用：

```text
non-moving mark-and-sweep
```

## 16.5 Root sources

- native root frame chain；
- Actor state；
- current handler Message；
- runtime temporary roots；
- root Action heap roots。

Mailbox transit objects不属于 Actor heap。

## 16.6 GC safepoint

主要发生：

```text
allocation slow path
explicit runtime poll when necessary
```

Native handler 不需要 bytecode instruction safe point。

## 16.7 不做 moving GC

因为 native code 持有 raw managed pointers，第一版 moving GC 会大幅增加复杂度。

未来若要 moving GC，再引入 LLVM statepoints。

---

# 17. Native Actor Runtime

## 17.1 Actor 不再保存 VM execution state

旧：

```text
VM stack
VM call frames
PC
instruction budget
```

全部删除。

新 Actor：

```text
Actor_Id
Actor_Descriptor*
state pointer
Actor heap
mailbox
status
watchers
scheduler flags
```

## 17.2 Handler signature

概念：

```c
Om_Status handler(
    Om_Execution_Context *context,
    void *state_payload,
    const void *message_payload);
```

实际 payload layout 由 compiler/descriptor 确定。LLVM 后端生成 Runtime ABI
adapter，借助当前 execution context 取得受管理的 state/message object，再
调用 lowered receive function。

## 17.3 Run-to-completion

worker：

```text
dequeue ready Actor
lock/claim Actor
pop one Message
create execution context
call native handler function pointer
handler returns
commit Actor status
queue again if mailbox non-empty
release Actor
```

一个 handler 运行过程中：

- 不切换到同 Actor 的其他 Message；
- 不保存 native stack；
- 不迁移 continuation；
- 不恢复 suspended OMBasic function。

## 17.4 删除 instruction quantum

现有 VM 的：

```text
256 instruction quantum
saved VM state
resume on another worker
```

全部删除。

Native worker 由 OS scheduler 抢占，但 OMBasic Runtime 不做用户态 continuation。

## 17.5 CPU-bound handler

如果 handler 无限循环：

- 它占据一个 worker；
- 其他 workers 仍可工作；
- Runtime 不假装可以透明暂停它。

这和“隐藏 continuation 不存在”的语言原则一致。

## 17.6 Worker pool

仍为 M:N：

```text
many Actors
↓
ready queue
↓
N worker OS threads
```

第一版保持：

```text
global locked FIFO ready queue
```

不要在重构时顺便引入 work stealing。

---

# 18. Actor State Native Layout

Compiler 为每个 Actor 生成 state struct layout。

例如：

```basic
ACTOR Counter
    VAR count = 0
END ACTOR
```

概念 native payload：

```text
Counter_State {
    i64 count
}
```

Runtime 只知道：

```text
size
alignment
initializer pointer
trace descriptor/function
```

Runtime 不理解 `count` 语义。

如果 state 包含 managed references，generated tracer 标记对应对象。

---

# 19. Message Ownership / Actor Isolation

OMBasic 的 Actor isolation 继续保持。

## 19.1 本地 SEND

Compiler 生成：

```text
construct Message payload
om_actor_send(context, receiver, descriptor, payload)
```

Runtime：

```text
copy payload into mailbox-owned transit storage
```

Receiver dispatch：

```text
copy payload into receiver Actor heap
```

## 19.2 为什么不直接共享 pointer

虽然 Message immutable，但第一版继续保持明确 ownership：

```text
sender heap != mailbox != receiver heap
```

后续可 benchmark 后加入 shared immutable storage。

## 19.3 Message copy

由 generated descriptor / copy function 完成。

不再通过 `Om_VM_Value` tree。

---

# 20. System Actor 重构

## 20.1 Built-in ActorRef

删除：

```text
LOAD_SYSTEM_ACTOR opcode
Om_Bytecode_System_Actor
```

改成 Runtime ABI：

```c
Om_Actor_Ref om_runtime_system_actor(
    Om_Execution_Context *context,
    Om_System_Actor_Id id);
```

## 20.2 Console

保留 Message API。

## 20.3 File_System

阻塞文件操作继续在 blocking pool 执行。

worker 不保留 Actor heap pointer。

## 20.4 Timer

保留独立 timer queue。

## 20.5 Completion

完成后：

```text
native worker
↓
construct transit Message
↓
mailbox enqueue
↓
Actor scheduler
```

没有 VM callback。

---

# 21. Native FFI 新设计

## 21.1 `.omi` 保留

`.omi` 仍然是：

```text
OMBasic source semantics
vs
C ABI metadata
```

的明确边界。

## 21.2 删除 libffi runtime call engine

Native AOT 后，普通 Native function 应编译为直接 native call。

```text
OMBasic native code
↓
LLVM-generated C ABI call
↓
linked C symbol
```

不再：

```text
OMBasic
↓
VM Value marshal
↓
libffi
↓
C function
```

## 21.3 Link-time library resolution

`.omi` 的 library identity 在 build/link 阶段转换为 link input。

无需运行时 `dlopen` 作为默认模型。

动态插件系统若未来需要，应作为新 feature 单独设计。

## 21.4 Target-aware C ABI lowering

当前 `abi_type.c` 使用 compiler host ABI，这对 Native AOT target model 不够。

新：

```text
Target Triple
↓
Target C ABI
↓
LLVM-level ABI signature
```

需要处理：

- CInt；
- CLong；
- pointer width；
- scalar extension attributes；
- aggregate return；
- aggregate parameter；
- alignment；
- calling convention。

## 21.5 ABI target modules

按 target family 分：

```text
SysV AMD64
Win64 x64
AAPCS64
Darwin ARM64
```

不要写一个巨大 `if (platform)` 文件。

## 21.6 Native Record

Native Record by-value 需要正确 ABI classification。

必须用真实 C/C++ fixture 做 roundtrip 测试。

## 21.7 CString

生成 wrapper：

```text
OMBasic String
↓ explicit temporary conversion
CString
↓ native call
↓ cleanup temporary
```

Owned CString result：

```text
native ptr
↓ copy UTF-8 into OMBasic String
↓ release function
```

## 21.8 Native pointer

普通 OMBasic value model 不引入 raw pointer。

Pointer 只能存在于 generated FFI wrapper / Native resource boundary。

## 21.9 BLOCKING Native

规则：

```text
Actor RECEIVE -> BLOCKING Native : compile error
```

建议 diagnostic：

```text
E08xx: blocking Native call is not allowed inside an Actor handler;
route the operation through a System/Native Actor and receive its result as a Message.
```

Root Action 中可同步调用，但会真实阻塞当前 OS thread；没有隐藏 suspend/resume。

---

# 22. Native Callback

当前：

```text
foreign thread
↓
libffi closure
↓
om_vm_native_callback_post
```

新：

```text
foreign thread
↓
generated native C-ABI-compatible trampoline
↓
om_runtime_native_callback_post
↓
copy payload
↓
mailbox
```

## 22.1 Trampoline 由 compiler 生成

签名在 `.omi` 已知，因此无需 libffi 动态 closure。

## 22.2 Foreign thread rules

callback 不得：

- 读取 Actor state；
- 访问 Actor heap；
- 直接执行 handler；
- 使用 caller stack payload after return。

只允许复制 supported ABI values 后 enqueue。

## 22.3 Callback lifetime and payload

Callback payloads currently map scalar integer aliases, `CBool`, `CFloat`, and
`CDouble` fields into the bound Message. Pointer, CString, and Native Record
fields are unsupported.

A registration is scoped to one Native call. The Native library must stop and
join all callback threads before returning, and it must not call a trampoline
after return. Generated code unregisters immediately after the Native call;
the Runtime waits for in-flight ingress before releasing registration state.
Each ABI signature has 64 generated trampoline addresses, limiting that
signature to 64 simultaneous registrations.

---

# 23. Distribution 重构

## 23.1 保留 Actor-level semantics

- remote ActorRef；
- SEND；
- WATCH；
- Actor_Down；
- NODE_UNREACHABLE；
- at-most-once；
- same sender → same receiver ordering。

## 23.2 Program fingerprint 来源改变

旧：

```text
OMBC header
```

新：

```text
Om_Program_Descriptor
```

编译器在 AOT build 阶段生成。

## 23.3 Interface fingerprint

由：

```text
Message schema fingerprints
Actor protocol fingerprints
```

计算。

## 23.4 Serializer

不再接收 VM Value。

API 形态：

```text
serialize(
    Om_Type_Descriptor*,
    const void *native_value,
    buffer)
```

## 23.5 Decoder

目标是：

```text
wire payload
↓ descriptor validation
↓ allocate in transit storage
↓ native typed payload representation
```

## 23.6 Receiver thread

仍然只：

- network I/O；
- validate；
- decode/copy；
- mailbox ingress。

不得执行 Actor handler。

---

# 24. Runtime Fault 模型

Native AOT 不再能通过 VM `return structured runtime error` 自然退出任意 call stack。

必须重新设计。

## 24.1 普通可恢复错误仍使用 Result

```basic
TRY
Ok
Error
```

全部编译成普通 branch/return。

绝对不使用 runtime unwind。

## 24.2 Runtime fault

例如：

- integer overflow；
- divide by zero；
- index out of bounds；
- internal limit exceeded；
- invalid runtime state。

推荐使用严格限制的 implementation trap boundary。

## 24.3 Trap boundary

每个：

```text
root Action
Actor handler
```

入口建立 runtime fault boundary。

底层 runtime check 失败调用：

```text
om_runtime_fault(context, code, source_span_id)
```

第一版可以内部使用受限 `setjmp/longjmp`，但只能存在于：

```text
runtime/fault/
```

且：

- 不用于普通语言控制流；
- 不用于 `TRY`；
- 不用于 `RETURN`；
- 不用于 `STOP`；
- fault 前必须恢复 GC root chain 到 boundary saved base；
- generated code 不允许持有需要 C destructor/manual cleanup 的局部资源。

如果不接受 `setjmp/longjmp`，替代方案是给所有 generated function 增加显式 runtime-status channel，但实现成本和 ABI 噪音显著更高。

本计划推荐前者作为 0.1 implementation mechanism。

---

# 25. `TRY` Native Lowering

示例：

```basic
user = TRY find_user(id)
```

HIR：

```text
v_result = Call find_user(id)
v_user = Try v_result
```

MIR：

```text
v_result = call find_user
v_tag = result_tag v_result
cond_br v_tag, ok_block, error_block

ok_block:
    v_user = result_ok_payload v_result
    ...

error_block:
    v_error = result_error_payload v_result
    return make_error(v_error)
```

LLVM：普通 CFG。

没有 exception、unwind、longjmp。

---

# 26. `SEND` Native Lowering

```basic
SEND worker, Work(item)
```

HIR：

```text
message = MessageConstruct Work(item)
Send worker, message
```

MIR：

```text
payload = alloc/init Work
call runtime_actor_send(
    context,
    worker,
    &Work_descriptor,
    payload)
```

LLVM 只负责生成 function call。

---

# 27. `SPAWN` Native Lowering

```basic
worker = SPAWN Worker
```

MIR：

```text
worker = runtime_actor_spawn(
    context,
    &Worker_actor_descriptor)
```

Runtime 根据 descriptor：

- allocate Actor state；
- create heap；
- run initializer；
- return ActorRef。

---

# 28. `WITH` Native Lowering

```basic
older = user WITH age = 21
```

第一版：

```text
allocate new Record object
copy all fields
replace age
```

未来可由 LLVM + escape analysis / custom optimization 改善。

---

# 29. Source Location / Debug Info

旧 VM 使用：

```text
bytecode PC -> source map
```

删除。

新分两层：

## 29.1 Runtime fault span IDs

Compiler 生成：

```text
static source span table
```

runtime check 调用携带：

```text
Span_Id
```

发生 runtime fault 可以打印：

```text
file.bas:line:column
```

## 29.2 Native debugger info

LLVM DIBuilder 生成：

- compile unit；
- source file；
- function；
- lexical scope；
- line mapping。

目标：

```text
lldb/gdb/Visual Studio
```

可以看到 OMBasic source line。

Phase 初期先完成 line tables，再扩展变量 debug info。

---

# 30. CLI 重构

## 30.1 `ombasic check`

执行：

```text
load -> syntax -> semantic -> HIR validation(optional)
```

不需要 LLVM。

## 30.2 `ombasic build`

默认：

```text
native executable
```

## 30.3 `ombasic run`

严格 AOT：

```text
compile native executable in temp build dir
↓
spawn executable
↓
forward exit code/stdout/stderr
```

不使用：

- VM；
- JIT；
- interpreter。

## 30.4 Debug emit options

```text
ombasic build --emit=hir
ombasic build --emit=mir
ombasic build --emit=llvm-ir
ombasic build --emit=asm
ombasic build --emit=obj
```

这些是 compiler/debug artifacts，不是语言运行制品。

## 30.5 `ombasic fmt`

保留。

## 30.6 `ombasic bind`

保留 `.omi` 生成工具，但生成后需审核 effect/blocking/ownership。

## 30.7 `ombasic check-omi`

保留。

## 30.8 `ombasic lsp`

继续只依赖 frontend/semantic。

禁止 LSP 自己复制 HIR/LLVM 逻辑。

---

# 31. Whole-program Compilation 第一阶段策略

第一版 Native AOT 建议：

> **whole-program compile**

Module graph 仍然存在，但最终：

```text
all module HIR
↓
program specialization
↓
program MIR
↓
one LLVM module (or tightly controlled module set)
↓
one native object
```

原因：

- generic monomorphization 简单；
- Actor descriptor registry 简单；
- program/interface fingerprint 简单；
- no stable cross-object OMBasic ABI needed；
- no incremental object cache complexity。

在 Native baseline 稳定后，再引入 per-module object/incremental compile。

---

# 32. Symbol Naming

生成 native symbol 不直接使用用户名称作为唯一 linker symbol。

内部：

```text
_om_<kind>_<stable-id>_<readable-name>
```

例如：

```text
_om_fn_0031_calculate_total
_om_actor_0012_Payment_Service
_om_msg_0045_Payment_Accepted
```

这样：

- 避免 module collision；
- debug 仍然可读；
- 不暴露 C ABI 稳定承诺。

---

# 33. CMake 全面重写

不要在当前 20k+ `CMakeLists.txt` 上继续叠加 Bytecode 和 Native 两套 target。

建议重写根 CMake。

## 33.1 主要 targets

```text
ombasic_foundation
ombasic_source
ombasic_syntax
ombasic_module_graph
ombasic_native_metadata
ombasic_semantic
ombasic_hir
ombasic_mir
ombasic_target
ombasic_backend_llvm
ombasic_linker
ombasic_compiler
ombasic_runtime
ombasic_distribution
ombasic_tooling
ombasic
```

删除：

```text
ombasic_bytecode
old ombasic_runtime VM coupling
old ombasic_compiler native_binding target
```

## 33.2 LLVM discovery

```cmake
find_package(LLVM 23 REQUIRED CONFIG)
```

检查：

```text
LLVM_PACKAGE_VERSION major == 23
```

开发基线测试 23.1.4。

## 33.3 libffi

完全移除：

```text
find libffi
OM_LIBFFI_ROOT
ffi.h copy workaround
libffi link
```

## 33.4 Runtime library

Runtime 作为 static library target。

Compiler executable 与 Runtime source 不互相依赖。

Compiler build target只需要知道 runtime archive output 与 Runtime ABI header。

---

# 34. Architecture Checker 重构

现有 checker 已检查：

- raw allocator；
- unsafe APIs；
- include direction；
- private header。

新 checker 增加：

## 34.1 CMake target graph validation

不能只扫 `#include`。

必须验证 target dependencies。

## 34.2 Legacy symbol ban

重构完成后 CI 中禁止出现：

```text
Om_VM
om_vm_
Om_Bytecode
om_bytecode_
OM_BYTECODE_
OMBC
LOAD_SYSTEM_ACTOR opcode
```

允许出现的地方只有：

```text
Git history（不扫描）
重构计划文档（可加入 ignore）
```

## 34.3 LLVM boundary

只有：

```text
src/backend/llvm/
```

允许 include：

```text
llvm-c/*
```

## 34.4 Runtime/compiler boundary

Runtime 不能 include：

```text
ombasic/semantic
ombasic/ir
llvm-c
```

## 34.5 Raw allocator

继续限制 allocator owner。

新的 GC heap 是合法 allocator owner。

---

# 35. C23 Safety Profile 顺便修正

当前工程要求 `<stdckdint.h>` 必须存在；实际 GCC 13 环境已出现 header 缺失问题。

重构中增加：

```text
foundation/checked_int
```

API：

```text
om_checked_add_size
om_checked_mul_size
...
```

实现：

```text
有 stdckdint.h -> ckd_*
否则受支持 compiler builtin -> __builtin_*_overflow
```

上层不直接 include `<stdckdint.h>`。

这不是旧行为兼容，而是收窄 C safety contract。

---

# 36. Tests：哪些保留

完整保留并继续通过：

```text
foundation tests
source tests
lexer tests
parser tests
module graph tests
name resolution tests
type inference tests
effect tests
actor protocol semantic tests
serializable semantic tests
formatter tests
conformance lexical/semantic fixtures
```

---

# 37. Tests：哪些删除

直接删除：

```text
bytecode format golden
OMBC version tests
bytecode verifier tests
bytecode mutation tests
VM opcode tests
VM stack tests
VM frame tests
VM image validation tests
VM instruction limit tests
VM instruction quantum tests
```

不转换成“legacy regression”。

---

# 38. 新增 HIR Tests

每个语言 feature 至少：

```text
source
↓
expected normalized HIR dump
```

重点：

- Try；
- With；
- Function/Action；
- Result；
- Actor；
- Send；
- Spawn；
- Watch；
- System Actor；
- Native declaration references。

HIR dump 必须 deterministic。

---

# 39. 新增 MIR Tests

Golden MIR：

- control flow；
- short-circuit；
- loops；
- Result propagation；
- checked arithmetic；
- record construction；
- actor intrinsics；
- GC root planning；
- Native call lowering。

MIR verifier 必须对人为构造 invalid MIR 有失败测试。

---

# 40. LLVM Backend Tests

## 40.1 IR verification

所有 generated LLVM module 必须过：

```text
LLVMVerifyModule
```

## 40.2 IR pattern tests

不建议对完整 LLVM IR 做极度脆弱的全文 golden。

对关键结构做 normalize/pattern assertion：

- overflow intrinsic；
- runtime send call；
- actor descriptor；
- GC root push/pop；
- native external declaration。

## 40.3 Object smoke

生成 `.o/.obj` 后使用 LLVM tools 验证 object 可读。

---

# 41. Native Integration Tests

取代 VM integration。

每个 fixture：

```text
.bas
↓ ombasic build
native executable
↓ execute as child process
stdout/stderr/exit code
```

覆盖：

- recursion；
- loops；
- Record；
- List/Map；
- Result/TRY；
- interpolation；
- runtime faults；
- Actor；
- system Actor；
- Native FFI；
- distribution。

---

# 42. GC Native Tests

除 unit test 外必须有 compiled native program tests：

- deep object graph；
- cycles；
- dead cycles；
- nested Record/List/Map；
- Result containing object；
- Actor state roots；
- function parameters roots；
- repeated allocation collection；
- message transfer；
- Actor stop release。

---

# 43. Actor Native Tests

需要证明：

1. 同一 Actor 不并行执行两个 handler。
2. 不同 Actor 可并行。
3. handler 真正 native stack run-to-completion。
4. self-send 排到当前 handler 之后。
5. STOP 丢弃 pending mailbox（按规范）。
6. failed Receive 产生 Actor_Down FAILED。
7. WATCH semantics。
8. Actor state isolation。
9. millions-of-message stress。
10. 无 VM instruction quantum 依赖。

---

# 44. FFI Tests

必须编译真实 C shared/static fixtures。

覆盖：

- signed/unsigned integer widths；
- Float/Double；
- Bool；
- CString；
- owned return；
- release function；
- Native Record padding；
- struct return；
- C++ `extern "C"` wrapper；
- callback foreign thread；
- blocking call prohibition in Actor；
- callback → Message ingress。

每个 target ABI 进入独立 test group。

---

# 45. Distribution Tests

保留/重写：

- frame bounds；
- malformed frame；
- handshake；
- fingerprint mismatch；
- message schema mismatch；
- remote Send；
- remote Watch；
- Actor_Down；
- Node unreachable；
- remote spawn；
- ordering；
- at-most-once；
- large nested payload budget。

但 payload 必须来自 AOT descriptors，而非 VM value tree。

---

# 46. Fuzzing

保留：

```text
lexer bytes
parser malformed source
import paths
OMI parser
distribution frame decoder
```

新增：

```text
HIR verifier constructed inputs
MIR verifier constructed inputs
type descriptor serializer/decoder
```

LLVM backend fuzz 不直接执行 arbitrary generated native code；先：

```text
build HIR/MIR
LLVMVerifyModule
object emit
```

作为 compile-crash fuzz。

---

# 47. Sanitizer Strategy

Compiler/Runtime C 源继续：

```text
ASan + UBSan
TSan
MSan where practical
```

Native-generated OMBasic code第一阶段不要求完整 sanitizer instrumentation。

Runtime library本身必须 sanitizer-clean。

后续可增加 compiler option：

```text
--sanitize=address
--sanitize=thread
```

通过 LLVM pass pipeline 对 generated native code 插桩。

不是 Native AOT 首个 Gate。

---

# 48. Documentation 全面清理

## 删除

```text
src/bytecode/README.md
Bytecode format docs
OMBC docs
VM runtime README
old Bytecode/VM development phases
```

## 重写

```text
docs/architecture/overview.md
docs/architecture/dependency_rules.md
docs/development/implementation_status.md
docs/development/testing_strategy.md
README.md
```

README 新 build requirement：

```text
C23 compiler
CMake
LLVM 23
LLD
Python (tooling)
```

删除 libffi requirement。

---

# 49. ADR 全面重置

因为明确不考虑兼容性，建议不保留大量 `Superseded` VM ADR 在主架构目录中。

Git history已经保存。

新的 ADR 目录建议重新整理：

```text
ADR-0001 C23 Implementation Baseline
ADR-0002 Explicit Architecture
ADR-0003 Native AOT with LLVM
ADR-0004 Compiler Arena / Stable IDs
ADR-0005 Immutable AST + Semantic Side Tables
ADR-0006 HIR / MIR Boundaries
ADR-0007 Generic Specialization
ADR-0008 Runtime ABI / Program Descriptor
ADR-0009 Native Object Model / Shadow Root GC
ADR-0010 Native Actor Scheduler / Run-to-Completion
ADR-0011 System Actors / Blocking Pool
ADR-0012 Native C ABI / OMI
ADR-0013 Native Callback Ingress
ADR-0014 Distribution Descriptors / Fingerprints
ADR-0015 Runtime Fault Boundary
ADR-0016 Native Linking / Artifact Model
ADR-0017 Diagnostics / Native Debug Info
```

---

# 50. 需要删除的旧 ADR / 概念

当前与 VM/Bytecode 强绑定的 ADR 不进入新 main tree architecture set：

```text
ADR-0003 old custom bytecode VM
ADR-0010 old bytecode format
ADR-0011 old single-thread VM
ADR-0012 old VM-backed actor scheduler
ADR-0013 old VM-root-based actor GC
ADR-0014 old instruction-quantum scheduler
ADR-0015 old VM system actor integration
ADR-0016 old libffi/VM native boundary
ADR-0018 old OMBC schema fingerprints
```

其他 ADR 内容可重新编号/整理后保留。

---

# 51. Phase 0 — Architectural Reset

### 操作

1. 新建 native-aot 重构分支。
2. 记录当前 master commit 作为 VM 最终历史节点。
3. 更新 README 标注 Native AOT migration。
4. 写新 ADR-0003 Native AOT with LLVM。
5. 写新 dependency graph。
6. 写 Runtime ABI 草案。
7. 写 HIR/MIR 草案。

### Gate

必须先回答：

- Native data representation；
- managed pointer model；
- shadow roots；
- Actor handler ABI；
- Message descriptor；
- Program descriptor；
- LLVM version；
- linker model。

没有这些，不开始 codegen。

---

# 52. Phase 1 — Destructive VM / OMBC Removal

这是故意让仓库暂时只剩 frontend/check 的阶段。

### 删除

```text
src/bytecode/**
include/ombasic/bytecode/**
tests/bytecode/**
src/runtime/vm/**
src/runtime/vm_internal.h
include/ombasic/runtime/vm.h
src/compiler/native_binding/**
include/ombasic/compiler/native_binding.h
src/runtime/distribution/vm_distribution.c
```

### CMake 删除

```text
ombasic_bytecode
old ombasic_runtime VM sources
old compiler native binding target
VM tests
bytecode tests
```

### CLI

暂时只保证：

```text
ombasic check
ombasic fmt
ombasic check-omi
ombasic lsp
```

`build/run` 暂时明确报：

```text
Native backend not implemented
```

这是开发阶段，不发布。

### Gate

- project builds；
- frontend/semantic tests pass；
- architecture checker确认不存在 Bytecode/VM dependency。

---

# 53. Phase 2 — Clean Native Metadata Capability

### 工作

- 移动 `.omi` parser/project loader 到 `native/metadata`；
- 删除 libffi/dlopen execution layer；
- semantic 继续读取 OMI metadata；
- effect/blocking/ownership 仍静态可查；
- `ombasic bind` / `check-omi` 恢复。

### Gate

- OMI parser tests；
- Native metadata 不依赖 Runtime；
- Runtime 不依赖 Native metadata parser。

---

# 54. Phase 3 — HIR

### 工作

- HIR storage；
- AST → HIR lowering；
- HIR dump；
- HIR verifier；
- source spans；
- semantic IDs/types/effects 绑定。

### Gate

所有现有 non-runtime conformance fixtures 能生成 deterministic HIR。

---

# 55. Phase 4 — Specialization

### 工作

- Function Instance ID；
- type argument canonicalization；
- worklist；
- recursive calls；
- generic recursion diagnostics；
- concrete Result/collection descriptors。

### Gate

当前 polymorphic identity 等 semantic fixture 能产生有限 concrete instances。

---

# 56. Phase 5 — MIR

### 工作

- CFG；
- block parameters；
- Value IDs；
- MIR types；
- high-level lowering；
- runtime intrinsics；
- MIR verifier；
- MIR dump。

### Gate

无 Actor、无 Native 的语言子集完整进入 MIR。

---

# 57. Phase 6 — Minimal LLVM Native Backend

先只做：

```text
Int
Float
Bool
Function
Action
If
While
For
Call
Return
checked arithmetic
```

输出：

```text
.o/.obj
```

然后 link hello-world executable。

### Gate

```basic
ACTION main()
    print("Hello")
END ACTION
```

通过真正 native executable 执行。

如果 `print` 此时还未就绪，可用极小 runtime console helper。

---

# 58. Phase 7 — Native Object Model + GC

### 工作

- Runtime ABI；
- String；
- Record；
- List；
- Map；
- type descriptors；
- shadow root stack；
- non-moving mark/sweep；
- root action heap。

### Gate

所有现有 non-Actor VM integration 语义 fixture 转成 native integration 并通过。

---

# 59. Phase 8 — Result / TRY / Runtime Faults

### 工作

- concrete Result layout；
- TRY CFG；
- checked runtime faults；
- source span table；
- root/handler fault boundary。

### Gate

- Error propagation；
- divide zero；
- overflow；
- bounds fault；
- source location。

---

# 60. Phase 9 — Native Actor Runtime

### 工作

- Program descriptor actor tables；
- Actor descriptor；
- native handler function pointers；
- Actor state layout；
- mailbox；
- scheduler；
- spawn/send/watch/stop；
- ActorDown。

### 删除最后 VM scheduler assumptions

- instruction count；
- instruction quantum；
- saved continuation；
- VM worker stack/frame。

### Gate

全部 Actor semantic/runtime fixtures native 通过。

---

# 61. Phase 10 — System Actors

### 工作

- Console；
- File_System；
- Timer；
- blocking pool；
- timer service；
- message completion；
- runtime shutdown。

### Gate

system actor native fixture 完成文件读写/Timer/Console。

---

# 62. Phase 11 — Direct Native FFI

### 工作

- target C ABI；
- external LLVM declarations；
- linker native libs；
- CString；
- scalar calls；
- Native Records；
- ownership；
- generated callback trampolines；
- callback-to-Message ingress from foreign threads, limited to supported scalar payloads；
- call-scoped registrations that are unregistered after Native returns, after all callback threads have been joined；
- concurrent callback registration and ingress checks on Windows and Linux。

### Gate

不链接 libffi；C/C++ Native roundtrip 与 foreign-thread callback-to-Message fixture 在 Windows clang-cl/LLD 和 Linux Clang/LLD 上通过。

---

# 63. Phase 12 — Distribution Native Integration

### 工作

- AOT descriptor serializer；
- program/interface fingerprints；
- runtime peer registry；
- remote ActorRef；
- SEND/WATCH/ActorDown；
- remote spawn。

### Gate

两个 native executable / 两个 runtime node localhost integration 通过。

---

# 64. Phase 13 — CLI / Tooling Completion

恢复：

```text
check
build
run
fmt
bind
check-omi
lsp
version
```

`build` native-only。

`run` AOT-only。

删除所有 bytecode artifact wording。

---

# 65. Phase 14 — Native Debugging

### 工作

- LLVM line table；
- function names；
- source files；
- stack traces；
- runtime fault source spans。

### Gate

GDB/LLDB/Visual Studio 至少能从 crash/fault stack 定位到 `.bas` 行。

---

# 66. Phase 15 — Release Gates

必须：

- no VM/Bytecode symbols；
- no libffi；
- architecture checks；
- ASan/UBSan compiler+runtime；
- TSan Actor runtime；
- fuzzers；
- native integration；
- foreign-thread callback-to-Message integration on Windows and Linux；
- callback integration under Linux ASan/UBSan and TSan；
- FFI；
- distribution；
- formatter idempotence；
- LSP frontend tests。

---

# 67. 第一批 PR 顺序

建议每个 PR 都可独立 review：

1. `ADR: Native AOT architecture reset`
2. `Delete OMBC/bytecode capability`
3. `Delete VM public API and VM implementation`
4. `Reduce CMake to frontend/semantic/tooling-check baseline`
5. `Split OMI metadata from runtime FFI`
6. `Add runtime ABI headers`
7. `Add typed HIR storage and dump`
8. `Lower AST+semantic to HIR`
9. `Add HIR verifier`
10. `Add generic specialization`
11. `Add MIR storage/CFG`
12. `Lower HIR to MIR`
13. `Add MIR verifier`
14. `Integrate LLVM 23 C API`
15. `Emit scalar native object`
16. `Add LLD linker capability`
17. `Generate native entry point`
18. `Add native object model`
19. `Add shadow-root GC`
20. `Lower Record/List/Map/Result`
21. `Add native runtime faults`
22. `Rewrite Actor runtime around native handlers`
23. `Restore System Actors`
24. `Implement direct C ABI Native calls`
25. `Generate Native callback trampolines`
26. `Rewrite distribution around descriptors`
27. `Restore build/run CLI`
28. `Add LLVM debug info`
29. `Reset documentation/implementation status`
30. `Native AOT release gate`

---

# 68. 第一批 Issue 列表

## Architecture

- Define Runtime ABI ownership rules.
- Define `Om_Execution_Context`.
- Define Type/Message/Actor descriptors.
- Define Program Descriptor.
- Define Native handler ABI.
- Define runtime fault boundary.
- Define HIR invariants.
- Define MIR invariants.
- Define target support policy.

## Deletion

- Remove OMBC files and public headers.
- Remove bytecode CMake target.
- Remove bytecode tests.
- Remove VM public API.
- Remove vm.c/vm_internal.h.
- Remove VM value/heap naming.
- Remove compiler native VM binding.
- Remove VM distribution adapter.
- Remove libffi dependency.

## HIR/MIR

- HIR IDs/storage.
- AST→HIR expressions.
- AST→HIR statements.
- HIR Actors/Messages.
- HIR verifier.
- Specialization worklist.
- MIR CFG.
- MIR block params.
- MIR runtime intrinsics.
- MIR verifier.

## LLVM

- LLVM discovery/version gate.
- LLVM context/module lifecycle.
- MIR scalar lowering.
- checked integer intrinsics.
- LLVM verification.
- target machine.
- object emission.
- LLD invocation.
- native entry generation.

## Runtime

- native object header.
- type descriptors.
- string/list/map.
- shadow root stack.
- mark/sweep.
- Actor registry.
- mailbox transit ownership.
- native handler scheduler.
- system actor refs.
- runtime fault reporting.

## FFI

- target-aware ABI scalar mapping.
- direct external function lowering.
- CString wrapper.
- Owned return cleanup.
- Native Record ABI classification.
- callback trampoline generation.
- blocking Native actor diagnostic.

## Distribution

- descriptor serializer.
- descriptor decoder.
- program fingerprint generation.
- runtime peer routing.
- remote ActorRef.
- native remote Watch.

---

# 69. Definition of Done — Native Compiler

Native compiler完成必须满足：

- `.bas` 不生成 C；
- `.bas` 不生成 OMBC；
- compiler 生成 object；
- linker 生成 executable；
- executable 运行不需要 `ombasic`；
- executable 运行不需要 LLVM；
- executable 运行不存在 VM interpreter；
- all language semantics originate in Semantic/HIR/MIR；
- LLVM backend 只执行 mechanical lowering；
- generated code calls stable internal Runtime ABI。

---

# 70. Definition of Done — Runtime

Runtime 不包含：

```text
PC
opcode
bytecode image
operand stack
VM frame
instruction quantum
```

Runtime 包含：

```text
execution context
object heap
GC roots
Actor state
mailbox
scheduler
system actor services
native callback ingress
distribution peers
```

---

# 71. Definition of Done — Architecture

Architecture checker 必须能自动证明：

- no Bytecode capability；
- no VM capability；
- no runtime→compiler dependency；
- no semantic→LLVM dependency；
- only backend/llvm includes LLVM headers；
- only approved memory owners use raw allocators；
- no cross-capability internal header access；
- CMake target graph follows declared DAG。

---

# 72. Definition of Done — Language Philosophy

新的实现应该让 OMBasic 的语言原则和 Runtime 实现一致：

> 值默认稳定，变化必须可见。<br>
> 状态必须有主人。<br>
> 控制流必须能看见。<br>
> Actor handler 不存在隐藏 continuation。<br>
> Native blocking 不会偷偷 suspend/resume。<br>
> 类型由编译器理解，代码由人理解。

Native AOT 应强化这些原则，而不是通过 LLVM 引入另一套隐藏语义。

---

# 73. 本次明确不做

为了避免重构范围失控，Native AOT baseline 不同时做：

- JIT；
- ORC；
- generated C；
- custom x86 assembler；
- custom ELF/COFF writer；
- custom linker；
- LLVM custom optimization passes；
- moving GC；
- LLVM statepoint GC；
- work-stealing scheduler；
- shared immutable heap；
- LTO；
- incremental native object cache；
- hot reload；
- dynamic plugin ABI；
- backward OMBC compatibility。

---

# 74. 后续优化路线（Native baseline 完成后）

按 benchmark 决定：

1. escape analysis；
2. stack allocation of non-escaping Records；
3. scalar replacement；
4. devirtualized generic collection helpers；
5. per-module native object cache；
6. ThinLTO；
7. actor scheduling improvements；
8. immutable message sharing；
9. precise GC liveness；
10. LLVM statepoint / moving GC only if necessary。

每一个优化必须：

```text
benchmark evidence
+
ADR
+
correctness test
```

---

# 75. 关键风险

## 75.1 C ABI aggregate lowering

这是 Native FFI 最大技术风险。

不要假设：

```text
LLVM struct == C ABI struct call convention
```

必须实现 target-specific ABI classification，并用真实 C fixture 验证。

## 75.2 Native GC roots

VM 删除后 root tracking 不能遗漏。

第一版 shadow root stack 比 LLVM stackmap 更保守但风险更低。

## 75.3 Actor runtime 与 native stack

不能为了复制旧 instruction quantum 而引入 fiber/coroutine。

这会重新制造隐藏 continuation。

## 75.4 FFI ownership

Native pointer 不能污染普通 OMBasic value graph。

## 75.5 LLVM dependency leakage

如果 semantic/HIR 开始直接创建 LLVM values，架构会迅速失控。

严格限制 LLVM headers。

## 75.6 `vm.c` 诱惑

当前 `vm.c` 已经实现很多功能，最危险的重构方式是：

> 一点一点改名，最终得到一个叫 Runtime 的 VM。

因此应删除并按 Runtime ABI 重建 Actor/scheduler/GC integration。

---

# 76. 推荐的“删除后第一天仓库状态”

完成 Phase 1 后仓库应该故意非常简单：

```text
Source
Syntax
Module Graph
Semantic
Formatter
LSP/check
OMI metadata
```

可以：

```text
ombasic check
ombasic fmt
```

暂时不能：

```text
ombasic build
ombasic run
```

然后从 HIR → MIR → LLVM 开始重新建立执行链。

这比同时维护新旧两套 execution path 更容易验证架构正确性。

---

# 77. 最终目标目录中的“不应该存在”

重构完成后执行：

```text
grep -R "Om_VM\|om_vm_\|Om_Bytecode\|OM_BYTECODE\|OMBC" src include tests
```

结果应为空。

执行：

```text
grep -R "libffi\|ffi_call\|ffi_prep_cif" src include CMakeLists.txt cmake
```

结果应为空（`.omi` 的 FFI 文档文字除外）。

执行：

```text
find src -maxdepth 2 -type d
```

应该清晰看到：

```text
ir
backend
runtime
target
linker
```

而不是：

```text
bytecode
vm
```

---

# 78. Native AOT 最终架构图

```text
                         ┌────────────────────────┐
                         │      .bas source       │
                         └───────────┬────────────┘
                                     │
                            ┌────────▼────────┐
                            │ Source / Module │
                            └────────┬────────┘
                                     │
                            ┌────────▼────────┐
                            │ Lexer / Parser  │
                            └────────┬────────┘
                                     │
                            ┌────────▼────────┐
                            │      AST        │
                            └────────┬────────┘
                                     │
                      ┌──────────────▼──────────────┐
                      │ Semantic / Type / Effects  │
                      └──────────────┬──────────────┘
                                     │
                            ┌────────▼────────┐
                            │   Typed HIR     │
                            └────────┬────────┘
                                     │
                            ┌────────▼────────┐
                            │ Specialization │
                            └────────┬────────┘
                                     │
                            ┌────────▼────────┐
                            │  Native MIR     │
                            └────────┬────────┘
                                     │
                            ┌────────▼────────┐
                            │  LLVM Backend   │
                            └────────┬────────┘
                                     │
                            ┌────────▼────────┐
                            │ Object / ASM    │
                            └────────┬────────┘
                                     │
                            ┌────────▼────────┐
                            │      LLD        │
                            └────────┬────────┘
                                     │
                           ┌─────────▼─────────┐
                           │ Native Executable │
                           └─────────┬─────────┘
                                     │
             ┌───────────────────────▼────────────────────────┐
             │             libombasic_runtime                 │
             │                                                │
             │ GC │ Actor │ Mailbox │ Scheduler │ System I/O │
             │ Native callback │ Distribution │ Diagnostics  │
             └────────────────────────────────────────────────┘
```

---

# 79. 最终结论

本次重构不是：

> “给现有 VM 再增加一个 Native backend。”

而是：

> **删除 VM 作为程序执行模型，以 Typed HIR / Native MIR 作为 OMBasic 编译器自己的中间语义，以 LLVM 作为纯机器码后端，以独立 Runtime ABI 连接 native executable 与 Actor/GC/Distribution runtime。**

最重要的结构变化：

```text
旧：
AST/Semantic
↓
Bytecode
↓
VM
↓
Runtime behavior

新：
AST/Semantic
↓
HIR
↓
MIR
↓
LLVM
↓
Native code
↓
Explicit Runtime ABI
```

这个架构更符合 OMBasic 的长期定位：

- 静态类型；
- Native executable；
- 明确 C ABI；
- Actor isolation；
- 无隐藏 Await；
- 无 VM artifact dependency；
- 无 interpreter dispatch overhead；
- 编译期语义与运行时职责明确分离；
- Explicit Architecture 的 capability boundary 可以真正自动验证。

---

# 80. 参考基线

## 当前 OMBasic 源码

- https://github.com/risol/OMBasic
- 基线 commit：`ecbbacc5fb235c4a706a9c4eeb31bc7b41dec299`

## Explicit Architecture

- https://github.com/risol/ExplicitArchitecture

## LLVM

- https://llvm.org/
- 当前开发基线：LLVM 23.1.4（2026-10-20 发布）
