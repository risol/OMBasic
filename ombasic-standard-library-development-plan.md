# OMBasic 标准库完整开发计划

> **文档类型：** 架构与实施路线图\
> **版本：** 1.0-draft\
> **日期：** 2026-10-30\
> **适用范围：** OMBasic 0.1 及后续标准库演进\
> **状态：** 开发计划草案，需通过 ADR 和代码评审逐项确认

------------------------------------------------------------------------

## 1. 执行摘要

本计划的目标不是单纯增加标准库函数数量，而是在不破坏 OMBasic
核心语言模型的前提下，形成一套完整、可测试、跨平台、可随 SDK
发布的标准库。

OMBasic 的并发和异步模型必须保持唯一：

-   **同步计算：** 使用 `FUNCTION`。
-   **显式副作用：** 使用 `ACTION`。
-   **并发执行与长期状态：** 使用 `ACTOR`。
-   **异步请求与结果通知：** 使用 `MESSAGE`、`SEND`、`RECEIVE` 和
    Mailbox。
-   **错误结果：** 使用 `Result<T, E>` 或明确的系统错误消息。
-   **Native 能力：** 通过受控的 `.omi` ABI 边界提供。

本计划最重要的架构约束是：

> **禁止 Actor 模型之外的用户级异步
> API。任何需要异步完成的标准库操作，都必须通过 Actor + Message +
> Mailbox 暴露。**

因此，标准库不得引入或包装出另一套异步机制，包括：

-   `async` / `await`
-   `Future` / `Promise`
-   Coroutine / Task
-   Callback-based Async API
-   隐式 continuation、挂起与恢复
-   让普通 `FUNCTION` 看起来同步、实际在内部等待异步结果的包装
-   暴露给用户的独立事件循环或异步调度器

Runtime 内部可以使用 Blocking Pool、平台 I/O
API、线程或其他实现手段，但这些属于系统 Actor
的内部实现细节。用户代码只能通过 Actor 协议与这些服务交互。

### 1.1 最终目标

标准库 v1 应当让用户能够用 OMBasic 开发：

1.  命令行工具；
2.  配置文件和 JSON 数据处理程序；
3.  文件处理与目录管理程序；
4.  定时任务和 Actor 并发程序；
5.  TCP/UDP 网络服务；
6.  结构化日志和后台服务；
7.  需要调用 Native 系统能力的应用程序。

### 1.2 实施优先级

推荐顺序：

1.  标准库架构规则与现状盘点；
2.  JSON v1 完善；
3.  Collections v1；
4.  File System v1；
5.  Time v1；
6.  Console v1；
7.  Byte / Buffer 与 Encoding；
8.  Network v1；
9.  Logging 与 Random；
10. Process 及其他系统服务。

不要同时启动 Compiler、Runtime、Native ABI
和多个大型标准库模块的无边界改造。每个阶段必须先明确公共 API
和错误语义，再实现内部代码。

------------------------------------------------------------------------

## 2. 当前项目基线

本节以仓库当前语言标准、标准库计划和模块文档为基线。实现状态发生变化时，应以代码和对应测试为准，不应仅凭本计划推断功能已经完成。

### 2.1 已有架构基础

OMBasic 已经具备以下关键基础：

-   静态类型推导；
-   `FUNCTION`、`ACTION`、`ACTOR`；
-   Actor 消息发送与接收；
-   `Record`、`Message`、`List`、`Map`、`Result`；
-   Native AOT；
-   GC 与 Runtime；
-   Native ABI 元数据 `.omi`；
-   显式 `@std/` 标准库导入根；
-   SDK 标准库 manifest 与内容摘要；
-   模块 Facade 和显式 Re-export 机制；
-   Linux / Windows 构建与测试基础。

语言标准已经明确不提供 Async、Await、Future、Promise、Coroutine 和隐式
continuation；外部结果需要在当前 `RECEIVE` 处理结束后，通过新的 Message
返回。标准库必须遵守这一语言约束，不能绕开它。

### 2.2 当前标准库模块盘点

  -------------------------------------------------------------------------------------------
  模块                    当前基础                   下一步
  ----------------------- -------------------------- ----------------------------------------
  Math                    已有整数 Math Facade       补充 Float 数学能力，明确边界语义
                          与基础函数

  Text                    已有 String 辅助函数与     补充常用文本操作和完整测试
                          Unicode 处理边界

  Path                    已有纯字符串路径操作       完善跨平台路径语义，不混入文件系统访问

  JSON                    已有                       完善 Value Accessors、对象查询、数字转换
                          Value、Parser、Stringify
                          和 Native 私有辅助

  Console                 已有 Console Actor 和文本  统一输入、输出与错误协议
                          Payload 辅助

  File System             已有文本读取、覆盖写入     增加目录、追加、元数据等能力
                          Actor 服务

  Timer                   已有 Timer Actor           设计取消、时钟查询和更完整的时间 API
                          和单调时钟 deadline

  Collections             语言已有 List / Map 基础   建立稳定的标准库集合操作 API

  Encoding                尚需明确 Byte / Buffer     先完成类型和 ABI 设计
                          表示

  Network                 Runtime 已有分布式通信基础 设计面向应用程序的 Socket Actor API

  Logging                 尚需形成统一公共 API       设计 Logger Actor 和日志事件模型

  Random                  尚需形成统一公共 API       区分可复现 PRNG 与系统安全随机源

  Process                 尚需形成统一公共 API       通过 Process Actor 管理外部进程
  -------------------------------------------------------------------------------------------

### 2.3 需要特别保留的现有语义

**JSON**

当前 JSON 使用不可变 `Value(encoded)` Record 保存一个紧凑 JSON
值。当前设计保留数字原始文本、对象键顺序和字符串转义形式，并对输入大小、嵌套深度、UTF-8、重复键等进行限制。不要为了增加访问器而贸然改成递归
Native 指针树或引入新的语言类型系统特性。

**File System**

当前 File System 通过系统 `File_System` Actor 提供 UTF-8
文本读取和覆盖写入。请求携带 Reply Actor，完成后通过 Completion Message
返回 `Result`。底层阻塞工作由 Runtime Worker Pool
执行。当前路径按进程工作目录解释，不等同于 Project Source Root 或 SDK
Root。

**Timer**

当前 Timer 通过 `Timer_After` 向目标 Actor 投递指定
Message，使用单调时钟计算 deadline。它不提供同步
sleep，也不保证精确到毫秒送达。当前没有取消句柄或 Wall Clock API。

**Console**

当前 Console Actor 负责异步输出；`console_api` 辅助函数只准备文本
Payload，不负责隐藏发送、等待或完成通知。现有 Prelude `print`
的语义不应在没有单独决策的情况下改变。

------------------------------------------------------------------------

## 3. 不可违反的架构规则

以下规则作为所有标准库 API Review 的强制检查项。

### 3.1 API 类型规则

  ----------------------------------------------------------------------------
  操作类型                 允许的接口              禁止的接口
  ------------------------ ----------------------- ---------------------------
  纯计算                   `FUNCTION`              隐式读写外部状态

  纯文本、路径、数学处理   `FUNCTION`              隐藏系统调用

  显式副作用               `ACTION`                把副作用伪装成纯函数

  异步 I/O                 Actor Request Message + Future、Promise、Callback
                           Completion Message

  长期可变状态             Actor State             跨 Actor 共享普通可变对象

  定时投递                 Timer Actor + Message   阻塞式 sleep 作为异步替代品

  Native 系统能力          `.omi` + Runtime/Actor  在普通 BASIC API
                           边界                    中暴露裸指针和隐藏线程
  ----------------------------------------------------------------------------

### 3.2 不得隐藏异步

以下设计均不允许：

``` basic
' 禁止：普通函数内部等待文件读取完成
contents = file_system.read_text("config.json")

' 禁止：Future / Promise 风格
future = file_system.read_async("config.json")
contents = await future

' 禁止：回调式 API
file_system.read_async("config.json", on_completed)
```

正确的设计必须显式表现 Actor 边界：

``` basic
' 示意：请求发给 File_System Actor
SEND File_System, File_Read("config.json", reply_actor)
```

请求完成后，由 `reply_actor` 在 `RECEIVE` 中处理对应的 Completion
Message。

以上代码用于表达协议形态；实际 Message 名称、字段和 Actor
类型必须以模块公开接口为准。

### 3.3 Actor 服务统一协议

每个异步系统服务必须定义：

1.  **Request Message**：操作、参数和 Reply Actor。
2.  **Completion Message**：成功或失败结果。
3.  **Reply Protocol**：接收方必须实现的消息协议。
4.  **错误模型**：稳定错误类别和必要上下文。
5.  **资源生命周期**：服务、连接、句柄和 Actor 终止时的行为。
6.  **并发语义**：同一 Actor 多个请求的处理顺序和隔离规则。
7.  **失败语义**：服务停止、请求拒绝、系统资源耗尽时如何通知调用方。
8.  **容量策略**：Mailbox、队列、缓冲区和单请求资源上限。
9.  **测试契约**：成功、失败、边界、并发和关闭场景。

异步请求不一定都需要独立 Completion Message；例如 Timer
可以按约定直接向目标 Actor 投递用户指定的
Message。但这必须是明确的协议设计，不能依赖隐式恢复调用栈。

### 3.4 错误处理规则

-   可预期的操作失败必须通过 `Result<T, E>` 或明确的错误 Message 表达。
-   不得依赖操作系统本地化错误文本作为程序判断条件。
-   公共错误码必须稳定；错误描述文本可以因平台而不同。
-   不得把超时、取消、拒绝、服务关闭和操作系统 I/O
    失败混成一个含义不清的错误。
-   语言级 Runtime Fault 与普通业务错误必须区分。
-   不得通过 `NULL`、空字符串或伪造的默认值静默吞掉失败。

### 3.5 同步与阻塞边界

-   纯计算 API 可以同步执行。
-   文本、路径、JSON、集合等纯内存操作可以同步执行。
-   需要等待外部 I/O、网络、进程或系统服务的操作，应通过 Actor
    协议暴露。
-   Runtime 可以在系统 Actor 内部使用 Blocking Pool，但不能把
    Worker、Future 或 Callback 暴露给 BASIC 调用方。
-   不得在 `FUNCTION` 中执行文件、网络、时钟或其他外部副作用。
-   如果某个操作确实是同步且可能阻塞，必须明确记录其阻塞属性；不得将它包装成看似廉价的普通函数。

### 3.6 Native 实现规则

实现优先级：

1.  普通 OMBasic `.bas`；
2.  已有 Runtime 服务；
3.  最小化的 Native C 实现；
4.  必要时才修改 Compiler / Runtime ABI。

Native 代码只负责 BASIC
当前无法安全或高效实现的底层能力，例如操作系统调用、UTF-8
边界处理、解析器热点或字节缓冲区管理。

每个 Native API 必须定义：

-   ABI 参数和结果类型；
-   Effect；
-   Blocking 属性；
-   指针所有权；
-   Owned / Borrowed 生命周期；
-   错误映射；
-   跨平台实现；
-   Native 单元测试与 Sanitizer 覆盖。

标准库不能通过大量 Native 函数把 OMBasic 源码层变成薄薄的 C API 外壳。

------------------------------------------------------------------------

## 4. 标准库目录与模块组织

建议沿用当前显式模块导入和 Facade 机制。

``` text
stdlib/
├── math/
│   ├── math.bas
│   ├── integer.bas
│   └── float.bas
├── text/
│   ├── text.bas
│   └── ...
├── path/
│   ├── path.bas
│   └── ...
├── json/
│   ├── json.bas
│   ├── value.bas
│   ├── parser.bas
│   ├── stringify.bas
│   ├── json_error.bas
│   └── private/
├── collections/
│   ├── collections.bas
│   ├── list.bas
│   └── map.bas
├── console/
│   ├── console.bas
│   └── console_api.bas
├── file_system/
│   ├── file_system.bas
│   ├── file_error.bas
│   └── ...
├── time/
│   ├── time.bas
│   ├── duration.bas
│   ├── instant.bas
│   └── datetime.bas
├── encoding/
│   ├── encoding.bas
│   ├── hex.bas
│   └── base64.bas
├── random/
│   ├── random.bas
│   └── ...
├── network/
│   ├── network.bas
│   ├── tcp.bas
│   └── udp.bas
├── logging/
│   ├── logging.bas
│   └── ...
└── process/
    ├── process.bas
    └── ...
```

这是目标组织结构，不表示所有文件都必须立即创建。每个模块目录的 `x/x.bas`
是推荐公共 Facade；调用方仍然显式导入 `.bas` 文件。

### 4.1 导入规则

-   标准库只能通过 `@std/` 显式导入。
-   不增加隐式模块搜索路径。
-   不增加目录自动导入。
-   不增加包管理器、依赖求解器或 lockfile。
-   Facade 只显式 Re-export 公共符号。
-   私有实现文件不得意外进入公共 API。
-   标准库不得依赖消费项目的源码目录。
-   模块依赖必须无循环，并保持可审查的依赖方向。

### 4.2 文档布局

``` text
docs/stdlib/
├── overview.md
├── architecture.md
├── json.md
├── collections.md
├── file_system.md
├── time.md
├── console.md
├── encoding.md
├── network.md
├── logging.md
├── random.md
└── process.md
```

每个公开模块至少需要：

-   功能范围；
-   导入方式；
-   公共类型和函数；
-   Message 协议；
-   错误类型和错误码；
-   同步 / 异步分类；
-   阻塞与资源限制；
-   跨平台差异；
-   示例；
-   Conformance 测试清单。

------------------------------------------------------------------------

## 5. 各模块开发计划

## 5.1 Math

**目标：** 提供稳定、可预测的整数和浮点数学函数。

### 当前基础

已有整数 Math Facade 和基础函数。

### 建议扩展

整数：

-   `abs`
-   `min`
-   `max`
-   `clamp`
-   `sign`
-   `is_even`
-   `is_odd`

浮点：

-   `sqrt`
-   `sin`
-   `cos`
-   `tan`
-   `asin`
-   `acos`
-   `atan`
-   `atan2`
-   `exp`
-   `log`
-   `log10`
-   `floor`
-   `ceil`
-   `round`
-   `trunc`
-   `is_finite`
-   `is_nan`

### 语义要求

-   Int 运算继续遵循 checked overflow。
-   `abs(Int64_MIN)` 必须有明确的溢出行为。
-   Float 函数必须说明 NaN、Infinity、负零和超出定义域时的行为。
-   不得静默把 Int 转为 Float。
-   数值函数应优先通过标准 Native 数学库实现，不重复编写低质量近似算法。
-   纯数学 API 不得读取系统时钟或随机源。

### 验收

-   正常值、边界值、溢出、NaN、Infinity、负零和特殊定义域测试。
-   Linux / Windows 结果符合已定义的容差。
-   API 文档和实际返回语义一致。

------------------------------------------------------------------------

## 5.2 Text

**目标：** 形成稳定的 Unicode 文本处理 API。

### 建议 API

-   `length`
-   `is_empty`
-   `equals`
-   `concat`
-   `contains`
-   `starts_with`
-   `ends_with`
-   `index_of`
-   `replace`
-   `split`
-   `join`
-   `trim`
-   `trim_start`
-   `trim_end`
-   `to_lower`
-   `to_upper`
-   `substring` 或明确命名的切片函数

### 语义要求

-   明确区分 UTF-8 字节数、Unicode Scalar Value 数量和未来可能支持的
    Grapheme Cluster 数量。
-   现有 `length` 语义不得在没有版本决策的情况下改变。
-   索引单位必须写入文档；不允许不同函数悄悄采用不同单位。
-   非法 UTF-8 必须按统一边界规则处理。
-   大小写转换必须说明 Unicode 数据版本和平台一致性策略。
-   String 是文本类型，不得用来承载任意二进制数据。
-   不默认引入完整 ICU 依赖；复杂区域化文本功能可后续独立评估。

### 验收

覆盖 ASCII、多字节
UTF-8、组合字符、空字符串、边界索引、非法输入和大字符串。

------------------------------------------------------------------------

## 5.3 Path

**目标：** 提供纯路径字符串处理，不承担文件系统 I/O。

### 建议 API

-   `is_absolute`
-   `join`
-   `normalize`
-   `parent`
-   `filename`
-   `extension`
-   `stem`
-   `change_extension`
-   `separator`（若确有必要，必须明确平台语义）

### 语义要求

-   Path 模块是纯字符串操作，不访问磁盘。
-   逻辑路径规范化与真实文件系统解析必须区分。
-   `normalize` 不得声称已经验证文件存在或解析了符号链接。
-   OS 路径、Project Source Root、SDK Root 和 Import
    路径必须保持不同的安全边界。
-   文件系统 API 必须明确相对路径以进程工作目录还是其他目录为基准。
-   不得把 Import 路径规则直接复用为用户文件路径安全策略。

### 验收

Linux、Windows
路径样例；绝对路径、相对路径、盘符、UNC、重复分隔符、`.`、`..`、根路径和空路径测试。

------------------------------------------------------------------------

## 5.4 JSON v1 完善

**优先级：P0。**

**目标：** 让用户能够完整地解析、查询、构造和序列化
JSON，而不丢失数字精度。

### 当前基础

当前公共模型使用不可变 `Value(encoded)`
Record；解析器保留数字原始文本和对象键顺序，并对
UTF-8、重复键、深度和输入大小进行限制。解析和序列化通过普通 BASIC Facade
暴露，底层可使用私有 Native 辅助。

### 建议补齐的 API

  ----------------------------------------------------------------------------------------
  API                        目的                    结果建议
  -------------------------- ----------------------- -------------------------------------
  `as_string(value)`         读取 String             `Result<String, Parse_Error>`

  `as_boolean(value)`        读取 Bool               `Result<Bool, Parse_Error>`

  `as_number_text(value)`    读取原始数字文本        `Result<String, Parse_Error>`

  `is_null(value)`           判断 Null               `Result<Bool, Parse_Error>`

  `object_has(value, key)`   判断对象字段是否存在    `Result<Bool, Parse_Error>`

  `object_length(value)`     获取对象字段数量        `Result<Int, Parse_Error>`

  `object_keys(value)`       获取对象键集合          `Result<List<String>, Parse_Error>`

  `object_entries(value)`    获取有序键值项          `Result<List<Entry>, Parse_Error>`
  ----------------------------------------------------------------------------------------

根据当前类型系统和 API 评审结果，再决定是否增加：

-   `number_as_int64`
-   `number_as_float64`
-   `array_to_list`
-   `object_get_or`
-   明确语义的 JSON Pointer / Path 查询

### 数字精度规则

-   JSON Number 首先保留原始十进制文本。
-   不得在解析时默认转换成 Float64。
-   Int64 转换必须检查范围和整数格式。
-   Float64 转换必须明确精度损失和非有限值策略。
-   需要精确保留的场景必须能够重新取得原始数字文本。

### 对象查询规则

-   必须区分字段不存在、字段存在但为 JSON Null、字段类型不匹配。
-   对象键的顺序语义必须保持一致。
-   重复键策略必须保持稳定；当前拒绝重复解码键的行为不能被静默改变。

### 不建议现在做的事情

-   不为了 JSON 单独增加递归 Tagged Union。
-   不把 JSON.Value 改成裸 Native 指针对象。
-   不引入普通 `Any`。
-   不在解析失败时返回伪造的空对象。
-   不为方便而牺牲大数字精度。

### 测试与验收

-   RFC 8259 有效与无效样例；
-   String 转义和 Unicode Surrogate；
-   重复解码键；
-   数字边界和超大数字；
-   对象键顺序；
-   缺失字段与 Null；
-   非法 UTF-8；
-   深度和大小限制；
-   构造、解析、Stringify 往返；
-   SDK relocation；
-   Native Parser 单元测试与 BASIC Facade Conformance 测试。

------------------------------------------------------------------------

## 5.5 Collections v1

**优先级：P1。**

**目标：** 为现有不可变 List / Map 提供稳定的常用操作。

### List API 候选

-   `length`
-   `is_empty`
-   `contains`
-   `index_of`
-   `first`
-   `last`
-   `take`
-   `drop`
-   `reverse`
-   `concat`
-   `slice`（必须先确定边界规则）
-   `map`
-   `filter`
-   `fold`

### Map API 候选

-   `length`
-   `is_empty`
-   `contains_key`
-   `keys`
-   `values`
-   `entries`
-   `get_or`
-   `remove`（返回新 Map）
-   `set`（返回新 Map）

### 语义要求

-   List / Map 默认不可变。
-   `set`、`remove`、`append`
    等操作如果提供，必须返回新集合，不得悄悄修改输入集合。
-   不跨 Actor 共享普通可变容器。
-   保持 Map 当前已定义的键类型限制。
-   保持已定义的 Map 遍历顺序；当前实现按插入顺序迭代，除非通过 ADR
    变更，不得改变。
-   空集合的类型推导必须遵守语言规则。
-   不引入 `Any` 作为集合元素逃生口。
-   相等比较必须遵循语言已经定义的类型相等语义。
-   越界、缺失 Key 和非法范围必须有明确结果，不得依赖崩溃或未定义行为。

### 高阶函数的实施门槛

`map`、`filter`、`fold` 依赖 Function
值传递、间接调用和泛型推导。虽然编译器已有相关基础，仍必须通过实际端到端测试确认：

-   函数值的类型推导；
-   参数和返回值的泛型特化；
-   递归或间接调用边界；
-   GC safepoint 下函数参数和集合元素的存活；
-   是否需要闭包捕获。

如果闭包语义尚未定义，第一版只支持已命名的 Function
值，不得假设存在闭包。

### 实施策略

先实现纯 `.bas` 集合操作；只有性能测量证明必要时，才增加 Native
快路径。不要为了几十个便利函数修改 Compiler Builtin。

### 验收

空集合、单元素、大集合、重复元素、不同 Key 类型、缺失 Key、索引边界、Map
插入顺序、GC 存活和高阶函数测试。

------------------------------------------------------------------------

## 5.6 File System v1

**优先级：P2。**

**目标：** 将现有文本文件 Actor 服务扩展成可用于实际应用的文件系统服务。

### 当前能力

-   UTF-8 文本读取；
-   UTF-8 文本覆盖写入；
-   Request Message 携带 Reply Actor；
-   Completion Message 返回 Result；
-   阻塞系统调用由 Runtime Worker Pool 执行。

### 第一批建议 API

  操作         Request              Completion
  ------------ -------------------- ------------------------------
  读取文本     `File_Read`          `File_Read_Completed`
  覆盖写入     `File_Write`         `File_Write_Completed`
  追加文本     `File_Append`        `File_Append_Completed`
  判断存在     `File_Exists`        `File_Exists_Completed`
  删除文件     `File_Delete`        `File_Delete_Completed`
  创建目录     `Directory_Create`   `Directory_Create_Completed`
  列出目录     `Directory_List`     `Directory_List_Completed`
  获取元数据   `File_Stat`          `File_Stat_Completed`

以上是协议设计候选，不代表这些 Message
已经存在。正式实现前必须确认命名、字段、错误类型和版本兼容策略。

### 错误模型

建议设计稳定的 `File_Error`，至少区分：

-   Not Found；
-   Permission Denied；
-   Already Exists；
-   Not Directory；
-   Is Directory；
-   Invalid Path；
-   Invalid UTF-8；
-   Resource Exhausted；
-   I/O Error；
-   Unsupported Operation。

错误码必须稳定。操作系统原始错误码可以作为诊断信息，但不能要求业务代码匹配本地化错误字符串。

当前接口使用 `Result<String, String>`。如果升级为
`File_Error`，必须通过明确的标准库版本策略或新增消息版本完成，不能静默破坏已有调用方。

### 路径与安全

-   默认继续明确以进程工作目录解释相对路径，除非单独设计工作目录上下文。
-   不将 `@std/`、Project Source Root 或 SDK Root 作为文件操作根目录。
-   明确符号链接、重解析点和路径穿越的处理策略。
-   文件访问权限由操作系统权限模型决定；不得宣称 Path.normalize
    已提供沙箱保护。
-   目录遍历必须定义资源上限。
-   目录返回顺序必须明确；建议公共 API
    返回确定性排序结果，避免测试和业务逻辑依赖平台目录顺序。

### 二进制 I/O

二进制文件读写依赖明确的 Byte / Buffer 表示，应放到 Encoding / Buffer
设计之后。不得把任意字节强行塞入 UTF-8 String。

### 不做的事情

-   不提供 `read_text_sync` 作为异步 API 的隐藏包装。
-   不提供 Future 或 Callback 版本。
-   不在普通 Function 内等待文件操作。
-   第一版不必同时实现文件锁、文件监控、内存映射、复杂权限 ACL
    和事务文件系统。

### 验收

文本往返、覆盖和追加、缺失文件、权限失败、目录目标、非法
UTF-8、目录创建和遍历、符号链接策略、资源上限、服务关闭和并发请求测试。

------------------------------------------------------------------------

## 5.7 Time v1

**优先级：P3。**

**目标：** 将 Duration、Monotonic Instant、Wall Clock DateTime 和 Timer
明确分开。

### 类型与职责

  概念       含义             主要用途
  ---------- ---------------- -----------------------------
  Duration   时间长度         时间间隔计算
  Instant    单调时钟时间点   测量耗时、deadline
  DateTime   日历时间         UTC 时间戳、日期展示
  Timer      定时投递服务     到期后向 Actor 发送 Message

### 同步纯函数

Duration 的单位转换、比较、加减、格式化等可以是纯 Function。DateTime
的纯解析和格式化也可以是纯 Function。

### Actor 服务

获取当前系统时间、请求系统时钟或需要异步完成的时钟操作，应通过明确的
Clock Actor 协议提供，例如：

-   `Clock_Now`；
-   `Clock_Now_Completed`。

具体协议应在 ADR 中确定，不应在纯 Function 中偷偷读取系统时钟。

Timer 继续使用 Timer Actor：

``` basic
SEND Timer, Timer_After(1000, Tick(), recipient)
```

### 语义要求

-   Monotonic Clock 用于耗时和 deadline。
-   Wall Clock 用于日期和时间戳。
-   两者不可混为同一时间轴。
-   Instant 不应被当成跨进程或跨机器可比较的日历时间。
-   Timer 的毫秒参数表示单位，不代表精确送达保证。
-   deadline 溢出、负延迟和系统关闭行为必须有定义。
-   不提供阻塞式 `sleep` 作为 Actor Timer 的替代。
-   时区数据库和复杂区域化日历可以后续单独规划。

### 后续增强

-   Timer cancellation；
-   Clock Actor；
-   UTC DateTime；
-   明确的 Unix 时间戳转换；
-   时区和 DST；
-   更高精度 Duration。

取消能力必须使用明确的 Actor Message 和句柄/标识协议，不得引入取消
Token + Future 一类的另一套抽象。

### 验收

单调性、负延迟、deadline 溢出、Mailbox 顺序、Runtime
Shutdown、时区边界和跨平台测试。

------------------------------------------------------------------------

## 5.8 Console v1

**优先级：P4。**

**目标：** 保持现有 Console Actor 模型，并形成统一的输入输出协议。

### 输出

-   保留当前 Console Actor 输出模型；
-   保留明确的 Completion Message；
-   `write_payload` / `write_line_payload` 只负责文本准备；
-   不在辅助 Function 中自动 Send 或等待完成；
-   定义多 Actor 同时输出时的消息顺序保证。

### 输入

可以增加 Actor 化输入协议：

-   `Console_Read_Line`；
-   `Console_Read_Line_Completed`；
-   明确 EOF；
-   明确终端输入失败；
-   明确取消或关闭行为（如实现）。

### Prelude `print`

现有 `print`
的语义必须单独盘点。不能为了统一命名而无意改变其同步/输出行为。若要迁移到
Console Actor，应通过独立设计和兼容性决策完成。

### 验收

UTF-8 输出、换行、空字符串、并发输出顺序、输入
EOF、管道重定向、终端错误和 Windows / Linux 测试。

------------------------------------------------------------------------

## 5.9 Byte / Buffer 与 Encoding

**优先级：P5；属于 Network 和二进制文件 I/O 的前置项目。**

**目标：** 在实现 Base64、Hex、二进制文件和 Socket API
之前，确定安全、明确的字节表示。

### 必须先回答的问题

1.  OMBasic 是否引入独立 Byte 类型？
2.  Byte 是否只是受范围约束的 Int，还是专用运行时表示？
3.  Buffer 是不可变值还是可变对象？
4.  Buffer 的 GC 和所有权规则是什么？
5.  Native API 如何 Borrow / Own Buffer？
6.  Buffer 是否可跨 Actor Message 发送？
7.  分布式 Actor 序列化如何处理 Buffer？
8.  超大 Buffer 的长度和资源限制是什么？

### 设计原则

-   String 只表示文本，不表示任意二进制。
-   不得用带 NUL 的 String 模拟字节数组。
-   不得把 C 指针直接暴露给普通 BASIC 代码。
-   优先考虑不可变、可安全传递的公共值模型。
-   如果需要可变 Buffer，必须有显式所有权和 Actor 隔离规则。
-   Buffer 的分配、复制、切片和释放成本必须可测量。

### Encoding API 候选

-   `hex_encode` / `hex_decode`
-   `base64_encode` / `base64_decode`
-   UTF-8 encode / decode（若 Byte 模型需要）
-   明确的错误偏移和非法输入策略

Base64 / Hex 应在 Byte / Buffer 表示确定后实施，不应提前定义一套临时字节
API。

------------------------------------------------------------------------

## 5.10 Network v1

**优先级：P6。**

**目标：** 提供面向应用程序的网络能力，同时保持 Actor 为唯一异步抽象。

### 模块范围

第一阶段：

-   TCP Listener；
-   TCP Connection；
-   TCP Send；
-   TCP Receive；
-   连接关闭；
-   连接错误；
-   基本超时策略。

本轮补齐：

-   connected UDP Socket；
-   由操作系统解析器执行的 DNS Resolution。

TLS 和更高层协议保留为后续独立设计，不属于 Network v1 的 TCP / UDP
Actor 能力验收范围。

### Actor 模型

v1 使用预定义 `Network` System Actor 管理本地 TCP / UDP 资源；Listener /
Connection / Datagram Socket 由不透明 Int ID 标识，操作和结果仍通过 Actor Message 传递。正式协议为
`Tcp_Listen`、`Tcp_Connect`、`Tcp_Accept`、`Tcp_Send`、`Tcp_Receive`、
`Tcp_Close`、`Udp_Connect`、`Udp_Send`、`Udp_Receive`、`Udp_Close` 及对应
完成 Message，详见 ADR-0037。Buffer 在公共 Facade 处转换为字节 List，
Native socket 值不会进入 BASIC。

### 关键语义

-   TCP 是字节流，不是消息边界协议。
-   UDP 发送和接收保留单个数据报边界；超出接收缓冲上限的数据报返回稳定错误。
-   必须明确发送完成代表"数据已交给本地传输层"还是"对端已确认收到"；两者不能混淆。
-   必须定义连接关闭、半关闭、远端断开和本地 Runtime Shutdown。
-   必须定义缓冲区上限和背压策略。
-   必须避免无界接收队列。
-   必须定义多个 Actor 同时发送时的顺序。
-   超时通过 Timer Actor 或明确的服务协议完成。
-   DNS、连接建立和读写不得通过 Callback 暴露给用户。
-   Runtime 内部可以使用平台 I/O Completion、epoll、kqueue、IOCP 或
    Worker Pool，但不能改变用户 API 模型。

### 与 Distribution 的边界

现有分布式 Actor 传输和未来 Network 标准库必须保持职责分离：

-   Distribution 是 Actor 系统内部的节点通信能力；
-   Network 是应用程序可以显式使用的 TCP / UDP 等网络服务；
-   不得把内部 Distribution 帧协议直接当成公共 Socket API；
-   不得因 Network 模块引入第二套 Actor 远程调用语义。

### 验收

本机回环、连接拒绝、远端关闭、半关闭、慢速消费者、发送队列上限、超时、Runtime
Shutdown、并发连接、恶意输入和跨平台测试。

------------------------------------------------------------------------

## 5.11 Logging v1

**优先级：P7。**

**目标：** 提供统一的结构化日志模型。

### API 分层

同步纯函数：

-   日志级别名称；
-   事件格式化；
-   字段规范化；
-   纯文本渲染。

异步输出：

-   `Logger` Actor；
-   `Logger_Write(text, reply)` Message；
-   `Logger_Write_Completed(Result<Unit, String>)`；
-   Console / File Sink 由 Actor 服务管理。

### 设计要求

-   明确 Debug、Info、Warning、Error、Critical 等级。
-   日志事件应支持时间、级别、组件、消息和结构化字段。
-   不得默认把敏感数据写入日志。
-   必须定义日志队列满、Sink 失败和 Runtime Shutdown 时的行为。
-   Fire-and-forget 与带确认写入必须明确区分。
-   不得承诺未实现的持久化或 exactly-once 语义。
-   不应在每次日志调用时隐式创建 Actor。

### 验收

日志级别过滤、结构化字段、并发写入顺序、Sink
失败、队列容量、关闭刷新和敏感字段策略测试。

------------------------------------------------------------------------

## 5.12 Random v1

**优先级：P7。**

**目标：** 区分可复现伪随机数和系统安全随机数。

### 可复现 PRNG

如果算法状态显式传入并返回新状态，可用纯 Function 实现：

``` text
next(state) -> (new_state, value)
```

这类 API 具有可复现性，适用于模拟、测试和非安全随机需求。

### 系统安全随机源

读取操作系统熵源属于外部系统能力。若设计为异步服务，应使用 Random Actor
请求和完成消息；不得通过隐藏全局状态的纯函数伪装为确定性计算。

### 设计要求

-   明确算法和版本；
-   明确种子格式；
-   明确跨平台复现保证；
-   密码学安全随机数与普通 PRNG 分离；
-   不得把普通 PRNG 宣称为密码学安全；
-   不得在 Actor 之间共享隐式可变随机状态。

------------------------------------------------------------------------

## 5.13 Process v1

**优先级：P8。**

**目标：** 通过 Actor 生命周期管理外部进程。

### API 候选

-   启动进程；
-   传递参数和环境变量；
-   获取标准输出；
-   获取标准错误；
-   发送标准输入；
-   请求终止；
-   接收退出码；
-   处理启动失败。

### Actor 模型

Process Actor
表示一个外部进程或进程会话。进程启动、输出到达、退出和终止结果都通过
Message 通知调用方。

### 安全要求

-   不得默认通过 Shell 拼接命令字符串。
-   参数必须保持独立边界，避免命令注入。
-   必须定义环境变量继承策略。
-   必须定义工作目录和权限语义。
-   必须定义子进程退出、父 Actor 退出和 Runtime Shutdown 的行为。
-   必须限制 stdout / stderr 缓冲区。
-   不得把线程、Coroutine 或 Callback 暴露为进程异步 API。

------------------------------------------------------------------------

## 6. 模块依赖与实施顺序

建议依赖关系：

``` text
Prelude / Language Core
        |
        +---- Math
        |
        +---- Text ---- Path
        |       |
        |       +------ JSON
        |
        +---- Collections
        |
        +---- Actor Runtime
                 |
                 +---- Console
                 |
                 +---- File System
                 |
                 +---- Timer / Clock
                 |
                 +---- Logging
                 |
                 +---- Process

Byte / Buffer
      |
      +---- Encoding
      |
      +---- File System Binary I/O
      |
      +---- Network
```

注意：

-   JSON 的第一版不应依赖 Byte / Buffer 才能工作。
-   File System 的文本能力可以先完善；二进制能力等待 Buffer。
-   Network 的正式 API 必须等待 Byte / Buffer 和 Actor 服务容量策略。
-   Logging 可以先依赖 Console Actor；文件 Sink 等 File System
    接口稳定后再增加。
-   Timer 现有能力可以继续使用，不必等待完整 DateTime。
-   Distribution 不应成为普通标准库模块的隐式依赖。

------------------------------------------------------------------------

## 7. 分阶段实施路线

## Phase 0 --- 架构规则与现状冻结

**目标：** 防止标准库扩张过程中出现第二套异步模型。

任务：

-   [x] 将 Actor-only Async 约束写入语言标准或独立规范章节。
-   [x] 定义同步 Function、同步 Action、Actor 异步服务的 API 分类。
-   [x] 定义系统服务 Request / Completion Message 规范。
-   [x] 盘点现有 Prelude、Math、Text、Path、JSON、Console、File
    System、Timer API。
-   [x] 检查每个模块的公开符号和私有实现。
-   [x] 在 `docs/development/stdlib_api_inventory.md` 固定公开与私有面清单。
-   [x] 定义错误码、错误 Record 和 Runtime Fault 的边界。
-   [x] 定义 Blocking Pool 只能作为 Runtime 内部实现的规则。
-   [x] 建立标准库 API Review Checklist。
-   [x] 为必要的语义变化创建 ADR。

**完成标准：** 没有未分类的公共 API；所有异步系统服务都能说明其 Actor
协议。

## Phase 1 --- JSON v1 完善

任务：

-   [x] 补充 String / Bool / Number Text Accessors。
-   [x] 补充 Null 判断。
-   [x] 补充对象字段存在性和对象长度。
-   [x] 补充有序对象键和 Entry 查询。
-   [x] 明确 Int64 / Float64 数字转换语义。
-   [x] 固定 Parse_Error 稳定错误码。
-   [x] 补齐构造、解析和序列化边界测试。
-   [x] 更新 `docs/stdlib/json.md`。
-   [x] 更新 Conformance 与 SDK relocation 测试。

**完成标准：** 用户可以完成常见 JSON 配置读取和构造，不需要直接访问
`Value.encoded`。

## Phase 2 --- Collections v1

任务：

-   [x] 明确 List / Map 的不可变语义。
-   [x] 明确缺失元素、越界和缺失 Key 的错误行为。
-   [x] 实现基础 List API（length、empty、contains、index、concat、reverse）。
-   [x] 实现当前类型系统可稳定导出的 String-key Map 基础 API。
-   [x] 验证高阶函数所需的 Function 值和泛型特化。
-   [x] 明确闭包不在第一版范围内，待函数值与泛型特化稳定后再评估。
-   [x] 验证 Map 插入顺序。
-   [x] 测试 GC、嵌套集合和大集合。
-   [x] 更新 `docs/stdlib/collections.md`。

**完成标准：** 集合操作不需要修改
Runtime；高阶函数只在语言能力和测试充分时进入稳定 API。

## Phase 3 --- File System v1

任务：

-   [x] 冻结当前文本读取和覆盖写入契约。
-   [x] 设计稳定 File_Error。
-   [x] 设计 Append、Exists、Delete、Directory Create、Directory
    List、Stat Message。
-   [x] 明确相对路径基准。
-   [x] 明确符号链接和路径安全策略。
-   [x] 明确目录顺序和资源上限。
-   [x] 定义服务关闭和请求失败行为。
-   [x] 增加 Linux / Windows Conformance。
-   [x] 更新 `docs/stdlib/file_system.md`。

**完成标准：** 文本文件和常见目录操作可通过统一 File System Actor
完成，且无同步隐藏包装。

## Phase 4 --- Time v1

任务：

-   [x] 定义 Duration。
-   [x] 明确 Monotonic Instant 语义。
-   [x] 定义 UTC DateTime 和时间戳转换。
-   [x] 设计 Clock Actor。
-   [x] 保留 Timer Actor 现有投递模型。
-   [x] 评估 Timer cancellation 协议。
-   [x] 定义 deadline overflow 和 Runtime Shutdown 语义。
-   [x] 更新 `docs/stdlib/time.md`。
-   [x] 实现纯 Instant 比较和毫秒差值 API。
-   [x] 实现 UTC Date_Time 与 Unix 毫秒互转及边界测试。
-   [x] 实现 Clock Actor 请求、完成消息和 Linux / Windows Runtime。

**完成标准：** Duration、Instant、DateTime 和 Timer
的职责分离，不存在阻塞 sleep 或隐藏等待。

## Phase 5 --- Console v1

任务：

-   [x] 审查现有 `print` Prelude 语义。
-   [x] 冻结 Console Write 协议。
-   [x] 定义并发输出顺序。
-   [x] 设计 Console Read Line Actor 协议。
-   [x] 定义 EOF、输入错误和关闭语义。
-   [x] 更新 Console 示例和测试。
-   [x] 实现可取消的 Read Line Runtime 与 Linux / Windows 测试。

**完成标准：** 控制台输入输出均有清晰契约；异步操作通过 Actor 暴露。

## Phase 6 --- Byte / Buffer 与 Encoding

任务：

-   [x] 编写 Byte / Buffer ADR。
-   [x] 决定字节值表示。
-   [x] 决定 Buffer 不可变性和 GC 所有权。
-   [x] 定义 Native Borrow / Owned ABI。
-   [x] 定义跨 Actor 传递和 Distribution 序列化。
-   [x] 实现 Hex。
-   [x] 实现 Base64。
-   [x] 增加二进制边界测试。

**完成标准：** 不再使用 String 模拟任意二进制数据；Byte / Buffer
可以安全地用于文件和网络 API。

## Phase 7 --- Network v1

任务：

-   [x] 编写并完成 Network Actor 架构 ADR。
-   [x] 定义 TCP Listener / Connection Actor。
-   [x] 定义 Send / Receive / Close Message。
-   [x] 定义 Byte / Buffer 传输协议。
-   [x] 定义连接错误和关闭语义。
-   [x] 定义缓冲区容量和背压。
-   [x] 定义超时与 Timer Actor 的组合方式。
-   [x] 实现跨平台 TCP Listen / Connect / Accept / Send / Receive / Close。
-   [x] 添加 TCP loopback conformance 场景。
-   [x] 在 Windows / Linux Release 上运行 TCP loopback conformance。
-   [x] 实现跨平台 connected UDP Bind / Connect / Send / Receive / Close。
-   [x] 添加 UDP loopback conformance 场景并在 Windows / Linux Release 上运行。
-   [x] 使用操作系统解析器解析 TCP / UDP 远端地址。
-   [x] 明确 Network 与 Distribution 的边界。

**完成标准：** 用户可以通过 Actor 实现 TCP 服务端和客户端，不需要
Future、Callback 或用户级事件循环。

## Phase 8 --- Logging 与 Random

任务：

-   [x] 定义结构化 Log Event。
-   [x] 实现纯格式化辅助。
-   [x] 设计 Logger Actor。
-   [x] 实现 Logger 写入和 Flush Runtime 协议。
-   [x] 定义日志队列、Sink 错误和 Shutdown 语义。
-   [x] 增加 Logger FIFO / Flush conformance 测试。
-   [x] 定义 PRNG 算法和显式状态 API。
-   [x] 设计系统安全随机源 Actor（如需要）。
-   [x] 增加复现性和安全边界测试。

**完成标准：** 日志输出和系统随机源均具有明确的状态与 Actor 边界。

## Phase 9 --- Process v1

任务：

-   [x] 定义 Process Actor 生命周期。
-   [x] 定义启动参数、环境变量和工作目录。
-   [x] 定义 stdin / stdout / stderr Message。
-   [x] 定义退出码和异常终止。
-   [x] 定义缓冲区和资源限制。
-   [x] 定义 Runtime Shutdown 时的子进程策略。
-   [x] 增加命令注入和资源耗尽测试。

**完成标准：** 外部进程作为 Actor 管理的系统资源，不暴露第二套异步模型。

------------------------------------------------------------------------

## 8. 测试策略与质量门槛

每个标准库模块必须同时具备源代码测试、公共 API 测试和必要的 Runtime /
Native 测试。

### 8.1 测试分层

  层级           覆盖范围
  -------------- -----------------------------------------
  BASIC Unit     纯函数、边界和错误分支
  Conformance    公共 Facade、导入、类型和语言语义
  Runtime Unit   Actor 服务、资源生命周期、错误映射
  Native Unit    C 实现、ABI、所有权、平台调用
  Integration    多 Actor、多服务和真实系统资源
  SDK Install    安装、重定位、资源发现和 manifest
  Fuzz           Parser、Decoder、路径、网络帧和外部输入
  Stress         高并发、队列上限、GC 和资源耗尽

### 8.2 Sanitizer

Sanitizer 是开发与 CI 质量门槛，不进入正式 Release SDK：

-   ASan + UBSan：完整相关测试，重点覆盖 Parser、GC、Native
    ABI、文件与网络边界。
-   TSan：Actor Scheduler、Mailbox、Blocking Pool、Timer、Foreign-thread
    Callback 和并发系统服务。
-   Fuzzing：外部输入解析器、JSON、编码、路径、网络帧和序列化。
-   Release：正常优化构建，`OM_SANITIZER=none`。

### 8.3 异步 Actor 测试要求

不得依赖任意 `sleep` 或固定延时来推断异步操作完成。

测试应优先使用：

-   明确的 Completion Message；
-   测试 Actor；
-   Mailbox 顺序断言；
-   Timer Actor 的可控场景；
-   Runtime 提供的测试钩子（仅限测试构建，且不得成为公共语言 API）。

每个异步服务至少覆盖：

-   正常完成；
-   输入无效；
-   系统资源失败；
-   多个并发请求；
-   Reply Actor 终止；
-   服务停止；
-   队列容量耗尽；
-   GC 压力；
-   Runtime Shutdown。

### 8.4 跨平台门槛

Linux 和 Windows 都必须覆盖公共标准库 API。平台不支持的功能必须明确返回
Unsupported，而不是静默退化成不同语义。

------------------------------------------------------------------------

## 9. 每个模块的 Definition of Done

一个模块只有满足以下条件，才可以标记为稳定：

-   [ ] 有明确的模块目标和非目标。
-   [ ] 有稳定公共 Facade。
-   [ ] 所有导出符号都有文档。
-   [ ] 同步 / Actor 异步分类明确。
-   [ ] 没有 Future、Promise、Callback 或隐藏等待。
-   [ ] 错误类型和错误码稳定。
-   [ ] 资源所有权和生命周期明确。
-   [ ] 公开 API 的边界行为有测试。
-   [ ] 失败分支有测试。
-   [ ] Linux / Windows 测试通过。
-   [ ] ASan / UBSan 相关测试通过。
-   [ ] TSan 相关并发测试通过。
-   [ ] Fuzz 目标已覆盖外部输入解析器（如适用）。
-   [ ] SDK relocation 测试通过。
-   [ ] `stdlib-manifest.json` 与内容摘要正确。
-   [ ] 文档示例可以实际编译运行。
-   [ ] API 变更有版本说明和必要 ADR。

------------------------------------------------------------------------

## 10. SDK 发布与兼容性

### 10.1 标准库随 SDK 发布

标准库源文件继续随 SDK 安装在：

``` text
share/ombasic/stdlib/
```

编译器只通过显式 `@std/` 导入标准库。标准库版本和文件 SHA-256
摘要必须写入 SDK manifest。

### 10.2 API 稳定策略

-   未发布的内部模块可以调整。
-   已公开 Facade 的符号、类型和错误语义必须视为兼容性接口。
-   破坏性变更必须明确版本化。
-   不得只改 `.bas` 实现而忘记更新文档、Conformance 和 manifest。
-   SDK relocation 后所有标准库导入必须继续工作。
-   不得在 SDK 中留下开发机绝对路径。

### 10.3 Release 内容

正式 Release SDK 包含：

-   Release 编译器；
-   Release Runtime；
-   标准库 `.bas` 源码；
-   必要的 Native 库；
-   SDK manifest；
-   工具和文档。

不包含 Sanitizer 构建产物作为默认运行依赖。Sanitizer 专用构建只用于 CI
和开发诊断。

------------------------------------------------------------------------

## 11. 明确不做的事情

在标准库 v1 阶段，以下事项不进入默认范围：

1.  Async / Await；
2.  Future / Promise；
3.  Coroutine / Task；
4.  Callback-based Async；
5.  用户级事件循环；
6.  隐式 continuation；
7.  隐藏阻塞的"同步外观异步"API；
8.  普通 Function 中执行 I/O；
9.  隐式共享可变状态；
10. 为单个标准库模块增加语言级特殊语法；
11. 为 JSON 单独增加递归类型系统；
12. 用 String 承载任意二进制数据；
13. 将 Distribution 内部协议直接公开为 Network API；
14. 过早引入完整时区数据库、复杂国际化或大型第三方运行时依赖；
15. 在标准库中加入包管理器、依赖求解器或隐式导入机制。

这些功能并非永远不能讨论；它们必须通过独立设计评审，且不能破坏 OMBasic
的核心模型。

------------------------------------------------------------------------

## 12. 需要单独形成 ADR 的问题

在进入对应阶段前，至少需要形成以下 ADR：

  ------------------------------------------------------------------------------------------------
  ADR 主题                必须回答的问题                                   阶段
  ----------------------- ------------------------------------------------ -----------------------
  Actor-only Async Policy Actor                                            Phase 0
                          是唯一用户级异步模型；系统服务协议的规范格式

  Standard Error Model    稳定错误码、错误 Record、平台错误映射            Phase 0

  Collections Semantics   不可变操作、边界错误、Map 顺序、高阶函数限制     Phase 2

  File System Error and   File_Error、相对路径、符号链接和目录限制         Phase 3
  Path Policy

  Clock and Timer Model   Clock Actor、Instant、DateTime、Timer            Phase 4
                          Cancellation

  Byte and Buffer Model   类型表示、GC、所有权、Actor 传递和序列化         Phase 6

  Network Actor Model     Listener、Connection、流语义、背压、关闭和错误   Phase 7

  Logging Delivery        日志确认、队列容量、Sink 失败和关闭刷新          Phase 8
  Semantics

  Random Model            PRNG 复现性和系统安全随机源                      Phase 8

  Process Actor Model     参数、环境、输出缓冲、退出和关闭策略             Phase 9
  ------------------------------------------------------------------------------------------------

------------------------------------------------------------------------

## 13. 推荐的首批 Pull Request 划分

为了降低单个 PR 的风险，建议按以下顺序拆分：

### PR 1 --- Stdlib Architecture Rules

-   标准库总览文档；
-   Actor-only Async Policy；
-   API 分类表；
-   API Review Checklist；
-   不修改 Runtime ABI。

### PR 2 --- JSON Accessors

-   JSON String / Bool / Number Text Accessors；
-   Null 与 Object 查询；
-   JSON 文档；
-   BASIC Conformance。

### PR 3 --- JSON Numeric Conversion

-   Int64 / Float64 显式转换；
-   精度与范围错误；
-   大数字和边界测试。

### PR 4 --- Collections Basic

-   List / Map 基础纯函数；
-   不可变语义；
-   Map 顺序；
-   边界和 GC 测试。

### PR 5 --- Collections Higher-Order

-   `map` / `filter` / `fold`；
-   函数值与泛型验证；
-   仅在语言能力已经支持时合入。

### PR 6 --- File System Contract

-   File_Error ADR；
-   Append / Exists / Delete / Directory / Stat 协议；
-   错误和关闭语义；
-   不一次性引入二进制 I/O。

### PR 7 --- File System Implementation

-   Runtime Actor 服务；
-   Worker Pool 内部执行；
-   Linux / Windows 测试；
-   SDK 示例和文档。

### PR 8 --- Time Contract

-   Duration；
-   Instant / DateTime 边界；
-   Clock Actor；
-   Timer 语义和取消评估。

### PR 9 --- Byte / Buffer ADR

-   类型和内存模型；
-   GC；
-   Native ABI；
-   Actor 传递和 Distribution 影响。

### 后续 PR

依次推进 Encoding、Network、Logging、Random 和 Process。

每个 PR 必须尽量保持单一主题，不要把语言语法改动、Runtime 重构和标准库
API 扩展混成一个不可审查的大 PR。

------------------------------------------------------------------------

## 14. 最终验收目标

OMBasic 标准库 v1 达成以下状态时，可以认为第一轮建设完成：

### 语言与架构

-   [x] Actor 是唯一用户级异步模型。
-   [x] 不存在 Async / Await、Future、Promise、Coroutine、Callback 异步
    API。
-   [x] Function 保持纯计算语义。
-   [x] Actor 间通过 Message 通信。
-   [x] Runtime 内部 Blocking Pool 不暴露为用户级抽象。

### 核心标准库

-   [x] Math 的整数和浮点 API 稳定。
-   [x] Text 的 Unicode 语义明确。
-   [x] Path 只处理路径字符串，不伪装文件系统操作。
-   [x] JSON 可以完整解析、查询、构造和序列化。
-   [x] Collections 具备稳定的 List / Map API。
-   [x] File System 具备常用文本和目录操作。
-   [x] Time 清晰区分 Duration、Instant、DateTime 和 Timer。
-   [x] Console 具有稳定的 Actor 输入输出协议。

### 系统标准库

-   [x] Byte / Buffer 的内存和所有权模型稳定。
-   [x] Encoding 可安全处理二进制数据。
-   [x] Network 通过 Actor 提供 TCP / UDP 能力。
-   [x] Logging 具有明确的投递和失败语义。
-   [x] Random 区分普通 PRNG 与安全随机源。
-   [x] Process 通过 Actor 管理外部进程。

### 工程质量

-   [x] Linux / Windows Release CTest 通过。
-   [x] ASan / UBSan 全套 CTest 通过。
-   [x] TSan Actor 并发压力测试通过。
-   [x] Fuzz 测试覆盖外部输入边界并通过 smoke runs。
-   [x] Conformance、Runtime、Native 和 Integration 测试通过。
-   [x] SDK 安装与 relocation 测试通过。
-   [x] 标准库导入和 API 文档示例通过 conformance。
-   [x] SDK 标准库 manifest 和内容摘要通过 relocation 校验。

------------------------------------------------------------------------

## 15. 最终建议

OMBasic 标准库的竞争力不应该来自 API
数量，而应该来自一致的语义、可预测的行为和清晰的系统边界。

最重要的设计原则是：

> **同步计算保持简单；副作用必须显式；异步操作只能通过
> Actor；消息是异步交互的唯一用户级载体。**

因此，当前最合理的开发策略是：

1.  先完成 JSON v1；
2.  再完成 Collections v1；
3.  然后完善 File System 和 Time；
4.  接着确定 Byte / Buffer；
5.  最后建立 Network、Logging、Random 和 Process 等系统服务。

每个阶段都先冻结 API 契约，再实现 `.bas` Facade、必要的 Native / Runtime
支持、测试和文档。不要为了追求标准库功能数量而牺牲 OMBasic 已经确立的
Actor 模型。

这份计划应当作为标准库后续 Issue、PR、ADR 和 Release Gate
的共同依据；当实现与计划不一致时，必须通过更新计划或 ADR
明确记录，而不是让代码和文档长期分叉。

------------------------------------------------------------------------

## 参考资料

以下为本计划使用的仓库基线文档：

-   [OMBasic Language
    Standard](https://github.com/risol/OMBasic/blob/master/OMBasic-Language-Standard.md)
-   [Prelude and Standard Library Development
    Plan](https://github.com/risol/OMBasic/blob/master/OMBasic-Prelude-and-Stdlib-Development-Plan.md)
-   [JSON Standard
    Library](https://github.com/risol/OMBasic/blob/master/docs/stdlib/json.md)
-   [File System Runtime
    Service](https://github.com/risol/OMBasic/blob/master/docs/stdlib/file_system.md)
-   [Timer
    Contract](https://github.com/risol/OMBasic/blob/master/docs/stdlib/time.md)
-   [Console](https://github.com/risol/OMBasic/blob/master/docs/stdlib/console.md)
-   [Module Facades and Explicit Re-exports ---
    ADR-0026](https://github.com/risol/OMBasic/blob/master/docs/architecture/adr/ADR-0026-module-facades-and-explicit-reexports.md)
-   [JSON Value Model ---
    ADR-0029](https://github.com/risol/OMBasic/blob/master/docs/architecture/adr/ADR-0029-json-value-model.md)
-   [SDK Resource
    Layout](https://github.com/risol/OMBasic/blob/master/docs/compiler/resource_layout.md)
-   [Testing
    Strategy](https://github.com/risol/OMBasic/blob/master/docs/development/testing_strategy.md)
