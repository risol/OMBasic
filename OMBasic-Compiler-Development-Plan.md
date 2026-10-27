# OMBasic 编译器与运行时完整开发计划

**项目：** OMBasic（Oh My BASIC）参考编译器与运行时  
**实现语言：** C23  
**目标规范：** OMBasic 0.1 Language Standard  
**文档状态：** Implementation Plan / Architecture Baseline  
**日期：** 2026-10-27  
**架构方法：** 采用 Explicit Architecture V1.1 原则，并针对编译器/语言运行时领域做适配

---

## 0. 执行摘要

OMBasic 的编译器实现应当和 OMBasic 语言本身遵循同一种思想：

> **结构可见、行为显式、依赖清晰、变化局部、约束可执行。**

第一版不追求“功能最多”或“性能最强”，而追求以下五件事：

1. **规范可信**：编译器行为能够稳定、准确地实现 OMBasic 0.1 规范。
2. **代码可审查**：人类或 AI 可以在有限上下文中理解某个编译阶段或 Runtime 能力。
3. **内存安全风险受控**：虽然使用 C23，实现必须通过受限 C 子集、Arena、Slice、Handle、checked arithmetic、Sanitizer 与 fuzzing 降低风险。
4. **Actor 语义正确优先**：先实现单线程确定性 Actor Runtime，再扩展到 M:N 多线程与分布式。
5. **架构约束可以自动验证**：模块依赖方向、危险 API 使用、测试层级、FFI 边界均进入 CI，而不是只写在文档中。

推荐总体路线：

```text
语言规范冻结
    ↓
C23 Foundation
    ↓
Source / Module Loader
    ↓
Lexer
    ↓
Parser / AST
    ↓
Name Resolution
    ↓
Type Inference
    ↓
Effect / Actor / Serialization Analysis
    ↓
Bytecode Compiler
    ↓
Single-thread VM + Actor Runtime
    ↓
Actor-local GC
    ↓
Multi-thread Scheduler
    ↓
System Actors / Native FFI
    ↓
Distributed Runtime
    ↓
Formatter / LSP / Incremental Build
```

第一版后端建议采用 **自定义 Bytecode VM**，而不是立即依赖 LLVM。这样更容易保持控制流、Actor 调度、GC、消息传输和调试模型的可见性，也符合 Explicit Architecture 的“必要复杂度”原则。

---

# 1. 项目目标

## 1.1 编译器目标

参考编译器必须支持 OMBasic 0.1 标准中的核心能力：

- `.bas` 源文件；
- UTF-8；
- 全大写关键字；
- `lower_snake_case` 普通名称；
- `Pascal_Snake_Case` 实体名称；
- 路径式 `IMPORT`；
- 文件即 Module；
- 默认不可变绑定；
- `VAR` 显式可变；
- `RECORD`；
- `MESSAGE`；
- `FUNCTION`；
- `ACTION`；
- `RESULT / TRY` 错误模型；
- 静态类型推导；
- 泛型推导的最小子集；
- Function purity；
- Actor protocol 静态检查；
- `ACTOR / SPAWN / SEND / RECEIVE / WATCH`；
- Receive run-to-completion；
- 无 `AWAIT` / `ASYNC`；
- Actor 隔离；
- Native C ABI；
- C++ 经 C wrapper 接入；
- 分布式 ActorRef 与消息序列化。

## 1.2 非目标

以下内容明确不属于第一阶段目标：

- LLVM native optimization backend；
- JIT；
- 自举编译器；
- 宏系统；
- 用户元编程；
- 反射；
- Class / inheritance；
- shared mutable heap；
- lock-free everything；
- generational GC；
- exactly-once delivery；
- 分布式一致性数据库；
- 自动服务发现；
- 完整 IDE；
- C++ ABI 直接绑定；
- 复杂编译期执行。

这些内容只有在出现明确需求时才重新评估。

---

# 2. 成功标准

OMBasic 0.1 参考实现进入“可发布”状态前，至少满足以下条件。

## 2.1 语言一致性

- 所有规范关键字、命名规则和语法均有 conformance test。
- 非法语法必须产生确定性诊断，不得 crash。
- Import graph 循环必须在编译期拒绝。
- Function purity 必须在编译期验证。
- Message serializability 必须在编译期验证。
- Actor protocol 发送错误 Message 必须在编译期拒绝。
- 不存在隐藏 Await/continuation 语义。

## 2.2 稳定性

对任意输入字节流：

```text
lexer/parser/type checker
```

只能：

```text
成功
或
返回诊断
```

不得出现：

- segmentation fault；
- use-after-free；
- double free；
- buffer overflow；
- assertion caused by malformed user input；
- undefined behavior。

## 2.3 Runtime 正确性

- 单 Actor 任意时刻最多被一个 worker 执行。
- Receive 处理期间不得处理第二条 Message。
- 本地与远程 Message 必须遵循相同的数据合法性规则。
- Actor 普通可变对象不得被其他 Actor 直接持有指针。
- Actor-local GC 不得要求全 Node stop-the-world。
- 外部 native callback 不得直接重入 Actor handler。

## 2.4 工程质量

- 核心模块无循环编译依赖。
- 每个能力模块具有明确 README 或 module doc。
- 每个模块拥有自己的单元测试。
- ASan + UBSan 运行全部核心测试。
- TSan 运行并发 Runtime 测试。
- lexer/parser/module loader 持续 fuzz。
- 架构检查进入 CI。

---

# 3. Explicit Architecture 适配方案

本项目采用 `risol/ExplicitArchitecture` V1.1 的核心原则，但不机械照搬业务系统目录。

参考：

- https://github.com/risol/ExplicitArchitecture
- https://github.com/risol/ExplicitArchitecture/blob/master/README.zh-CN.md

## 3.1 Capability & Kernel Boundaries

OMBasic 工程划分为：

```text
Compiler Capabilities
Runtime Capabilities
Shared Foundation
Composition Roots
```

### Compiler Capabilities

- Source Loading
- Syntax
- Module Graph
- Semantic Analysis
- Bytecode Generation
- Diagnostics

### Runtime Capabilities

- Value/Object Model
- VM Execution
- GC
- Actor
- Scheduler
- Native FFI
- Distribution

### Shared Foundation

只允许放真正跨能力共享且无领域语义的机制：

- memory primitives；
- arena；
- slice；
- vector；
- hash table；
- IDs；
- UTF-8 utilities；
- checked integer helpers；
- platform abstraction。

不得创建：

```text
common/
utils/
misc/
helpers/
```

作为任意代码的垃圾桶。

## 3.2 Structural Transparency

目录结构必须让阅读者不打开大量文件即可看出：

- 编译器有哪些阶段；
- Runtime 有哪些核心能力；
- CLI 在哪里组装；
- 哪些模块属于 Native/Network 边界。

因此不使用“所有源码平铺在 src/”的结构。

## 3.3 Responsibility Cohesion

一个模块是否拆分，不按行数决定，而按：

- 是否拥有独立不变量；
- 是否独立变化；
- 是否独立测试；
- 是否具有独立依赖；
- 是否存在不同故障模式。

例如：

`type_infer` 与 `actor_protocol` 虽然都属于 semantic，但它们拥有不同规则和测试边界，应拆分。

## 3.4 Explicit Control Flow

以下流程必须有清晰 orchestrator，而不是依赖隐式 callback 拼接：

- compile pipeline；
- module load pipeline；
- Actor scheduler loop；
- GC cycle；
- Native callback ingress；
- distributed message ingress；
- node shutdown。

## 3.5 Explicit Dependencies & Assembly

顶层 executable 是 composition root：

```text
ombasic CLI
   ↓
compiler driver
   ↓
compiler capabilities

ombasic run
   ↓
runtime bootstrap
   ↓
VM / actor / scheduler / transport
```

底层模块不得反向调用 CLI。

## 3.6 Necessary Complexity

第一版优先：

```text
mutex queue > lock-free queue
single-thread actor runtime > work stealing
mark-sweep > generational GC
bytecode VM > LLVM backend
manual .omi > full automatic C parser
```

只有指标证明必要时升级复杂度。

## 3.7 Local Change & Shared Invariants

必须存在单一权威定义：

| 不变量 | 权威模块 |
|---|---|
| Token kinds | syntax/token |
| Keyword spelling | syntax/keyword |
| Naming validation | syntax/name_rules |
| Type identities | semantic/type_store |
| Message serializable rules | semantic/serialization |
| Bytecode opcodes | bytecode/format |
| Runtime Value tags | runtime/value |
| Actor state transitions | runtime/actor |
| Wire protocol | runtime/distribution/protocol |
| Native ownership rules | native/interface |

不得在多个模块复制判断逻辑。

## 3.8 Composable Testability

测试分层：

```text
unit
→ capability
→ compiler pipeline
→ VM
→ actor protocol
→ integration
→ distributed
→ conformance
```

## 3.9 Bounded Context

每个能力模块应该能够通过以下最小集合被理解：

```text
README
public headers
invariants
unit tests
```

阅读 `semantic/type_infer` 不应要求理解 scheduler。

## 3.10 Layered Feedback & Executable Constraints

架构原则必须进入自动化：

- include dependency check；
- forbidden API check；
- sanitizer；
- fuzz；
- static analysis；
- compiler warnings；
- architecture tests。

---

# 4. 总体架构

```text
                       ┌─────────────────────┐
                       │       CLI           │
                       │ check/run/build/... │
                       └──────────┬──────────┘
                                  │
                       ┌──────────▼──────────┐
                       │  Compiler Driver    │
                       │ explicit pipeline   │
                       └──────────┬──────────┘
                                  │
       ┌──────────────┬───────────┼───────────┬───────────────┐
       │              │           │           │               │
       ▼              ▼           ▼           ▼               ▼
    Source          Syntax      Modules     Semantic       Bytecode
       │              │           │           │               │
       └──────────────┴───────────┴───────────┴───────────────┘
                                  │
                                  ▼
                          Bytecode Artifact
                                  │
                                  ▼
                        ┌──────────────────┐
                        │ Runtime Bootstrap│
                        └────────┬─────────┘
                                 │
          ┌──────────────┬───────┼────────┬──────────────┐
          ▼              ▼       ▼        ▼              ▼
         VM             GC     Actor   Scheduler       Native
                                         │              │
                                         ▼              ▼
                                      Network       C ABI
```

---

# 5. 代码仓库目录设计

推荐：

```text
ombasic/
├── CMakeLists.txt
├── cmake/
│   ├── Sanitizers.cmake
│   ├── Warnings.cmake
│   └── ArchitectureChecks.cmake
│
├── spec/
│   └── OMBasic-Language-Standard.md
│
├── docs/
│   ├── architecture/
│   │   ├── overview.md
│   │   ├── dependency_rules.md
│   │   ├── runtime_model.md
│   │   └── adr/
│   ├── development/
│   │   ├── c23_safety_profile.md
│   │   ├── testing_strategy.md
│   │   ├── diagnostics.md
│   │   └── fuzzing.md
│   └── protocols/
│       ├── bytecode.md
│       ├── native_interface.md
│       └── distribution.md
│
├── src/
│   ├── foundation/
│   │   ├── memory/
│   │   ├── arena/
│   │   ├── slice/
│   │   ├── vector/
│   │   ├── hash/
│   │   ├── string/
│   │   ├── utf8/
│   │   ├── ids/
│   │   └── platform/
│   │
│   ├── source/
│   │   ├── source_text/
│   │   ├── source_map/
│   │   └── file_loader/
│   │
│   ├── syntax/
│   │   ├── token/
│   │   ├── lexer/
│   │   ├── parser/
│   │   ├── ast/
│   │   └── name_rules/
│   │
│   ├── module_graph/
│   │   ├── import_path/
│   │   ├── graph/
│   │   └── interface/
│   │
│   ├── semantic/
│   │   ├── symbol/
│   │   ├── scope/
│   │   ├── type_store/
│   │   ├── type_infer/
│   │   ├── effect/
│   │   ├── actor_protocol/
│   │   ├── serializable/
│   │   └── semantic_model/
│   │
│   ├── bytecode/
│   │   ├── format/
│   │   ├── builder/
│   │   ├── verifier/
│   │   └── compiler/
│   │
│   ├── compiler/
│   │   ├── session/
│   │   ├── pipeline/
│   │   └── diagnostics/
│   │
│   ├── runtime/
│   │   ├── value/
│   │   ├── object/
│   │   ├── string/
│   │   ├── collection/
│   │   ├── heap/
│   │   ├── gc/
│   │   ├── vm/
│   │   ├── mailbox/
│   │   ├── actor/
│   │   ├── scheduler/
│   │   ├── system_actor/
│   │   ├── native/
│   │   └── distribution/
│   │
│   └── cli/
│       ├── main.c
│       ├── commands/
│       └── config/
│
├── tools/
│   ├── architecture_check/
│   ├── bind/
│   └── fuzz/
│
└── tests/
    ├── unit/
    ├── syntax/
    ├── semantic/
    ├── bytecode/
    ├── runtime/
    ├── actor/
    ├── native/
    ├── distribution/
    ├── integration/
    ├── conformance/
    ├── compile_fail/
    ├── golden/
    └── fuzz_corpus/
```

---

# 6. 模块依赖规则

建议用 CMake targets 实际约束依赖，而不是只靠约定。

基本方向：

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
bytecode
   ↓
compiler

foundation
   ↓
runtime/value
   ↓
runtime/vm
   ↓
runtime/actor
   ↓
runtime/scheduler
   ↓
runtime/distribution
```

横向允许的稳定契约：

```text
bytecode/format ← compiler emitter
bytecode/format ← runtime vm
```

不允许：

```text
syntax -> semantic
runtime -> compiler
foundation -> syntax
foundation -> runtime actor
semantic -> cli
actor -> module_graph
```

## 6.1 Public/Internal Header

每个能力模块应区分：

```text
include/ombasic/<capability>/...
src/<capability>/internal/...
```

其他能力只能 include public header。

CI 应检查跨 capability include 是否进入 `internal/`。

---

# 7. C23 工程安全基线

C23 是当前正式 C 标准，但 C23 本身不是内存安全语言。

因此 OMBasic 工程定义自己的 **Safe C23 Profile**。

## 7.1 allocator 权限

只有以下模块允许直接调用：

```c
malloc
calloc
realloc
free
aligned_alloc
```

允许位置：

```text
foundation/memory
foundation/arena
foundation/vector
runtime/heap
```

其他模块调用统一封装 API。

CI 使用文本/AST 检查阻止违规调用。

## 7.2 Checked allocation arithmetic

所有类似：

```c
count * sizeof(T)
```

的 allocation size 必须检查溢出。

优先使用 C23：

```c
#include <stdckdint.h>

size_t bytes;
if (ckd_mul(&bytes, count, sizeof(Token))) {
    return OM_ERROR_SIZE_OVERFLOW;
}
```

封装：

```c
void *om_alloc_array(size_t count, size_t item_size);
```

业务代码不得自己重复该逻辑。

## 7.3 禁止 VLA 与 alloca

禁止：

```c
int data[n];
alloca(...);
```

动态数据使用 Arena/Vector。

## 7.4 Pointer + Length

裸指针不得表示数组。

统一：

```c
typedef struct {
    Token *data;
    size_t length;
} Om_Token_Slice;
```

只读字符串：

```c
typedef struct {
    const char *data;
    size_t length;
} Om_String_View;
```

## 7.5 Handle/ID 优先

跨阶段长期引用不保存内部容器指针。

例如：

```c
typedef uint32_t Om_Symbol_Id;
typedef uint32_t Om_Type_Id;
typedef uint32_t Om_Source_Id;
typedef uint32_t Om_Node_Id;
```

这样 vector 扩容不会破坏引用。

## 7.6 Pointer arithmetic

任意 pointer arithmetic 仅允许存在于：

- slice；
- vector；
- arena；
- bytecode decoder；
- platform/ffi boundary。

其他模块应通过 index/iterator API。

## 7.7 C string 边界

编译器内部字符串默认不依赖 NUL termination。

只有：

- OS API；
- libc 边界；
- Native FFI；

允许临时构造 C string。

## 7.8 禁止危险 API

禁止：

```text
gets
strcpy
strcat
sprintf
scanf("%s") without limit
```

优先长度明确 API。

## 7.9 `void *` 限制

`void *` 只能用于：

- allocator；
- generic container internals；
- OS API；
- Native ABI。

领域 API 不得依赖无类型 `void *`。

## 7.10 初始化规则

所有 struct 创建必须通过：

```text
zero initialization
或
显式 constructor/init function
```

不允许部分未初始化对象进入 public API。

## 7.11 Destroy/Reset 规则

资源类型必须明确：

```text
init
reset
free/destroy
```

谁创建谁不一定销毁，但 ownership 必须在 public header 文档里明确。

## 7.12 Assertions

`assert()` 只能验证：

> 编译器内部不变量。

不得用于处理用户输入错误。

错误 `.bas` 文件必须返回 diagnostic。

---

# 8. 内存区域设计

## 8.1 Compiler Session Arena

生命周期：一次 `ombasic` 编译 session。

用于：

- interned strings；
- target configuration；
- module identity metadata；
- stable compiler tables。

## 8.2 Module Arena

生命周期：一个 Module 的编译过程。

用于：

- tokens；
- AST；
- semantic side tables；
- diagnostics payload。

当 Module 编译结束且缓存/bytecode 已生成后，可整体释放。

## 8.3 Scratch Arena

生命周期：单个 pass。

用于：

- temporary constraint lists；
- DFS stacks；
- sort buffers；
- diagnostic formatting scratch。

pass 结束 reset。

## 8.4 Runtime Actor Heap

每 Actor 一个长期 heap。

只由当前执行该 Actor 的 worker 访问。

## 8.5 Message Buffer

Mailbox 不直接保存来自 sender heap 的 pointer。

建议第一版统一：

```text
Message value
  ↓ encode/copy
Message Buffer
  ↓ enqueue
Receiver
  ↓ decode
Receiver Actor Heap
```

这样本地和远程 Message 使用相同隔离原则。

---

# 9. Foundation 模块

第一阶段必须先完成 foundation，并保持极小。

## 9.1 memory

API 示例：

```c
void *om_alloc(size_t size);
void *om_alloc_array(size_t count, size_t item_size);
void *om_realloc_array(void *ptr, size_t count, size_t item_size);
void om_free(void *ptr);
```

## 9.2 arena

需求：

- segmented arena；
- alignment-safe；
- reset；
- destroy；
- checked size arithmetic；
- debug poison option。

## 9.3 vector

要求：

- length/capacity；
- checked growth；
- no silent integer wrap；
- reserve/push/clear；
- no public raw realloc。

## 9.4 hash table

优先简单 open addressing。

第一版重点正确性，不做过度 SIMD 优化。

## 9.5 UTF-8

提供：

- validate；
- next codepoint；
- byte offset；
- line/column mapping。

标识符当前仅 ASCII，但 String/Comment 是 UTF-8。

## 9.6 IDs

所有 ID 使用明确 typedef，避免混用：

```c
typedef struct { uint32_t value; } Om_Source_Id;
typedef struct { uint32_t value; } Om_Symbol_Id;
typedef struct { uint32_t value; } Om_Type_Id;
```

若编译器支持良好，可以使用 struct wrapper，而不是裸 typedef uint32_t，以防误传。

---

# 10. Source 与 Source Map

## 10.1 Source_Text

保存：

```text
Source_Id
canonical module path
immutable bytes
byte length
line offset table
content hash
```

## 10.2 Span

所有 token / AST node / diagnostic 都使用：

```c
typedef struct {
    Om_Source_Id source;
    uint32_t start;
    uint32_t end;
} Om_Span;
```

offset 使用 byte offset。

行列号只在 diagnostic rendering 时计算，避免每个 AST node 保存 line/column。

## 10.3 Source size limit

必须设置合理单文件最大大小，防止恶意输入导致整数/内存问题。

限制应可配置，但编译器必须在超限时给出明确诊断。

---

# 11. Module Loader

Module loader 是高风险边界，应独立于 parser。

## 11.1 输入

```text
source root
entry .bas path
```

## 11.2 路径验证

严格按标准验证：

- `.bas`；
- `/` separator in source；
- no `.` segment；
- no `..`；
- no empty segment；
- lower_snake_case file/dir；
- relative to source root；
- exact casing。

## 11.3 Filesystem escape

实现必须防止：

```text
symlink escaping source root
```

推荐策略之一：

1. canonicalize root；
2. resolve imported file；
3. 验证 resolved path 仍在 root；
4. 可提供严格模式完全禁止 source tree symlink。

## 11.4 Import graph

建立 DAG：

```text
Module_Id
path
imports[]
state = Unseen / Loading / Loaded
```

DFS 遇到 Loading 即循环。

诊断输出完整链：

```text
a.bas -> b.bas -> c.bas -> a.bas
```

## 11.5 编译顺序

拓扑排序：

```text
imports first
then importer
```

为后续 incremental compilation 奠定基础。

---

# 12. Lexer

## 12.1 实现策略

手写 lexer。

原因：

- grammar 小；
- token 语义稳定；
- 错误信息更可控；
- 更容易 fuzz；
- 不引入生成器依赖。

## 12.2 Token

至少包含：

```text
kind
span
payload/index
```

不要把完整字符串复制进每个 Token。

## 12.3 Keyword

关键字表单一权威。

严格大小写：

```text
IF legal
If illegal identifier/name form
if illegal identifier/name form
```

诊断可以提示：

```text
Did you mean IF?
```

但编译器不得自动改写。

## 12.4 Comment

实现：

- `'` line comment；
- `'''` documentation line when first non-whitespace；
- 不支持 block comment。

## 12.5 String

需要：

- UTF-8 validation；
- escape rules；
- interpolation lexer state。

字符串插值应单独测试嵌套和错误恢复。

## 12.6 Fuzz targets

至少：

```text
fuzz_lexer_bytes
fuzz_string_literal
fuzz_comment_boundary
```

---

# 13. Parser

## 13.1 策略

- statement/declaration：recursive descent；
- expression：Pratt parser。

不使用通用 parser generator 作为 0.1 核心依赖。

## 13.2 Parser Context

包含：

```text
token slice
current index
module arena
diagnostic sink
```

## 13.3 AST

AST 必须只表示语法事实，不承载类型系统业务状态。

推荐：

```text
Ast_Node_Id
Ast_Node
Span
Kind
children IDs
```

## 13.4 错误恢复

不能首错即停止。

同步点：

```text
newline
END IF
END FUNCTION
END ACTION
END ACTOR
END RECEIVE
END FOR
END WHILE
下一顶层 declaration
```

但恢复逻辑不得产生无限循环。

## 13.5 parser error invariant

每次 parse failure 必须：

```text
消费至少一个 token
或
到达 EOF
```

防止 malformed input 死循环。

## 13.6 Golden AST Tests

输入：

```text
.bas
```

输出规范化 AST tree，用 golden file 比较。

---

# 14. Naming Rules

独立模块 `syntax/name_rules`。

必须验证：

## ordinary

```text
lower_snake_case
```

用于：

- variable；
- parameter；
- FUNCTION；
- ACTION；
- file/dir base names。

## entity

```text
Pascal_Snake_Case
```

用于：

- RECORD；
- MESSAGE；
- ACTOR。

## 禁止

- leading `_`；
- trailing `_`；
- `__`；
- same-scope case-only collision；
- magic names。

这些规则只实现一次，parser、semantic、formatter 复用。

---

# 15. Symbol 与 Scope

## 15.1 Symbol_Id

所有符号使用稳定 Symbol_Id。

Symbol 保存：

```text
name_id
kind
span
scope_id
visibility
module_id
```

## 15.2 Scope

明确区分：

- module scope；
- function/action scope；
- actor scope；
- receive scope；
- block scope。

## 15.3 Resolution

名称解析阶段不得执行类型推导。

先确定“这个名字指向谁”，再确定类型。

## 15.4 Import qualification

```basic
IMPORT "storage/database.bas"
connection = database.open(...)
```

`database` 解析为 Module_Id/Module_Symbol。

0.1 不支持 alias。

---

# 16. Type System 与 Type Store

## 16.1 Type representation

Type Store 统一 intern Type：

```text
Int
Float
Bool
String
Null
List<T>
Map<K,V>
Record<Id>
Message<Id>
ActorRef<Protocol>
Function<Args,Return>
Result<T,E>
TypeVar
```

每个类型返回 `Type_Id`。

## 16.2 不把 C pointer 当 Type_Id

避免类型节点 vector 扩容后悬空。

## 16.3 Unification

实现：

```text
find
bind type var
occurs check
unify structural types
unify nominal entity types
```

第一版不需要高阶 kind system。

## 16.4 泛型

最小 HM 风格：

```basic
FUNCTION identity(x)
    RETURN x
END FUNCTION
```

推导：

```text
T -> T
```

在 module public interface 中保存 type scheme。

## 16.5 Nominal types

必须 nominal：

- RECORD；
- MESSAGE；
- ACTOR protocol。

特别是两个相同字段的 Message 不兼容。

## 16.6 Numeric rules

0.1 必须提前冻结：

- integer default width；
- float default width；
- `Int + Float` 是否非法或明确转换；
- numeric literal range diagnostics。

如果语言标准未完全规定，应在实现前新增 ADR/规范修订，不得在 compiler 中偷偷决定。

---

# 17. Semantic Model

不要直接污染 AST。

建立 side tables：

```text
Node_Id -> Type_Id
Node_Id -> Symbol_Id
Node_Id -> Effect_Info
Node_Id -> Constant_Value(optional)
Actor_Id -> Protocol
Message_Id -> Schema
```

优点：

- AST 保持 immutable；
- semantic pass 独立；
- LSP 可复用；
- incremental compilation 更容易。

---

# 18. Effect / Purity Analysis

OMBasic 的 `FUNCTION` 与 `ACTION` 是核心可理解性机制。

## 18.1 Effect categories

内部至少记录：

```text
Pure
Mutates_Local
Mutates_Actor_State
Send_Message
Spawn_Actor
Native
File_IO
Network_IO
Clock
```

源码不需要看到这些完整类型，但编译器/LSP 应知道。

## 18.2 Function rule

FUNCTION body 不得：

- SEND；
- SPAWN；
- actor state access；
- ACTION call；
- effectful Native；
- external I/O。

## 18.3 Action rule

ACTION 可以有显式 effect，但不能隐式挂起。

## 18.4 Call graph

建立 static call graph。

effect 沿调用边传播：

```text
FUNCTION A -> FUNCTION B -> Native effect
```

则 A 非法。

## 18.5 Native default

建议安全默认：

> 未显式证明为 pure 的 Native call 一律视为 effectful。

---

# 19. Actor Protocol Analysis

## 19.1 Protocol

每个 Actor 编译出：

```text
accepted message types
handler mapping
actor state types
```

## 19.2 ActorRef type

```text
ActorRef<Payment_Service>
```

## 19.3 SEND checking

```basic
SEND payment_service, Payment_Accepted(...)
```

编译器检查：

1. receiver 是 ActorRef；
2. message 是 MESSAGE；
3. receiver protocol 接受该 Message；
4. payload types 成立；
5. message 可序列化。

## 19.4 SELF

`SELF` 是关键字，不是魔法变量名。

只能在 Actor/Receive 合法上下文出现。

---

# 20. Serializable Analysis

单独 capability。

可序列化：

- primitives；
- String；
- immutable List/Map of serializable；
- Record of serializable；
- Message of serializable；
- ActorRef。

不可序列化：

- CPointer；
- NativeHandle；
- callback token；
- file descriptor；
- raw runtime object pointer。

结果记录到 semantic model，供 SEND 与 distribution compiler 使用。

---

# 21. Lowering 策略

第一版避免过早引入大型优化 IR。

推荐：

```text
AST
+
Semantic Model
↓
Bytecode Builder
```

只实现一个轻量 lowering helper 层，将：

- `FOR`；
- `TRY`；
- `SELECT`；
- short-circuit `AND/OR`；

转为明确 control-flow blocks。

当优化需求出现时再引入 HIR/SSA，并通过 ADR 说明收益。

---

# 22. Bytecode 设计

## 22.1 目标

- 容易解释；
- 容易验证；
- source mapping 明确；
- 可序列化；
- 跨平台 representation 稳定；
- 不直接暴露 C struct memory layout。

## 22.2 Opcode 初始分类

```text
Constants
LOAD_CONST
LOAD_LOCAL
STORE_LOCAL

Arithmetic
ADD_INT
SUB_INT
MUL_INT
DIV_INT
ADD_FLOAT
...

Compare
EQ
LT
LE
...

Control
JUMP
JUMP_IF_FALSE
CALL
RETURN

Data
MAKE_RECORD
GET_FIELD
MAKE_LIST
MAKE_MAP

Error
CHECK_RESULT
UNWRAP_OK
RETURN_ERROR

Actor
SPAWN
SEND
WATCH
SELF
```

`RECEIVE` 不需要普通 opcode；Actor definition 可编译成 Message_Id → handler entry table。

## 22.3 Bytecode verification

VM 执行前必须验证：

- opcode legal；
- operands in range；
- constant index legal；
- jump target valid；
- function entry valid；
- stack discipline consistent。

即使 bytecode 来自本编译器，也不能假定永远正确。

## 22.4 Source Map

Bytecode instruction range → Source Span。

用于：

- runtime error；
- stack trace；
- debugger；
- profiling。

---

# 23. VM Value Model

第一版不用 NaN boxing。

推荐显式 tagged union：

```c
typedef enum {
    OM_VALUE_NULL,
    OM_VALUE_BOOL,
    OM_VALUE_INT,
    OM_VALUE_FLOAT,
    OM_VALUE_OBJECT,
    OM_VALUE_ACTOR_REF
} Om_Value_Tag;

typedef struct {
    Om_Value_Tag tag;
    union {
        bool boolean;
        int64_t integer;
        double floating;
        Om_Object *object;
        Om_Actor_Ref actor_ref;
    } as;
} Om_Value;
```

优点：

- 易调试；
- 易 Sanitizer；
- 语义显式；
- 以后可优化 representation，不影响语言规范。

---

# 24. Runtime Object Model

对象类型：

```text
String
List
Map
Record
Message
Function
Closure(if supported)
Result payload
```

基础 header：

```c
typedef struct Om_Object {
    Om_Object_Kind kind;
    bool marked;
    struct Om_Object *next;
} Om_Object;
```

第一版优先简单。

---

# 25. String Runtime

OMBasic String：

- UTF-8；
- immutable；
- length-known；
- 不依赖 NUL。

Runtime object：

```text
byte_length
hash cache(optional)
bytes[]
```

String interning：

- compiler identifier 推荐 intern；
- runtime user String 不强制全部 intern。

---

# 26. List / Map Runtime

第一版语义 immutable。

实现可以采用：

- copy-on-update；
- internal builder for literals；

不立即实现 persistent HAMT/vector trie。

若 profiling 证明复制成本过高，再通过 ADR 引入 persistent structure。

---

# 27. Actor-local GC

## 27.1 第一阶段

在 VM/Actor 语义尚未稳定前，允许：

```text
Actor-lifetime allocation
```

即 Actor 结束统一释放。

只用于早期开发，不作为正式长期 Runtime。

## 27.2 正式 0.1 GC

实现 non-generational mark-and-sweep。

每 Actor：

```text
objects list
bytes allocated
gc threshold
mark stack/gray stack
```

## 27.3 Roots

至少：

- actor state；
- current VM stack；
- call frames；
- current decoded message；
- open runtime references；
- local globals if language allows immutable constants。

## 27.4 Mailbox

最好存 serialized/copy buffer，而不是 receiver heap object pointer。

这样 mailbox 不需要被 Actor GC 追踪。

## 27.5 GC concurrency

Actor heap 只有当前 actor worker 访问。

因此 GC(A) 时：

```text
Actor B/C/D 可继续运行
```

不做全 Node STW。

## 27.6 GC Tests

必须包含：

- deep graph；
- cycle；
- dead cycle；
- nested records；
- list/map graph；
- repeated GC；
- actor stop cleanup。

---

# 28. VM 执行器

## 28.1 Frame

Frame 保存：

```text
function id
instruction pointer
base stack index
```

## 28.2 Runtime errors

用户错误/运行错误返回结构化 runtime error，不使用 C `longjmp` 作为正常控制流。

## 28.3 TRY

TRY 应在 bytecode lowering 中显式生成 Result 检查与 return path，不通过 hidden exception unwinding。

---

# 29. Actor Runtime

## 29.1 Actor structure

概念结构：

```c
typedef struct Om_Actor {
    Om_Actor_Id id;
    Om_Mailbox mailbox;
    Om_VM_State vm;
    Om_Actor_Heap heap;
    Om_Actor_Status status;
    ...
} Om_Actor;
```

## 29.2 Actor state machine

明确状态：

```text
Created
Idle
Queued
Running
Stopping
Stopped
```

状态转换集中在 actor/scheduler 模块。

## 29.3 Receive run-to-completion

语义要求：

```text
一条 message handler
开始
↓
执行
↓
结束
↓
下一 message
```

允许 scheduler 在 bytecode instruction quantum 后暂停当前 actor 并换 worker，但：

> 在当前 handler 完成前，不得为同一 Actor 开始第二条 Message。

这不属于 `AWAIT`，也不产生 reentrancy。

---

# 30. Mailbox

第一版使用正确性优先结构：

```text
mutex + FIFO queue
```

不要先做 lock-free MPSC。

接口：

```text
enqueue(message_buffer)
dequeue()
close()
```

要求：

- multi-producer；
- single logical consumer；
- bounded message size；
- OOM policy 明确。

后续如果 contention 数据证明需要，再升级。

---

# 31. Scheduler

## 31.1 Stage A：单线程 deterministic scheduler

先实现：

```text
ready queue
run actor
process message
repeat
```

好处：

- actor semantics 可验证；
- deterministic tests；
- 没有并发噪音。

## 31.2 Stage B：M:N worker scheduler

```text
N Actors
   ↓
Scheduler
   ↓
M OS Threads
```

初始实现：

```text
global ready queue + mutex + condition variable
```

## 31.3 Actor exclusivity

必须用原子/锁状态保证：

> 一个 Actor 不会同时进入两个 worker。

## 31.4 Fairness

为防止：

```basic
WHILE TRUE
END WHILE
```

独占 worker，VM 可设置 instruction budget。

budget 耗尽：

1. 保存当前 handler VM 状态；
2. Actor 重新入 ready queue；
3. 不 dequeue 下一 Message；
4. 下次继续同一 handler。

---

# 32. System Actors

I/O 全部优先通过 Actor 暴露：

- File_System；
- Network；
- Timer；
- Process；
- DNS；
- Console。

例如：

```text
User Actor
  ↓ SEND
File_System Actor
  ↓ native/platform operation
  ↓ completion
Message back
```

这样 OMBasic 用户不需要 `AWAIT`。

---

# 33. Blocking Worker Pool

阻塞系统调用不得占用普通 actor worker。

结构：

```text
Actor Scheduler Threads
         │
         │ request
         ▼
Blocking Pool
         │
         │ completion message
         ▼
Mailbox
```

Blocking task 不可直接持有普通 Actor heap pointer。

输入必须复制或转为稳定 native buffer/handle。

---

# 34. Native C FFI

## 34.1 `.omi`

Native grammar 单独 parser。

不得复用普通 `.bas` parser 后用大量 special case。

## 34.2 ABI descriptors

内部：

```text
Native_Function
calling convention
argument ABI types
return ABI type
ownership
blocking
callback policy
effect
```

## 34.3 Dynamic loading

平台层：

```text
POSIX: dlopen/dlsym
Windows: LoadLibrary/GetProcAddress
```

上层只看到统一 Native_Library API。

## 34.4 Ownership

明确：

```text
Borrowed
Owned
Static
```

Owned 必须绑定 destructor/free symbol。

## 34.5 C++

只支持：

```text
extern "C" wrapper
```

不得直接解析/调用 STL/class ABI。

## 34.6 Bind generator

阶段 1：手写 `.omi`。

阶段 2：`ombasic bind` 使用 libclang 或 Clang AST 自动生成基础签名。

但 ownership/blocking/callback threading 仍要求人工确认。

---

# 35. Native Callback

外部线程进入 Runtime：

```text
C callback thread
  ↓
trampoline
  ↓
copy callback payload
  ↓
thread-safe runtime ingress queue
  ↓
Actor mailbox
```

禁止：

```text
callback thread -> execute OMBasic Actor handler directly
```

Runtime ingress 是明确的安全边界。

---

# 36. 分布式 Runtime

分布式放在本地 Actor Runtime 稳定之后。

## 36.1 Node identity

建议：

```text
Node_Id: 128-bit random identity per runtime epoch
Actor_Id: 64-bit monotonic
```

ActorRef：

```text
Node_Id
Actor_Id
Epoch(optional explicit field)
```

## 36.2 Transport

第一版使用 ordered stream transport。

建议 TCP。

内部 Transport API：

```text
connect
send_frame
receive_frame
close
```

不要让 Actor 模块直接操作 socket。

## 36.3 Framing

定义稳定 wire frame：

```text
magic
protocol version
frame type
payload length
sender ActorRef
receiver ActorRef
message type fingerprint
payload
```

所有 length 先验证再分配。

设置：

```text
max frame size
max message nesting
max collection length
```

防止 memory DoS。

## 36.4 Message identity

由于 OMBasic 源码只有 Module，不存在 Package namespace，第一版建议使用：

```text
canonical module path
+
Message entity name
+
schema fingerprint
```

生成 stable message fingerprint。

## 36.5 Build compatibility

0.1 分布式阶段建议采用严格兼容策略：

> 节点握手必须确认 protocol version 与 program/interface fingerprint 兼容。

先不做复杂 schema evolution。

## 36.6 Delivery

保持标准：

```text
at-most-once
```

Runtime 不自动 retry 用户 Message。

## 36.7 Ordering

同一 sender 到同一 receiver 使用同一有序 node channel，保证发送顺序。

不同 sender 不保证全局排序。

## 36.8 Watch

本地：watch table。

远程：

```text
WATCH request
Actor down notification
Node disconnected notification
```

Node down 时，向 watcher 产生 `Actor_Down` 或等价系统 Message。

## 36.9 Distributed GC

明确不实现。

ActorRef 是 value，远端 Actor 生命周期不由本地 GC 决定。

---

# 37. Compiler Driver

Compiler Driver 是 Explicit Control Flow 的核心。

伪流程：

```text
create_session
load_entry_module
build_module_graph
for module in topo_order:
    lex
    parse
    resolve_names
    infer_types
    check_effects
    check_actor_protocols
    check_serializable
    emit_module_interface
    emit_bytecode
link/load bytecode image
```

每一步：

```text
input
output
errors
```

必须明确。

禁止通过 global callback registry 隐式触发后续 pass。

---

# 38. Diagnostics

OMBasic 的核心目标之一是人类可理解，因此 diagnostic 属于产品核心，而不是附属功能。

## 38.1 Diagnostic structure

```text
code
severity
primary span
message
secondary spans
notes
suggestions
```

## 38.2 Stable codes

例如：

```text
E0101 invalid keyword casing
E0201 invalid import path
E0202 unexpected or missing syntax token
E0203 illegal module-level construct or import ordering
E0204 missing expression
E0205 invalid direct-negative INT64_MIN literal
E0206 chained comparison
E0207 expression statement is not a call
E0208 parser recovery or nesting safety limit
E0301 undefined name
E0302 private imported module member
E0303 duplicate declaration
E0304 case-only name collision
E0305 shadowing a visible or prelude name
E0306 invalid or conflicting module alias
E0307 assignment to immutable binding
E0401 type mismatch
E0402 empty collection type cannot be inferred
E0501 function has side effect
E0601 actor does not accept message
E0701 message not serializable
```

具体编号在 ADR 中冻结。

## 38.3 Diagnostics should explain why

不要只输出：

```text
type mismatch
```

应该：

```text
E0401: cannot assign String to count
count was inferred as Int here: ...
```

## 38.4 Golden diagnostics

compile-fail tests 保存 expected stderr/structured JSON。

---

# 39. CLI 设计

最小命令：

```text
ombasic check
ombasic run
ombasic build
ombasic fmt
ombasic bind
ombasic version
```

建议显式 source root：

```text
ombasic check --root src src/main.bas
ombasic run   --root src src/main.bas
```

以后可允许配置文件记录 root，但不能改变语言层 Module identity 规则。

## 39.1 `check`

只运行到 semantic validation。

## 39.2 `run`

compile → bytecode → runtime。

## 39.3 `build`

第一版可输出：

- bytecode image；
- 或 bytecode + runtime launcher bundle。

不要承诺 native machine code。

## 39.4 `fmt`

formatter 很早实现，因为 OMBasic 有严格风格目标。

---

# 40. Formatter

Formatter 必须是 deterministic。

输入同一 AST：

```text
format(format(source)) == format(source)
```

强制：

- keyword uppercase；
- indentation；
- spacing；
- blank line conventions；
- comment placement。

但 formatter 不应“修复语义错误”。

错误关键字 casing 应由 compiler diagnostic 处理，而不是静默接受后再改。

---

# 41. LSP 规划

LSP 不放在第一编译器里实现，但从一开始让 semantic API 可复用。

未来需要：

- diagnostics；
- hover inferred types；
- inlay hints；
- go to definition；
- find references；
- completion；
- Actor protocol hints；
- effect hints；
- Module path completion。

Compiler semantic model 不应依赖 CLI，因此 LSP 可以直接嵌入。

---

# 42. Module Interface Metadata

编译每个 Module 生成内部 interface：

```text
public symbols
inferred type schemes
record schemas
message schemas
actor protocols
effects
serialization flags
interface hash
```

第一版可以只存在内存。

增量编译阶段再序列化 cache。

---

# 43. Incremental Compilation

稳定前不要过早实现。

后续 key：

```text
compiler version
language standard version
target
source content hash
imported interface hashes
```

若 implementation body 变化但 public interface hash 不变，下游 Module 可避免 semantic rebuild。

这正好利用 Import DAG。

---

# 44. Determinism

编译器应尽可能 deterministic。

相同：

```text
source
compiler version
target
flags
```

应生成相同 bytecode/interface metadata。

避免：

- hash iteration order 决定输出；
- pointer address 进入 artifact；
- current time 进入 artifact；
- nondeterministic symbol ordering。

---

# 45. 测试架构

## 45.1 Unit

测试 Foundation 与单个 algorithm。

## 45.2 Syntax

- lexer token golden；
- parser AST golden；
- invalid syntax recovery。

## 45.3 Semantic

- symbol；
- type inference；
- purity；
- Actor protocol；
- serializable。

## 45.4 Compile-fail

每条语言限制至少一个失败用例。

## 45.5 Bytecode

- emitter golden；
- verifier invalid bytecode tests；
- VM instruction tests。

## 45.6 Runtime

- value/object；
- GC；
- VM；
- actor；
- mailbox；
- scheduler。

## 45.7 Integration

`.bas` → expected output。

## 45.8 Distribution

- two node localhost；
- ordering；
- disconnect；
- Actor_Down；
- malformed frame；
- oversized message。

## 45.9 Conformance

按语言标准章节编号组织：

```text
tests/conformance/05_identifiers
tests/conformance/07_binding
tests/conformance/16_actor
...
```

这样规范修改可直接找到测试影响面。

---

# 46. Sanitizer 策略

Clang 官方 AddressSanitizer 可检测 heap/stack/global 越界、use-after-free、double-free 等；MemorySanitizer 用于未初始化内存；UndefinedBehaviorSanitizer 检测多类 UB；ThreadSanitizer 用于并发 race 检测。

建议 CI profile：

## ASan + UBSan

```text
-O1
-g
-fsanitize=address,undefined
-fno-omit-frame-pointer
```

运行：

- all unit；
- compiler integration；
- VM；
- GC；
- Actor single-thread。

## TSan

```text
-fsanitize=thread
```

运行：

- mailbox；
- scheduler；
- actor multi-thread；
- runtime ingress；
- network concurrency。

## MSan

独立 Linux job。

主要覆盖 foundation/compiler core。

由于 MSan 对外部未 instrumented library 有限制，不强求所有 Native integration 都在 MSan 下通过。

---

# 47. Fuzzing

## 47.1 Lexer

输入 arbitrary bytes。

## 47.2 Parser

输入 arbitrary bytes。

## 47.3 Module import path

输入 random path strings。

## 47.4 Bytecode verifier

输入 random bytecode bytes。

## 47.5 Message decoder

输入 random network payload。

## 47.6 Native metadata parser

输入 random `.omi`。

## 47.7 Fuzz invariant

永远：

```text
accept
or
reject with diagnostic
```

不得 crash/hang/UB。

---

# 48. Static Analysis

建议：

- Clang warnings；
- clang-tidy selected checks；
- clang static analyzer；
- GCC `-fanalyzer` 作为第二视角；
- include-what-you-use 可选。

不追求“所有规则全部打开”，避免 false-positive 驱动的无意义代码。

规则选择应写入 `docs/development/c23_safety_profile.md`。

---

# 49. Compiler Warning Policy

开发构建建议高警告等级：

```text
-Wall
-Wextra
-Wpedantic
-Wconversion
-Wshadow
-Wstrict-prototypes
```

项目自身代码 warnings-as-errors。

第三方库不继承 `-Werror`。

---

# 50. Architecture Checks

CI 自动验证：

## 50.1 Forbidden includes

例如：

```text
foundation cannot include semantic
syntax cannot include runtime
runtime cannot include compiler
```

## 50.2 Forbidden APIs

core 之外禁止：

```text
malloc
free
realloc
strcpy
sprintf
alloca
```

## 50.3 Dependency cycles

CMake target graph 不允许 cycle。

## 50.4 Public/private boundary

禁止 include `internal/` 跨 capability。

## 50.5 Runtime layering

network 不得修改 Actor internals，只通过 actor public API enqueue/system events。

---

# 51. CI Matrix

至少：

```text
Linux Clang Debug
Linux GCC Debug
Linux Clang Release
Linux ASan+UBSan
Linux TSan
Linux MSan
macOS Clang
Windows clang-cl
Fuzz smoke
Architecture checks
Conformance tests
```

若 C23 feature 在某平台缺失，必须通过明确 compatibility shim 或缩减平台声明解决，不能偷偷降级语义。

---

# 52. Performance Strategy

正确性稳定之前不做微优化。

先建立 benchmark：

## Compiler

- lex MB/s；
- parse MB/s；
- type inference large module；
- cold build；
- incremental build。

## Runtime

- function call；
- actor spawn；
- message throughput；
- mailbox contention；
- GC pause per actor；
- local serialization；
- remote message throughput。

优化必须绑定 benchmark/ADR。

---

# 53. Observability

Runtime 内部需要结构化可观测性，但默认不污染用户程序。

Debug runtime counters：

```text
actors alive
ready queue depth
messages sent
messages received
bytes serialized
GC count
GC reclaimed bytes
native jobs queued
network frames
```

Tracing 需要显式开启。

不要通过 hidden logging 修改 Actor 语义。

---

# 54. Security 边界

## Compiler

把源代码视为不可信输入。

限制：

- source size；
- nesting depth；
- token count；
- collection literal size；
- type inference recursion/constraint budget；
- import graph nodes。

## Runtime Network

把网络 payload 视为不可信：

- frame length check；
- schema check；
- message depth check；
- malformed UTF-8 reject；
- resource limits。

## Native

Native 明确是 trust boundary。

OMBasic 的语言安全不能保证恶意 C library。

---

# 55. 资源限制

需要统一 Runtime Limits：

```text
max source bytes
max modules
max parse nesting
max call depth
max actor count
max mailbox bytes
max message size
max network frame
max collection decode length
max native callback queue
```

默认值写入 runtime config，不散落 hardcode。

---

# 56. ADR 清单

开发开始前至少创建：

## ADR-0001 C23 作为实现标准

说明支持编译器与平台。

## ADR-0002 Explicit Architecture 适配

说明 capability/kernel 边界。

## ADR-0003 Bytecode VM 而非 LLVM First

记录收益、代价、未来迁移。

## ADR-0004 Arena + ID Compiler Memory Model

## ADR-0005 AST immutable + Semantic Side Tables

## ADR-0006 Stable Diagnostic Codes and Source Spans

## ADR-0007 Strict Source-Tree Path Resolution

## ADR-0008 Type Inference Algorithm

## ADR-0009 Semantic Effects and Messages

## ADR-0010 Bytecode Format

## ADR-0011 Single-thread VM

## ADR-0012 Single-thread Actor Scheduler

## ADR-0013 Actor-local Garbage Collection

## ADR-0014 Multi-thread Actor Scheduler

## ADR-0015 System Actor services

## ADR-0016 C ABI Native Boundary

## ADR-0017 Distribution Wire Protocol

## ADR-0018 Module Build Fingerprint

---

# 57. 规范冻结前检查

在写 parser 大量代码之前，先对现有语言标准做一次 implementation audit。

至少确认：

1. 所有保留关键字都在 grammar 或明确 future-reserved 列表中有解释。
2. `EXPORT` 的最终命名是否冻结。
3. Integer/Float 默认物理宽度。
4. numeric literal overflow 语义。
5. String escape 完整规则。
6. String interpolation grammar。
7. `NULL` 与具体类型的兼容规则。
8. shadowing 是否允许及作用域规则。
9. Result 的构造名称与类型形式。
10. `TRY` 在 `RECEIVE` 中 error 返回给谁/以何种值结束 handler。
11. `SELECT` 完整语法。
12. `WITH` Record update 完整语法。
13. `AT` Spawn remote Actor 语法。
14. `WATCH` 的系统 Message schema。
15. actor constructor/state initialization 规则。
16. Native `.omi` grammar。
17. Native pure/effect/blocking annotation grammar。
18. entry `ACTION main()` 的精确定义。

原则：

> 发现规范空洞时修改规范或写 ADR，不在 compiler 中形成“只有实现者知道”的语义。

---

# 58. 开发阶段与 Gate

下面阶段按依赖顺序推进；每一阶段必须通过 Gate 才进入下一阶段。

---

## Phase 0 — Specification Audit

### 交付

- grammar audit；
- keyword table；
- naming table；
- semantic gaps list；
- ADR-0001 ~ ADR-0005；
- conformance test skeleton。

### Gate

- lexer 所需 lexical grammar 无歧义；
- parser 所需 syntax grammar 无未定义核心结构；
- implementation-specific choice 已记录。

---

## Phase 1 — Foundation

### 实现

- memory；
- checked allocation；
- arena；
- vector；
- slice；
- hash；
- string view；
- UTF-8 validation；
- typed IDs；
- test harness。

### Gate

- ASan/UBSan clean；
- allocation overflow tests；
- arena alignment tests；
- vector fuzz/sizing tests。

---

## Phase 2 — Source / Module Loading

### 实现

- Source_Text；
- Span；
- line map；
- source root；
- path validation；
- Import extraction support；
- DAG；
- cycle diagnostics。

### Gate

- path fuzz clean；
- symlink escape policy tested；
- cyclic graph golden diagnostics。

---

## Phase 3 — Lexer

### 实现

- keyword；
- identifiers；
- number literals；
- strings；
- comments/doc comments；
- operators；
- newline semantics。

### Gate

- all lexical conformance tests；
- lexer fuzz 0 crash；
- keyword casing diagnostics。

---

## Phase 4 — Parser / AST

### 实现

- declarations；
- expressions Pratt；
- blocks；
- control flow；
- Actor syntax；
- TRY/SEND/SPAWN/WATCH；
- error recovery。

### Gate

- AST golden tests；
- malformed source fuzz；
- no infinite recovery loop。

---

## Phase 5 — Name Resolution

### 实现

- scopes；
- Symbol_Id；
- module qualification；
- EXPORT visibility；
- duplicate detection；
- case-only collision。

### Gate

- import visibility conformance；
- duplicate/collision diagnostics。

---

## Phase 6 — Type Inference

### 实现

- Type Store；
- TypeVar；
- unification；
- primitive ops；
- Record；
- List/Map；
- Function/Action signatures；
- generic schemes；
- Result。

### Gate

- positive inference tests；
- compile-fail mismatch tests；
- no Any fallback；
- public interface type stable。

---

## Phase 7 — Semantic Constraints

### 实现

- purity/effect；
- Actor protocol；
- serializable；
- Function/Action call restrictions；
- Actor state rules。

### Gate

- Function hidden effect impossible；
- wrong Message compile-fail；
- Native pointer message compile-fail。

---

## Phase 8 — Bytecode

### 实现

- format；
- builder；
- verifier；
- emitter；
- source map。

### Gate

- bytecode golden；
- verifier fuzz；
- invalid bytecode never crashes VM loader。

---

## Phase 9 — Single-thread VM

### 实现

- Value；
- Object；
- stack；
- call frame；
- arithmetic；
- records；
- list/map；
- Result/TRY。

### Gate

- normal non-Actor programs run；
- integration output tests；
- ASan clean。

---

## Phase 10 — Single-thread Actor Runtime

### 实现

- ActorRef；
- actor registry；
- mailbox；
- SPAWN；
- SEND；
- handler dispatch；
- WATCH local；
- deterministic scheduler。

### Gate

- run-to-completion guaranteed；
- Actor isolation tests；
- deterministic actor integration suite。

---

## Phase 11 — Actor-local GC

### 实现

- object list；
- roots；
- mark；
- sweep；
- thresholds；
- actor stop cleanup。

### Gate

- cycles reclaimed；
- repeated stress；
- ASan clean；
- GC object graph fuzz/property tests。

---

## Phase 12 — Multi-thread Scheduler

### 实现

- worker pool；
- ready queue；
- actor scheduling state；
- instruction quantum；
- shutdown coordination。

### Gate

- TSan clean；
- no actor simultaneous execution；
- stress millions of message operations；
- fairness regression tests。

### 状态：完成

- Windows GCC/Ninja 与 clang-cl/MSVC 的 CTest 全套均通过（10/10）；Runtime
  测试包含 23 项并连续运行 5 次通过。
- Linux GCC TSan Runtime 测试通过，包含 2,000,000 条消息的并发压力测试；
  容器需使用 non-PIE 可执行文件才能启动 TSan。
- 公平性测试使用单 Worker 和 1 条指令的 quantum，确认短 Actor 能在长任务
  完成前处理消息；压力测试确认两个 Actor 同时运行且各自消息计数准确。

---

## Phase 13 — System Actors

进度：完成。

### 实现

- Console；
- Timer；
- File_System；
- blocking worker pool。

当前内建消息 API：`Console_Write(text, reply_to)`、
`File_Read(path, reply_to)`、`File_Write(path, contents, reply_to)`、
`Timer_After(delay_milliseconds, message, recipient)`。前三项通过各自的
`*_Completed(Result<..., String>)` Message 返回；Timer 到期时向 recipient
投递传入的 Message。文件内容按 UTF-8 文本处理，写入会覆盖目标文件。

### Gate

- Console 与文件调用由 bounded blocking pool 执行，Actor worker 只提交
  请求，不直接等待原生 I/O；
- I/O 和 Timer 结果均通过普通 mailbox Message 返回；
- 正常结束时排空请求；Scheduler 失败时取消排队请求与 Timer，并等待活动
  原生调用和服务线程退出后再释放 VM 状态。

验收：Windows GCC/Ninja 与 clang-cl 的 CTest 均通过 12/12；运行时测试通过
25/25。Linux GCC ASan/UBSan 的 `runtime_vm`、`blocking_pool` 和 `timer_service`
检查通过 3/3。端到端用例覆盖 OMBasic 源码发起 Console、文件读写和 Timer
请求，并验证失败时会取消待执行的长延时 Timer。架构决策记录在 ADR-0015。
Linux TSan 当前因容器启动时的 `unexpected memory mapping` 无法运行 `runtime_vm`；
进程未进入测试，因此没有 TSan race report。

---

## Phase 14 — Native FFI

进度：已完成。

### 实现

- `.omi` parser；
- C ABI types；
- dynamic library loader；
- ownership metadata；
- CString conversion；
- Native Record；
- callbacks；
- blocking metadata。

当前已完成独立 `.omi` parser、C ABI 类型/Native Record 布局、动态库加载、
UTF-8/CString 转换与 Owned pointer 释放接口，并使用 libffi 将 `.omi` 签名
绑定到 C 导出符号后进行 typed ABI dispatch。语义流水线可从
`Om_Resolver_Config` 提供的 `.omi` module 导入支持的函数名、ABI 参数/返回类型
与 effect，并自动生成 external bytecode 声明；VM 可按 module 批量建立并拥有
对应的 Native bindings。Actor 中标记为 `BLOCKING` 的调用会在 bounded blocking
pool 执行，Actor 保存调用栈并让出 scheduler worker，完成后恢复并压入 Native
返回值。C/C++ roundtrip、CString 与 Owned 返回值、foreign-thread callback、
BASIC 到 typed binding 的端到端执行，以及单 scheduler worker 下的 blocking
并发进度测试已通过。

`.omi` 项目加载器已能按调用方提供的规范相对路径从 source root 读取和解析
metadata，编译组合层仍需把 `Om_Omi_Project.modules` 显式传给 resolver config；
实现不扫描目录或猜测 `.omi` 依赖。Actor 外的 root 调用保持同步；`BLOCKING`
Actor 调用完成后通过 scheduler 内部 completion 状态恢复 Actor。
0.1 callback closure 仅在对应 Native 调用期间有效；Native 函数必须在返回前
join 所有调用 callback 的线程。

### Gate

- C roundtrip tests；
- C++ extern C wrapper tests；
- callback thread test；
- invalid ownership metadata diagnostics；
- resolver 提供 `.omi` module 后，BASIC 调用、typed external 生成、module binding
  和执行的端到端测试；
- 单 scheduler worker 下，`BLOCKING` Native 调用让出 worker、另一 Actor 推进并
  完成恢复的测试；
- Native Record 参数/返回值的 C ABI layout roundtrip。

Native Record ABI gate 已由 libffi 自定义 struct 类型覆盖：`.omi` 函数参数和返回值
可引用同一 block 中的 Native Record；绑定时逐字段建立 libffi 类型，并核对 libffi
计算出的 size/alignment 与 `.omi` 的 C layout。按值参数复制到对齐的临时缓冲区，
返回 struct 由 `Om_Native_Call_Result` 持有到销毁。Native Record 仍未导入 BASIC 源码
函数签名；callback 与 Record 字段不支持嵌套 Record。指针字段的字节 marshalling
不会复制或管理指针指向的内存，所有权仍由调用方和 Native wrapper 遵守 metadata
约定。clang-cl 与 GCC/Ninja 两套配置均通过完整 CTest（14/14）。

---

## Phase 15 — Distribution

进度：已实现。

已实现分布式基础设施：128-bit 随机 Node identity、version 3 的
68-byte big-endian frame codec、Message schema fingerprint，以及有序 TCP
stream listener/connect/send/receive API。完整 frame 默认限制为 16 MiB，接收端
先验证 header 与长度再分配 payload；传输不自动 retry，坏 frame 或部分写入会关闭
连接。另有 field-wise Message payload serializer，覆盖原始值、UTF-8、List/Map、
Record/Message 与分布式 ActorRef，并限制 payload、单个集合长度、总值数量和嵌套深度。双向
HELLO/HELLO_ACK 会检查协议版本、Node identity、可选的预期 peer，以及 program/interface
fingerprint；失败时关闭连接。OMBC v3 会持久化 program/interface fingerprint 与每个
Record/Message schema fingerprint。Program fingerprint 由 canonical module path 和源内容
计算；interface fingerprint 汇总稳定的 schema fingerprint。Schema 指纹来自 canonical
module path、实体名与有序结构类型指纹，类型标识为 64 位。两节点集成测试覆盖兼容握手后的
Message frame 发送及 fingerprint 不匹配拒绝。VM 现在可附加已握手的 peer，将对端 Actor
映射为本地 ActorRef，并把远端 `SEND` 路由到正常 Actor mailbox。`WATCH` 控制帧支持远端
watcher；远端 Actor 停止时发送 `Actor_Down`，peer 断开时本地 watcher 收到
`NODE_UNREACHABLE`。远端 payload 复用有界 serializer，接收端校验节点身份和 Message
schema fingerprint。`SPAWN ... AT node` 通过请求/应答帧在远端创建 Actor；远端 scheduler
在下一次 `om_vm_execute` 时先执行 Actor initializer，再处理已排队的 Message。设计记录在
ADR-0017。

当前 VM 初始化会生成 128-bit Node identity，并通过 runtime API 暴露协议版本、Node
identity 与所加载 OMBC 的 program/interface fingerprint；transport 握手 API 会在双向校验通过后返回对端 descriptor。

VM distribution API 可连接或接受 peer，并将连接所有权转移给 VM。远端 ActorRef 通过
`SEND` 发送完整 Message；同一 peer 的并发发送由连接锁串行化。接收线程只负责校验和
mailbox ingress，不运行 Actor handler。`WATCH`、远端停止通知和连接丢失通知沿用
`Actor_Down` Message mailbox 路径。远端 `SPAWN` 等待对端确认 Actor 已分配，再返回远端
ActorRef。协议不重试失败发送；没有重连后的 request 去重或 peer 身份认证。宿主必须继续
调用 `om_vm_execute` 执行已经排队的远端 Actor initializer 与 Message handler。

### 实现

- Node；
- Transport；
- frame protocol；
- Message serializer；
- remote ActorRef；
- routing；
- ordering；
- Watch remote；
- node disconnect handling；
- remote `SPAWN ... AT node`。

### Gate

- two-node integration；
- malformed network fuzz；
- at-most-once behavior tests；
- ordered sender/receiver tests。

VM 集成测试覆盖两节点 remote spawn/send、List/Map payload、远端 Watch 正常停止、
peer disconnect 的 `NODE_UNREACHABLE`，以及 Actor limit 下的 spawn 拒绝。协议测试覆盖
顺序传输、边界值与 5,000 组 malformed frame/payload。

---

## Phase 16 — Tooling

状态：已实现 formatter、结构化 diagnostics JSON、内存中的 Module Interface
metadata、基础 LSP、`ombasic bind`、`run`（编译并执行 `main`/`--entry`）与
`build`（输出 OMBC bytecode image）。LSP 当前通过编译器子进程分析每次请求，
尚无跨请求增量分析缓存。

### 实现

- formatter；
- structured diagnostics JSON；
- interface metadata cache；
- early LSP；
- `ombasic bind`。

### Gate

- formatter idempotent；
- hover/inlay uses compiler semantic model；
- no duplicated parser/type logic in LSP。

当前门槛由 `tooling_formatter` 和 `tooling_cli` 覆盖：formatter 二次格式化
结果不变；LSP 的 hover/inlay 类型在编辑源代码后随 semantic model 更新；LSP
本身只处理 JSON-RPC 和源码位置映射，不实现第二套 parser/type checker。

---

## Phase 17 — Incremental Build / Optimization

只在 correctness baseline 稳定后。

可选：

- module cache；
- parallel module compile；
- persistent collections；
- bytecode peephole；
- scheduler work stealing；
- compact Value representation。

所有优化先 benchmark，再 ADR。

---

# 59. Issue 拆分原则

每个 issue 应是一个可验证能力，而不是“实现 compiler”。

好：

```text
Implement lower_snake_case validator
Reject parent path segments in IMPORT
Implement occurs-check for TypeVar
Verify SEND target Actor protocol
Implement actor mailbox FIFO
```

不好：

```text
Improve parser
Fix types
Actor stuff
Refactor runtime
```

每个 issue 必须包含：

```text
Problem
Scope
Invariants
Public API impact
Tests
Out of scope
```

这正好符合 Explicit Architecture 的有限上下文目标。

---

# 60. Pull Request Gate

每个 PR 检查：

1. 改了哪个 capability？
2. 是否新增反向依赖？
3. 是否新增 `common/utils` 式抽象？
4. 是否改变共享不变量？
5. 是否新增隐藏控制流？
6. 是否新增 raw allocator 使用？
7. 是否有测试？
8. 是否需要 ADR？
9. 是否影响 spec？
10. sanitizer 是否通过？

---

# 61. 代码 Review Checklist

## C Safety

- 所有 buffer 有 length？
- 所有 size arithmetic checked？
- ownership 清楚？
- raw pointer 是否会跨 realloc？
- user input 是否走 assert？
- cleanup path 是否完整？

## Compiler

- AST 是否保持 syntax-only？
- semantic rule 是否放在唯一 authority？
- diagnostic 是否有 span？
- parser recovery 是否推进 token？

## Actor

- 是否可能同时执行同一 Actor？
- 是否把 Actor heap pointer 发送到外部线程？
- 是否出现隐藏 blocking？
- handler 是否保持 run-to-completion？

## Native

- ownership？
- blocking？
- callback thread？
- pointer 是否被序列化？

---

# 62. 主要风险清单

## Risk 1：类型推导复杂度失控

缓解：

- 0.1 保持最小 HM subset；
- no subtyping maze；
- no traits/typeclasses；
- no overload；
- 类型规则先写 conformance。

## Risk 2：Actor Runtime 过早并发

缓解：

- 先 deterministic single-thread；
- 再 M:N；
- TSan gate。

## Risk 3：FFI 破坏 isolation

缓解：

- Native boundary module；
- callback enqueue only；
- handle owner；
- Native pointer non-serializable。

## Risk 4：分布式拖垮 0.1

缓解：

- local Actor semantics first；
- transport late；
- at-most-once；
- strict build fingerprint；
- no discovery/consensus。

## Risk 5：C 内存错误

缓解：

- Arena + ID；
- Safe C23 Profile；
- ASan/UBSan/MSan/TSan；
- fuzz；
- allocator API restrictions。

## Risk 6：Foundation 变成 god module

缓解：

- Foundation 不能含 compiler/runtime domain logic；
- architecture check；
- 每个新增 helper 说明为何跨 capability。

## Risk 7：Bytecode 与 Runtime 紧耦合

缓解：

- bytecode/format 是独立稳定 contract；
- verifier 独立；
- compiler emitter 与 VM 都依赖 format，而不是互相依赖。

---

# 63. 未来可演进方向

不是 0.1 Gate：

- Native AOT backend；
- LLVM backend；
- self-hosting；
- shared immutable large binary；
- persistent List/Map；
- generational actor heap；
- debugger；
- profiler UI；
- hot code upgrade；
- schema evolution；
- supervision DSL；
- secure cluster identity；
- compiler server；
- distributed build。

任何新增能力必须回答 Explicit Architecture 决策问题：

```text
谁拥有？
结构是否可见？
控制流是否可追踪？
依赖是否稳定？
复杂度解决什么真实问题？
修改能否局部化？
能否独立测试？
AI/人是否只需有限上下文？
如何自动验证？
```

---

# 64. 第一批实际开发任务

建议按以下顺序直接建立 issue：

1. 建立 repository/CMake/CTest。
2. 写 ADR-0001 C23。
3. 写 ADR-0002 Explicit Architecture dependency rules。
4. 建立 architecture-check script。
5. 实现 `om_alloc_array` checked arithmetic。
6. 实现 Arena。
7. 实现 Slice/String_View。
8. 实现 Vector。
9. 实现 UTF-8 validator。
10. 实现 Source_Text / Span。
11. 实现 name rules。
12. 实现 Token/Keyword table。
13. 实现 lexer numbers/identifiers/operators。
14. 实现 comments/doc comments。
15. 实现 strings/interpolation。
16. 接入 lexer fuzz。
17. 实现 AST node storage。
18. 实现 Pratt expression parser。
19. 实现 declarations/control parser。
20. 实现 parser recovery。
21. 接入 parser fuzz。
22. 实现 import path validator。
23. 实现 module DAG/cycle diagnostics。
24. 实现 symbols/scopes。
25. 实现 Type Store。
26. 实现 unification。
27. 实现 primitive inference。
28. 实现 Record/List/Map inference。
29. 实现 Function/Action inference。
30. 实现 Result/TRY typing。
31. 实现 effect checker。
32. 实现 Actor protocol checker。
33. 实现 serializable checker。
34. 冻结 bytecode format ADR。
35. 实现 bytecode builder/verifier。
36. 实现 VM primitive execution。
37. 实现 object/string/list/map。
38. 实现 Result/TRY bytecode execution。
39. 实现 deterministic Actor runtime。
40. 实现 actor-local GC。
41. 实现 multi-thread scheduler。
42. 实现 system actors。
43. 实现 Native `.omi` parser。
44. 实现 C ABI bridge。
45. 实现 callback ingress。
46. 冻结 distribution protocol ADR。
47. 实现 serializer。
48. 实现 two-node transport。
49. 实现 remote watch/down。
50. 实现 formatter。
51. 实现 interface cache。
52. 实现基础 LSP。

---

# 65. Definition of Done：单个 Capability

一个 capability 只有满足以下条件才算完成：

- public contract documented；
- ownership documented；
- invariants documented；
- dependency direction compliant；
- unit tests；
- invalid input tests；
- sanitizer clean；
- error diagnostics clear；
- no TODO required for correctness；
- architecture docs updated if boundary changed。

---

# 66. Definition of Done：OMBasic 0.1 Reference Implementation

最终发布 Gate：

## Compiler

- full core grammar；
- import DAG；
- static type inference；
- effect checking；
- Actor protocol checking；
- bytecode compiler；
- deterministic diagnostics。

## Runtime

- VM；
- local Actors；
- Actor-local GC；
- M:N scheduler；
- system I/O Actor model；
- C ABI FFI；
- basic distribution。

## Safety

- ASan/UBSan clean；
- TSan actor suite clean；
- fuzz campaigns no known crash；
- no known architecture dependency violation。

## Tooling

- `check`；
- `run`；
- `build`；
- `fmt`；
- structured diagnostics。

## Documentation

- language standard；
- architecture overview；
- all accepted ADRs；
- bytecode format；
- Native ABI contract；
- distribution wire protocol；
- contribution guide；
- C23 safety profile。

---

# 67. 推荐开发哲学

编译器实现本身必须成为 OMBasic 理念的示范项目。

不要写：

> “这段代码虽然难懂，但是性能好。”

除非 benchmark 证明它解决了真实瓶颈，并有文档和测试保护。

不要写：

> “这个 global callback 会自动做下一步。”

而应有清晰 orchestrator。

不要写：

> “所有地方都用这个 utils。”

而应确认能力归属。

不要写：

> “未来可能需要，所以先做抽象层。”

而应等真实变化原因出现。

推荐始终遵守：

> **先让正确性显式，再让性能变快；先让边界稳定，再增加抽象；先让单机 Actor 正确，再做分布式。**

---

# 68. 参考资料

## OMBasic

- `OMBasic-Language-Standard.md`

## Explicit Architecture

- https://github.com/risol/ExplicitArchitecture
- https://github.com/risol/ExplicitArchitecture/blob/master/README.zh-CN.md

本计划主要采用其以下思想：

- Capability & Kernel Boundaries；
- Structural Transparency；
- Responsibility Cohesion；
- Explicit Control Flow；
- Explicit Dependencies & Assembly；
- Necessary Complexity；
- Local Change & Shared Invariants；
- Composable Testability；
- Bounded Context；
- Layered Feedback & Executable Constraints。

## C23

- ISO/IEC 9899:2024（C23）
- `<stdckdint.h>` checked integer arithmetic

## LLVM Sanitizers

- https://clang.llvm.org/docs/AddressSanitizer.html
- https://clang.llvm.org/docs/MemorySanitizer.html
- https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html
- https://clang.llvm.org/docs/ThreadSanitizer.html

---

# 69. 最终架构宣言

OMBasic 编译器与 Runtime 应遵循：

> **Make Structure Visible. Make Behavior Explicit. Keep Dependencies Clear. Preserve Shared Invariants. Keep Changes Local. Make Tests Composable.**

在 OMBasic 项目中的具体含义是：

> **编译阶段必须可见，运行路径必须可追踪，模块依赖必须单向，语言规则必须有唯一权威，Actor 状态必须有明确主人，Native 边界必须显式，C 内存风险必须通过架构和工具共同约束。**

