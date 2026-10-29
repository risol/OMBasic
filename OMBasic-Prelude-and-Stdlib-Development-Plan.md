# OMBasic Prelude 与 `@std` 标准库完整开发计划

- **项目：** OMBasic
- **计划日期：** 2026-10-29
- **源码基线：** `risol/OMBasic` 默认分支 `master`
- **基线提交：** [`897db21b496ff87d3c3052a27847a771befad2f5`](https://github.com/risol/OMBasic/commit/897db21b496ff87d3c3052a27847a771befad2f5)
- **基线提交信息：** `feat: add module facades and explicit re-exports`
- **状态：** Implementation and required local gates complete — Windows 与 Linux Release、ASan/UBSan 全量 CTest 通过，libFuzzer smoke 通过；Docker Desktop 的 TSan 运行受宿主内存布局限制，见实施进度
- **目标：** 先收敛语言内建 Prelude，再引入显式 `@std` 标准库导入根，并建立可随 SDK 分发、可审查、可测试的标准库实现体系。

---

## 1. 执行摘要

本计划把 OMBasic 的语言内建与标准库划分为三个边界：

1. **Prelude：** 编译器自动提供的最小语言与 Runtime 契约。用户不需要 `IMPORT`。
2. **`@std`：** 通过明确的 `IMPORT` 使用的标准库源码。源码由 OMBasic SDK 提供，参与正常的解析、类型推导、语义分析、Native AOT 编译和链接。
3. **Runtime：** 为内存管理、Actor 调度、系统 I/O 等提供底层实现。Runtime 实现不意味着对应的全部公共 API 都必须成为编译器内建。

核心方向：

> **Prelude 尽量小；标准库显式导入；普通能力优先用 `.bas` 实现；真正需要底层支持的能力才进入 Runtime 或 Native 边界。**

推荐的使用形式：

```basic
IMPORT "@std/math/math.bas"

ACTION main()
    value = math.abs(-42)
    print(value)
END ACTION
```

这里：

- `@std/math/math.bas` 是 SDK 标准库中的显式源码路径；
- `math` 是由 Facade 文件名 `math.bas` 决定的模块限定名；
- `math.abs` 是普通模块公开的 API；
- `print` 在本计划第一阶段继续保留为当前语言提供的输出原语，不在引入 `@std` 时悄悄改变它的语义。

本计划**不引入** Package、包管理器、依赖注册表、版本求解器、`package.toml`、项目级 lockfile、隐式模块搜索路径或预编译 OMBasic 二进制模块格式。

### 1.1 必须先解决的现有规范问题

当前 `OMBasic-Language-Standard.md` 存在一个与本项目直接相关的矛盾：

- OM-21.02 / OM-21.04 的旧文字仍然要求所有 Import 都相对于项目 Source Root，并禁止 `./`；
- OM-21.14 已允许 `./` 相对于当前 Module 目录解析。

必须先统一这两处规范，不能在矛盾的路径语义上继续叠加 `@std`。

### 1.2 推荐实施顺序

1. 盘点并分类当前 Prelude 与编译器内建。
2. 修订 ADR 和语言标准，确定 `@std` 的规范语义。
3. 建立一致的 Prelude 注册表与测试。
4. 扩展 Import Path Resolver 与 Module Graph，支持 `@std` 根和带根类型的 Module identity。
5. 将标准库源码作为 SDK 资源分发，不要求复制进项目 Source Root。
6. 用真实的 `@std/math` 验证整个路径，再扩展 Text、Console、File System、Time 和 JSON。
7. 完成跨平台、SDK relocation、Conformance、Fuzz 和文档验收。

---

## 2. 当前源码基线审计

以下结论基于仓库 `master` 在 `897db21` 的源码和文档。实施时如果基线提交变化，应重新检查本节。

### 2.1 已有基础设施

| 能力 | 当前状态 | 对本计划的影响 |
|---|---|---|
| Prelude scope | `Om_Scope_Kind` 已包含 `OM_SCOPE_PRELUDE` | 不需要重新发明自动可见作用域 |
| Builtin identity | `Om_Builtin_Id` 已为内建声明分配稳定 ID | 应整理和收敛，而不是继续散落增加名称特判 |
| Prelude 扩展入口 | `Om_Resolver_Config` 有 `prelude_names` 和 `native_modules` | 需要明确它们的用途，避免标准库函数全部被全局注入 |
| `print` | 使用 `OM_BUILTIN_PRINT`，类型推导和 lowering 存在专门处理 | `print` 不能只通过增加一个 `.bas` 文件就被替换 |
| 系统 Actor / Message | Console、Timer、File_System 及相关 Message 已有内建身份 | 必须把语言内建、Runtime 服务协议和标准库便利 API 分开 |
| Module Facade | `X/X.bas` 约定和 `EXPORT IMPORT` 已进入实现 | 标准库可以直接采用 Facade 结构 |
| Module-relative Import | `./` 解析能力已经加入 Loader | `@std` 必须复用这套规则并保留根类型 |
| SDK 标准库资源 | SDK 已安装 `share/ombasic/stdlib/math/` | 可扩展现有 SDK 资源布局，不必另建分发系统 |
| SDK relocation 测试 | `tests/tooling/install_sdk_test.py` 已检查 SDK 移动后的标准库文件 | 扩展测试以验证 `@std` 能从移动后的 SDK 解析 |
| Native ABI metadata | `.omi` 已作为独立 Native 接口 metadata 使用 | Native 依赖不能与源码 `IMPORT` 混为一谈 |

相关源码位置：

- `include/ombasic/semantic/name_resolver.h`
- `src/semantic/name_resolver/name_resolver.c`
- `src/semantic/type_inference/type_inference.c`
- `src/ir/mir/lower_hir.c`
- `src/module_graph/module_loader.c`
- `include/ombasic/source/import_path.h`
- `src/source/import_path/import_path.c`
- `docs/compiler/resource_layout.md`
- `tests/tooling/install_sdk_test.py`

### 2.2 当前 Prelude / Builtin 清单

当前 `Om_Builtin_Id` 至少包含以下类别：

| 当前内建 | 当前类别 | 本计划的建议处理 |
|---|---|---|
| `Unit` | 核心类型 | 保留为 Prelude |
| `Result` | 核心类型 | 保留为 Prelude |
| `Ok`、`Error` | `Result` 构造器 | 保留为 Prelude |
| `Node_Ref` | Actor / 节点类型 | 保留为 Prelude，记录类型语义 |
| `Actor_Id` | Actor 身份类型 | 保留为 Prelude |
| `Actor_Down_Reason` | Actor 生命周期类型 | 保留为 Prelude |
| `Actor_Down` | Runtime 生命周期 Message | 保留为语言定义的系统 Message |
| `Actor_Down_Reason.normal` | 系统值 | 保留为标准内建成员 |
| `Actor_Down_Reason.failed` | 系统值 | 保留为标准内建成员 |
| `Actor_Down_Reason.node_unreachable` | 系统值 | 保留为标准内建成员 |
| `print` | 输出原语 | 第一阶段保留；记录其类型、格式化、换行和错误语义 |
| `Console` | 系统 Actor | 暂时保留现有协议身份，单独标记为 Runtime Service |
| `Timer` | 系统 Actor | 暂时保留现有协议身份，单独标记为 Runtime Service |
| `File_System` | 系统 Actor | 暂时保留现有协议身份，单独标记为 Runtime Service |
| `Console_Write`、`Console_Write_Completed` | 系统 Message | 保留现有 Runtime 协议；为未来标准库封装提供基础 |
| `File_Read`、`File_Read_Completed` | 系统 Message | 保留现有 Runtime 协议 |
| `File_Write`、`File_Write_Completed` | 系统 Message | 保留现有 Runtime 协议 |
| `Timer_After` | 系统 Message | 保留现有 Runtime 协议 |

`SELF`、`NULL`、`TRUE`、`FALSE`、`SEND`、`SPAWN`、`WATCH`、`STOP` 等属于关键字、字面量或语法结构，不应仅因为用户经常使用就归类成普通 Prelude 函数。

### 2.3 当前标准库太小，不能代表稳定 API

目前 `stdlib/math/math.bas` 只是：

```basic
EXPORT IMPORT "./integer.bas"
```

而 `stdlib/math/integer.bas` 只提供一个简单的 `add` 示例。

这足以验证 Facade 和 Re-export，但不能算完成了可供应用使用的 Math 库。计划应把它当作基础 fixture，在标准 API 经过设计和测试后再替换或扩充。

### 2.4 当前 SDK 资源布局

`docs/compiler/resource_layout.md` 已规定：

```text
<sdk>/
├── bin/
├── include/ombasic/
├── lib/ombasic/<target-triple>/
└── share/ombasic/
    ├── target-manifest.json
    └── stdlib/
        └── math/
            ├── math.bas
            └── integer.bas
```

当前标准库要求复制或 vendoring 到项目 Source Root 后再通过普通路径导入。本计划会将它改成：**标准库可以直接从所选 SDK 的固定 `share/ombasic/stdlib/` 根显式导入**，不需要复制源码，也不因此引入依赖管理器。

### 2.5 当前相关 ADR

- [ADR-0020 — Strict Source Tree Paths](https://github.com/risol/OMBasic/blob/master/docs/architecture/adr/ADR-0020-strict-source-tree-paths.md)
- [ADR-0025 — Source and Native Library Distribution](https://github.com/risol/OMBasic/blob/master/docs/architecture/adr/ADR-0025-source-and-native-library-distribution.md)
- [ADR-0026 — Module Facades and Explicit Re-exports](https://github.com/risol/OMBasic/blob/master/docs/architecture/adr/ADR-0026-module-facades-and-explicit-reexports.md)

ADR-0026 当前明确拒绝 `@std` 根，并要求把标准库复制或 vendoring 到项目 Source Root。本计划将正式修订这项决定，而不是让新规则与 Accepted ADR 并存。

---

## 3. 目标与非目标

### 3.1 目标

- [x] 建立可审计、可测试、唯一来源的 Prelude / Builtin 清单。
- [x] 明确哪些名称是语言语义原语、哪些是 Runtime Service、哪些是普通标准库 API。
- [x] 让 `IMPORT "@std/..."` 解析到当前 SDK 自带的标准库源码。
- [x] 让标准库源码以普通 `.bas` 形式参与现有 Module Graph 和 Native AOT 编译。
- [x] 让标准库内部的 `./` Import 在 SDK 安装位置变化后仍然有效。
- [x] 为 Project Source Root 与 SDK Standard Library Root 建立不混淆的 Module identity。
- [x] 保留 Source Root 和 SDK Root 各自的路径安全检查、精确大小写、无 symlink traversal 等约束。
- [x] 让 `check` 可以分析引用 `@std` 的程序，而不要求链接 Runtime archive。
- [x] 让 `build` / `run` 使用与 `check` 一致的标准库解析语义。
- [x] 扩展 SDK manifest、SDK relocation 测试、Conformance、Fuzz 和文档。
- [x] 用少量有实际用途的库模块验证机制，再逐步扩展标准库。

### 3.2 非目标

本计划不实现：

- Package / Project / Namespace 等源码级新概念；
- 包管理器、注册表、依赖求解器或 lockfile；
- 编译器自动下载、更新或联网获取标准库；
- 任意 `@foo` 根或用户可配置的无限 Import roots；
- 隐式 Import、自动加载标准库所有模块或把标准库函数全部注入 Prelude；
- `.omlib` 等预编译 OMBasic 二进制模块；
- VM 或字节码后端；
- C 源码中间输出；
- 一次性实现庞大、未经测试的标准库 API；
- 在本计划中直接删除 `print` 或改变它的运行语义；
- 将 Native `.omi` metadata 当作 OMBasic 源码 Module。

### 实施进度（2026-10-29）

- 已完成：Prelude builtin/member registry、`@std` Root-aware Import、SDK stdlib manifest 与 relocation、Math v1，以及系统 Actor 的 Console/Timer/File_System 运行时 Conformance。
- 已完成：Text 当前能力审计和纯源码 v1（`concat`、`equals`、`is_empty`）；补齐 `+ String`、Unicode scalar 顺序比较及其 Runtime / LLVM 路径；LSP 支持 `@std` member 的 completion、hover 和 definition。
- 已完成：模块级 `NATIVE IMPORT` 语法、root-aware `.omi` 加载、模块作用域符号可见性、SDK Native metadata 路径，以及不泄漏到其他模块的正反向 Conformance。
- 已完成：Text v1 scalar length、prefix/suffix、find、slice、split、join，采用显式的 NUL rejection 契约；以不可变 List 拼接支持纯源码列表构造；Path v1 逻辑路径字符串 API 和 Conformance。
- 已补充验证：Timer 消息与 Actor mailbox 排序；File_System 缺失文件、非法 UTF-8 读取和目录目标写入的错误分支；Console 的 write/write-line payload 与 completion 行为。
- JSON 0.1 Value 表示、Parser / Value / Stringify facade、C Native parser 与公开 conformance 已实现；SDK relocation 已覆盖 JSON。Linux 专属 File_System 受保护目标/存储错误 fixture 已通过 Linux Release 与 ASan/UBSan CTest。
- 验收记录：Windows Release CTest 38/38；Linux LLVM 23.1.4 Release CTest 38/38；Linux ASan/UBSan CTest 38/38；9 个 libFuzzer smoke target 各运行 200 次，LLVM backend target 运行 20 次。`architecture_check`、`spec_check` 与 Conformance manifest 校验通过。
- TSan target 已成功构建；Docker Desktop 的 Linux 容器运行时因 incompatible memory layout 退出，宿主禁止 `setarch -R`，因此 TSan 需在支持的 Linux runner 上复验。

---

## 4. 核心架构决策

以下是本计划建议采用的目标设计。正式实施前应以 ADR 和语言标准固化。

### 4.1 Prelude 与 `@std` 必须是两种不同机制

#### Prelude

- 编译器自动提供。
- 不需要 `IMPORT`。
- 只包含语言语义、必要的类型构造器、生命周期契约和第一阶段暂时保留的运行时原语。
- 名称、类型规则、作用域和 lowering 行为必须可枚举。
- 不应把 `math.abs` 这种普通 API 自动注入所有模块。
- Prelude 名称的重复声明和遮蔽规则必须明确。

#### `@std`

- 必须显式 `IMPORT`。
- 使用普通 Module Alias，例如 `math.abs(...)`。
- 源码文件位于 SDK 的 `share/ombasic/stdlib/`。
- 正常进入 Module Graph、Name Resolver、Type Inference、HIR、MIR 和 Native AOT 编译。
- 不自动执行模块顶层初始化；Import 仍然只有静态依赖语义。
- 标准库版本跟随明确的 SDK 发布版本及其源码 manifest。

**禁止把 `@std` 的函数通过 `prelude_names` 全局注入所有模块。**

### 4.2 `@std` 是唯一特殊标准库根

标准形式：

```basic
IMPORT "@std/math/math.bas"
IMPORT "@std/text/text.bas"
```

规则：

- `@std/` 必须精确匹配；OMBasic 区分大小写。
- `@std` 只在 `IMPORT` 路径字符串中具有特殊意义。
- 只支持标准库根 `@std`，不开放任意 `@foo`。
- 必须写完整 `.bas` 路径，不省略扩展名。
- 仍然使用 `/` 作为源码路径分隔符。
- 不扫描目录，不尝试其他位置，不回退到项目 Source Root。
- `@std` 不代表 Package，也不增加 Package 语义。
- `IMPORT "@std/math/math.bas"` 导入的是普通 Module；`math` 限定名仍由 `math.bas` 的文件名决定。

### 4.3 三种 Import 路径形式

#### A. Project Source Root-relative

```basic
IMPORT "vendor/json/json.bas"
```

解析基准为当前编译目标的 Project Source Root。

它继续用于项目自身代码、vendor 源码和第三方源码树。

#### B. Current-Module-relative

```basic
IMPORT "./parser.bas"
EXPORT IMPORT "./value.bas"
```

解析基准为当前 `.bas` 文件所在目录，并且必须保留当前 Module 的根类型：

- Project Module 的 `./` 只能解析到 Project Source Root；
- `@std` Module 的 `./` 只能解析到 SDK Standard Library Root。

`./` 不能跨根，也不能逃出所属根。

#### C. SDK Standard Library Root

```basic
IMPORT "@std/math/math.bas"
EXPORT IMPORT "@std/text/text.bas"
```

解析基准为所选 SDK 的 `share/ombasic/stdlib/`。

对于标准库内部跨子树依赖，使用显式 `@std/...`；对于同一目录或子目录的内部依赖，使用 `./...`。

### 4.4 Module identity 必须带根类型

当前 Module identity 不能只依赖相对路径字符串，因为项目和 SDK 标准库可能同时存在同名路径：

```text
project Source Root:
    math/math.bas

SDK Standard Library Root:
    math/math.bas
```

它们是两个不同的 Module。

建议把逻辑身份定义为：

```text
Module_Key =
    Root_Kind
    + Canonical_Relative_Path
```

其中 `Root_Kind` 至少包括：

```text
PROJECT_SOURCE
SDK_STDLIB
```

示例：

```text
(PROJECT_SOURCE, "math/math.bas")
(SDK_STDLIB,     "math/math.bas")
```

这两个 Key 不得相等。

实现要求：

- `Om_Module_Id` 仍然可以是 Module Project 内部的紧凑 ID，但 ID 分配前必须按完整 `Module_Key` 去重。
- 绝对文件系统路径不是 Module identity。
- `@std/` 是逻辑根标识，不是文件系统中的目录名。
- 诊断、LSP 和接口 metadata 应显示稳定逻辑路径，例如 `@std/math/math.bas`，而不是把 SDK 安装绝对路径作为用户可见身份。
- Module-relative `./` 只决定解析基准，不进入最终 Module identity。
- 相同根类型和 canonical path 的多次导入必须得到同一个 Module ID。
- 根类型不同的同名路径必须得到不同 Module ID。

### 4.5 标准库与项目源码之间的依赖边界

为避免标准库偷偷依赖使用它的应用，建议采用以下规则：

- 项目 Module 可以 `IMPORT "@std/..."`。
- 标准库 Module 可以 `IMPORT "./..."`，引用同一标准库根中的相邻文件。
- 标准库 Module 可以 `IMPORT "@std/..."`，引用另一个标准库 Module。
- 标准库 Module 不得依赖调用方项目的 Source Root。
- 因此标准库源码中不得使用普通的 Project Source Root-relative Import。
- 标准库不得依赖工作目录、环境变量或 SDK 的安装绝对路径。
- 标准库内部依赖必须在源码中显式可见。

例如：

```basic
' @std/math/math.bas
EXPORT IMPORT "./integer.bas"
```

跨模块依赖：

```basic
' 某个标准库 Module
IMPORT "@std/text/text.bas"
```

### 4.6 路径安全边界

以下约束继续保留：

- 拒绝 `..`、`./../` 和任何 parent traversal；
- 拒绝绝对路径；
- 拒绝反斜杠路径分隔符；
- 拒绝重复 `/` 和空路径段；
- 拒绝不符合 `lower_snake_case` 的路径段；
- 拒绝缺少 `.bas` 的路径；
- 精确匹配文件系统大小写；
- 拒绝 symlink / reparse-point traversal；
- 不以当前工作目录作为隐式解析基准；
- 不允许路径规范化后逃出所属 Root；
- SDK Root 必须先被规范化并固定，再解析标准库文件。

路径解析应先分类 Import root，再进行 canonical path validation；不能把带 `@std/` 前缀的原始字符串直接交给只接受普通相对路径的 validator。

### 4.7 不增加包管理器

版本来源：

- `@std` 的内容由所选 OMBasic SDK 版本固定；
- SDK 的标准库 manifest 记录该版源码清单和内容摘要；
- 构建 provenance 记录 SDK 版本与标准库 manifest 摘要；
- 项目需要固定标准库版本时，固定 OMBasic SDK 的版本；
- 第三方库仍通过 Git commit/tag、submodule、subtree 或 vendoring 固定。

不新增 `package.toml`、项目 lockfile、依赖下载或版本求解。

### 4.8 `print` 的第一阶段策略

**本计划不直接移除 `print`，也不假设它能立刻被一个普通 `.bas` 函数替换。**

原因：

- 当前 `print` 有 `OM_BUILTIN_PRINT` 身份；
- Type Inference 和 MIR lowering 中存在专门处理；
- 当前系统 Console 服务是通过 Actor Message 与 completion Message 提供的异步 I/O；
- OMBasic 没有 `AWAIT`，不能用一个同步表面 API 隐藏等待和 continuation。

第一阶段必须：

1. 审计 `print` 当前接受的参数类型和格式化规则；
2. 明确是否自动追加换行、输出失败如何处理、是否有 flush 语义；
3. 将实际行为写入语言标准和测试；
4. 把 `print` 明确标记为当前版本保留的 Runtime-backed Prelude primitive；
5. 不允许新标准库函数因为方便而继续增加同类编译器特判。

后续可以通过独立 ADR 评估是否保留、弃用或替换 `print`。如果未来需要 `@std/console`，它应遵守 Actor I/O 的显式异步语义，不得伪装成同步阻塞调用。

### 4.9 Runtime Service 与 Prelude 的分类

第一阶段不强行删除现有的 `Console`、`Timer`、`File_System` 和系统 Message。应先把它们从“普通语言核心内建”的概念中单独分类为 Runtime Service Protocol。

目标分类：

1. **Core Prelude：** `Unit`、`Result`、`Ok`、`Error`、Actor/Node 身份类型、生命周期类型与标准生命周期 Message。
2. **Runtime Service Protocol：** Console、Timer、File_System 及相关系统 Message。
3. **Runtime-backed primitive：** 当前的 `print`。
4. **普通标准库 API：** 通过 `@std` 显式导入的源码模块。

所有类别都应有明确的注册来源、类型规则、lowering 路径和测试；不得继续依赖不受审查的零散名称表。

---

## 5. 规范性示例

### 5.1 从 SDK 导入标准库

```basic
IMPORT "@std/math/math.bas"

ACTION main()
    value = math.abs(-42)
    print(value)
END ACTION
```

这里的 `abs` 是计划示例，不代表当前仓库已经实现该 API。

### 5.2 标准库内部使用相对路径

```text
stdlib/
└── math/
    ├── math.bas
    └── integer.bas
```

`math.bas`：

```basic
EXPORT IMPORT "./integer.bas"
```

`integer.bas`：

```basic
EXPORT FUNCTION add(left, right)
    RETURN left + right
END FUNCTION
```

调用方：

```basic
IMPORT "@std/math/math.bas"

ACTION main()
    value = math.add(10, 20)
    print(value)
END ACTION
```

### 5.3 项目文件和标准库文件同名

```text
src/
└── math/
    └── math.bas

<sdk>/share/ombasic/stdlib/
└── math/
    └── math.bas
```

以下导入指向不同 Module：

```basic
IMPORT "math/math.bas"
IMPORT "@std/math/math.bas"
```

由于两者的文件 stem 都是 `math`，在同一个 Module 中同时导入时仍会触发现有 Module Alias 冲突规则。0.1 不新增 Import Alias 语法。

### 5.4 非法写法

```basic
IMPORT "@STD/math/math.bas"
IMPORT "@unknown/math/math.bas"
IMPORT "@std/../outside.bas"
IMPORT "@std/math"
IMPORT "@std//math/math.bas"
IMPORT "@std\\math\\math.bas"
```

以上都应在解析或路径验证阶段失败，给出稳定、可定位的诊断，不得回退到项目目录。

---

## 6. 目标架构与数据流

```text
IMPORT literal
    |
    v
Import Path Classifier
    |
    +-- "foo/bar.bas" ---------> PROJECT_SOURCE root
    |
    +-- "./foo.bas" -----------> importer's Root_Kind + directory
    |
    +-- "@std/foo/bar.bas" ----> SDK_STDLIB root
    |
    v
Root-specific path validation
    |
    v
Canonical Module_Key
    (Root_Kind, relative path)
    |
    v
Module Graph deduplication / cycle detection
    |
    v
Lexer / Parser
    |
    v
Name Resolver + Prelude registry
    |
    v
Type Inference / Semantic Analysis
    |
    v
HIR / specialization / MIR
    |
    v
LLVM Native AOT backend
    |
    v
Target Runtime + Native linker
```

关键要求：

- `@std` 不绕过正常的 Parser、Type Inference 或 Module Graph。
- 标准库函数不会因为位于 SDK 中就自动成为 Builtin。
- SDK 资源路径解析和目标 Runtime archive 查找是两个不同职责。
- `check` 需要标准库源码时不需要链接 Runtime archive。
- `build` / `run` 需要 Runtime archive，但标准库源码仍走相同的 Import 解析流程。

---

## 7. 分阶段实施计划

## Phase 0 — 建立当前 Prelude 与标准库事实清单

**目标：** 在改动语义前，确认所有现有内建的注册、类型、lowering 与 Runtime 依赖。

### 任务

- [x] 以 `897db21` 为基线重新检查工作树；如果 `master` 已前进，更新本计划的基线。
- [x] 列出 `Om_Builtin_Id` 的全部枚举项。
- [x] 列出 `Om_Builtin_Member_Id` 的全部枚举项。
- [x] 列出 `name_resolver.c` 中实际注册的所有名称。
- [x] 列出 Type Inference 中针对 Builtin ID 的专门分支。
- [x] 列出 HIR / MIR lowering 中针对 Builtin ID 的专门分支。
- [x] 列出 Runtime Service Actor 和系统 Message 的名称、字段、payload 类型与完成语义。
- [x] 找出所有通过 `prelude_names` 增加名称的调用方。
- [x] 找出所有通过 `native_modules` 注入 Prelude 的调用方。
- [x] 检查所有相关 Conformance、semantic、type inference、MIR、runtime 测试。
- [x] 审计 `print` 的输入类型、输出格式、换行、错误路径和 Runtime 调用。
- [x] 检查 SDK 的 `target-manifest.json`、资源根解析和安装布局。
- [x] 检查 `check` / `build` / `run` 的资源依赖是否已经分离。

### 交付物

- `docs/development/prelude_inventory.md`
- 一份每个 Builtin 的“声明位置—类型规则—lowering—Runtime—测试—标准 Rule ID”对应表。
- 一份当前标准库 API 清单及缺失能力列表。

### 验收标准

- [x] 不存在只在 lowering 中出现、但未记录在 Builtin 清单中的隐式内建。
- [x] 每个系统 Actor / Message 都能追溯到标准条款与测试。
- [x] `print` 的实际行为已经通过源码和测试确认，而不是根据名称猜测。

---

## Phase 1 — ADR 与语言标准先行

**目标：** 在改动 Loader 之前，明确 Prelude 和 `@std` 的规范语义。

### 1.1 修订现有 Import 规则

更新 `OMBasic-Language-Standard.md`：

- [x] 修订 OM-21.02：明确 Project Source Root 与 SDK Standard Library Root 的不同角色。
- [x] 修订 OM-21.03：正式定义三种 Import 路径形式。
- [x] 修订 OM-21.04：允许 leading `./` 和 `@std/`，继续禁止 `..`、绝对路径、空段、反斜杠和路径逃逸。
- [x] 修订 OM-21.14：明确 `./` 保留 importer 的 Root_Kind，不能跨 Root。
- [x] 明确标准库 Module 不得导入调用方项目源码。
- [x] 明确 `@std` 只允许出现在 Import path literal 中，不是普通标识符语法。
- [x] 明确 `@std` 不会自动导入任何符号。
- [x] 明确 `IMPORT "@std/..."` 与普通 `.bas` Module 一样建立静态依赖。
- [x] 明确 `@std` 不改变 `EXPORT IMPORT` 的公开面与冲突语义。
- [x] 新增稳定 Rule ID；建议从 OM-21.18 起分配，但必须先检查 ID 未被占用。
- [x] 为 `check`、`build`、`run` 统一规定标准库资源缺失时的行为。

建议新增规则主题：

- `OM-21.18 — SDK Standard Library Root`
- `OM-21.19 — Root-aware Canonical Module Identity`
- `OM-21.20 — Standard Library Dependency Isolation`
- 如需拆分，再增加 `OM-21.21 — SDK Standard Library Version Identity`

以上名称是建议；Rule ID 分配必须以当前标准文件为准。

### 1.2 新增 ADR

建议新建：

`docs/architecture/adr/ADR-0027-explicit-stdlib-import-root.md`

实际创建前先确认 ADR-0027 未被其他提交占用。

ADR 应记录：

- 为什么标准库采用显式 `@std` 根；
- 为什么标准库源码不再要求复制到 Project Source Root；
- 为什么 `@std` 是一个固定、受信任、受安全检查的 SDK root，而不是任意搜索路径；
- 为什么 Module identity 必须带 Root_Kind；
- 为什么 `@std` 不等于 Package 或包管理器；
- 为什么标准库源码继续采用 Facade 和 `EXPORT IMPORT`；
- 为什么标准库依赖版本跟随 SDK，而不是由项目 lockfile 解析；
- 为什么 `print` 暂时保留为 Runtime-backed Prelude primitive。

### 1.3 更新既有 ADR

- [x] 修订 ADR-0020 中“唯一 Source Root”的相关文字：对每个 Root 保留严格 canonicalization 与安全边界；明确 `@std` 是唯一的显式 SDK 资源根例外。
- [x] 修订 ADR-0025：标准源码库仍然是源码树；SDK 标准库可以通过 `@std` 显式访问；第三方库仍由项目自己 vendoring / Git 固定。
- [x] 修订 ADR-0026：移除“No `@std` root”的旧决定；保留 Facade、Re-export、无目录扫描、无包管理器等决定。
- [x] 更新 `docs/compiler/resource_layout.md`。
- [x] 更新 `docs/development/spec_audit.md` 中仍然禁止 `./` 的旧描述。
- [x] 更新 `OMBasic-Source-Library-Facade-Plan-v2.md`，标记它已被新计划取代，避免继续作为执行依据。

### 验收标准

- [x] ADR 之间不再互相否定。
- [x] OM-21.02、OM-21.03、OM-21.04 与 OM-21.14 对 `./` 的规定一致。
- [x] 规范明确区分 Project Source Root 和 SDK Standard Library Root。
- [x] 所有路径语义都有正例、反例和错误处理规则。

---

## Phase 2 — 建立 Prelude / Builtin 注册体系

**目标：** 让 Prelude 成为可审查的、具有唯一事实来源的语言契约。

### 2.1 建立 Builtin 注册清单

建议新增一个内部注册表，例如：

- `include/ombasic/semantic/builtin_registry.h`
- `src/semantic/builtin_registry.c`

或使用项目现有风格的内部 `.def` 文件。

不要求必须使用以上确切文件名，但注册表必须避免“resolver、type inference、lowering 各自维护不同名称列表”。

每个条目至少应包含：

| 字段 | 含义 |
|---|---|
| Builtin ID | 稳定的语义身份 |
| Source spelling | OMBasic 源码中的名称 |
| Category | Core Prelude / Runtime Service / Runtime-backed primitive |
| Symbol kind | Type / Constructor / Actor / Message / Function / Member |
| Type rule | 具体类型或类型推导规则的位置 |
| Lowering kind | 普通调用、专门 lowering、Runtime Service 引用等 |
| Standard Rule ID | 规范依据 |
| Test IDs | 相关测试 |
| Documentation | 用户可读说明 |

实现不应把所有行为强行抽象成一个动态插件系统。类型推导和 lowering 仍然可以用显式 `switch (Builtin_Id)` 实现；重点是注册清单、名称、分类和文档不再漂移。

### 2.2 清理 `Om_Resolver_Config`

- [x] 明确 `prelude_names` 仅用于明确批准的编译器 / Runtime 声明。
- [x] 禁止把整套 `@std` API 作为 `prelude_names` 注入。
- [x] 明确 `native_modules` 何时允许形成 Prelude 声明。
- [x] 为重复名称、与内建冲突、非法命名增加稳定诊断。
- [x] 保证注册顺序不影响符号 ID 的语义与错误诊断。
- [x] 保证内建名称不能被用户模块遮蔽，符合现有标准。

### 2.3 明确各类内建的边界

#### Core Prelude

保留语言必须知道其语义的类型与构造器，例如：

- `Unit`
- `Result`
- `Ok`
- `Error`
- `Node_Ref`
- `Actor_Id`
- `Actor_Down_Reason`
- `Actor_Down` 与生命周期约定值

#### Runtime Service

- `Console`
- `Timer`
- `File_System`
- 相关系统 Message

这些名称有 Runtime 协议和 Actor lowering 语义，不能当作普通 `.bas` 函数处理。第一阶段保留它们，但在文档和注册表中与 Core Prelude 分开。

#### Runtime-backed primitive

- 当前 `print`

保留现有可用行为，并为其建立明确的类型、格式化、换行和错误语义。

### 2.4 类型推导与 lowering 一致性

- [x] 每个有专门类型推导规则的 Builtin 都在注册表中注明。
- [x] 每个有专门 HIR / MIR lowering 的 Builtin 都有测试。
- [x] 未知 Builtin ID 不能静默走普通函数路径。
- [x] 注册表中没有对应实现的条目应在开发期测试中失败。
- [x] 删除无调用方、无规范依据的历史 Builtin 条目时，先更新标准与测试。

### 2.5 Prelude Conformance

新增正向与负向测试：

- 核心类型与构造器可用；
- `Actor_Down_Reason` 的限定成员可用；
- 用户不能重新声明保留的内建名称；
- 普通导入 Module 不会把其所有公开函数注入 Prelude；
- `@std` 导入仍需显式写在源码中；
- `prelude_names` 冲突报告稳定诊断；
- Runtime Service 的消息 payload 类型正确；
- `print` 行为符合标准。

### 验收标准

- [x] Prelude 清单与实际注册完全一致。
- [x] Type Inference 和 MIR lowering 没有未登记的名称特判。
- [x] 所有 Prelude 公开名称都能追溯到 Rule ID。
- [x] 现有语义测试、Conformance 与 Native AOT 测试通过。

---

## Phase 3 — 实现 Root-aware Import Path Resolver

**目标：** 在不破坏已有路径安全机制的前提下支持 `@std`。

### 3.1 Import path 分类

在现有 `source/import_path` 能力上扩展路径分类。

概念 API：

```c
typedef enum Om_Import_Root_Kind {
    OM_IMPORT_ROOT_PROJECT_SOURCE = 0,
    OM_IMPORT_ROOT_SDK_STDLIB
} Om_Import_Root_Kind;

typedef enum Om_Import_Path_Kind {
    OM_IMPORT_PATH_SOURCE_ROOT_RELATIVE = 0,
    OM_IMPORT_PATH_MODULE_RELATIVE,
    OM_IMPORT_PATH_SDK_STDLIB
} Om_Import_Path_Kind;
```

以上是设计草案，具体命名应符合当前项目 C API 风格。

分类规则：

```text
"foo/bar.bas"         -> PROJECT_SOURCE + Source-Root-relative
"./foo.bas"           -> importer's Root_Kind + Module-relative
"@std/foo/bar.bas"    -> SDK_STDLIB + SDK-root-relative
```

### 3.2 Canonical Module Key

增加或扩展内部 Module Key，使其包含：

```text
Root_Kind
Canonical_Relative_Path
```

要求：

- 不使用绝对路径作为身份；
- `./` 在解析后被移除；
- `@std/` 转换为 SDK_STDLIB Root_Kind 与相对路径；
- Root_Kind 参与去重、接口指纹和构建图；
- Project 与 SDK 的同名文件不被错误合并；
- 同一标准库文件被多次导入时只加载一次。

### 3.3 文件系统 Root 管理

建议将路径解析与文件读取拆成清晰步骤：

1. 解析 Import literal；
2. 确定 Root_Kind；
3. 获取已规范化、已固定的 Root；
4. 对相对路径执行 lexical validation；
5. 安全打开目标文件；
6. 检查实际路径、大小写与 containment；
7. 生成 canonical Module Key；
8. 将源码加入 Module Graph。

Project Source Root 与 SDK Standard Library Root 应复用相同的安全原则，但不应把两个 Root 的路径字符串混为一谈。

### 3.4 错误处理

新增或复用稳定诊断，覆盖：

- 未知 `@` root；
- `@std` 路径格式错误；
- 标准库文件不存在；
- SDK 未提供标准库资源；
- manifest 无效或标准库资源目录缺失；
- `@std` 路径包含非法 segment；
- 标准库尝试导入 Project Source Root；
- Module-relative Import 试图跨 Root；
- Root 内部路径大小写不匹配；
- symlink / reparse-point traversal；
- canonical Module Key 冲突或路径别名。

诊断码应按项目现有诊断分配规范选取，先检查 `docs/compiler/diagnostics.md`，不得复用已有代码。

### 3.5 Loader 与 Module Graph

主要文件：

- `src/module_graph/module_loader.c`
- `include/ombasic/module_graph/module_loader.h`
- `src/source/import_path/import_path.c`
- `include/ombasic/source/import_path.h`

任务：

- [x] 让 Loader 能同时持有 Project Source Root 与 SDK Standard Library Root。
- [x] 让每个 Import 声明保留足够的信息，以便错误报告原始 literal 和 span。
- [x] 按 Root_Kind + canonical relative path 去重。
- [x] `EXPORT IMPORT` 与普通 `IMPORT` 使用同一套 root classifier。
- [x] `./` 在 Project 与 SDK 两种 Root 中都正确解析。
- [x] 标准库内部的普通 Project-root-relative Import 必须被拒绝。
- [x] Module cycle detection 跨两个 Root 正确工作。
- [x] Interface metadata 中记录稳定逻辑 Module path。
- [x] Module Graph 不依赖当前工作目录。
- [x] 解析失败不产生部分注册、重复 Module 或悬空 Source ID。

### 验收标准

- [x] 三种 Import 路径形式都能正确解析。
- [x] 相同 Root_Kind 和 canonical path 得到同一个 Module ID。
- [x] Project 与 SDK 同名 Module 得到不同 Module ID。
- [x] `EXPORT IMPORT "@std/..."` 与普通 `IMPORT "@std/..."` 具有一致的路径语义。
- [x] 不存在 Root 逃逸、搜索 fallback 或 symlink 绕过。

---

## Phase 4 — SDK 标准库资源布局与版本身份

**目标：** 让 `@std` 在安装后的 SDK 中可靠解析，并且不依赖源码仓库路径。

### 4.1 SDK 布局

目标布局：

```text
<sdk>/
├── bin/
│   └── ombasic[.exe]
├── include/ombasic/
├── lib/ombasic/<target-triple>/
│   ├── ombasic_runtime[.lib|.a]
│   └── ...
└── share/ombasic/
    ├── target-manifest.json
    ├── stdlib-manifest.json
    └── stdlib/
        ├── math/
        │   ├── math.bas
        │   └── integer.bas
        ├── text/
        │   └── text.bas
        ├── console/
        │   └── console.bas
        └── ...
```

未实现的模块不应提前放入 SDK 或伪装成已稳定 API。上面的目录只是目标布局示意。

### 4.2 SDK manifest

扩展当前 `target-manifest.json` 或以独立 `stdlib-manifest.json` 描述标准库资源。

建议标准库 manifest 包含：

- manifest format version；
- OMBasic / SDK release version；
- 标准库 API 版本；
- 标准库根目录相对路径；
- 每个已发布 `.bas` 文件的相对路径；
- 文件内容摘要；
- 可选的公开 API fingerprint；
- manifest 自身的内容摘要或校验字段。

不得包含：

- 开发机绝对路径；
- 构建目录绝对路径；
- 用户 home 目录；
- 与 SDK 安装位置绑定的路径。

manifest 是 SDK 资源清单，不是 Package manifest 或依赖 lockfile。

### 4.3 SDK Root 解析

沿用当前 SDK 资源根选择策略：

1. `--resource-dir <sdk-root>`；
2. `OMBASIC_RESOURCE_DIR`；
3. 已安装布局下可从可执行文件位置推导的 SDK 根；
4. build-tree SDK 布局下的既有推导方式。

具体顺序以当前实现为准，不应无故改变现有资源覆盖优先级。

标准库路径固定为 SDK Root 下的相对资源路径，例如：

```text
share/ombasic/stdlib/
```

如果显式选择的 SDK 缺少该资源，必须报告错误，不能偷偷使用另一个 SDK、源码仓库目录或系统路径。

### 4.4 `check`、`build`、`run` 的区别

- `check`：若源码使用 `@std`，需要 SDK 标准库源码与 manifest；不需要链接 Runtime archive。
- `build`：需要 SDK 标准库源码，并按当前 target 规则解析 Runtime archive 和链接工具。
- `run`：与 build 使用一致的标准库与 Runtime 解析结果，再执行生成的 Native executable。
- 不使用 `@std` 的 `check` 不应因为标准库源码缺失而无条件失败。
- 资源解析必须先区分“源码资源缺失”和“Runtime archive 缺失”，再给出明确错误。

### 4.5 构建可复现性

构建输入应记录：

- OMBasic compiler version；
- target triple；
- 标准库 manifest 摘要；
- 被导入标准库 Module 的内容摘要；
- Native `.omi` 与链接输入的摘要（若使用）。

不得把 SDK 的绝对安装路径作为 Module identity 或公开 API fingerprint 的一部分。

### 验收标准

- [x] SDK 安装后移动目录，`@std` 仍能解析。
- [x] SDK manifest 不含开发机绝对路径。
- [x] 标准库资源缺失时诊断清楚。
- [x] 不存在隐式 SDK fallback。
- [x] `check` 不要求 Runtime archive，但能够读取标准库源码。
- [x] `build` 与 `check` 使用同一标准库版本和同一 Import 语义。

---

## Phase 5 — Conformance、Fuzz 与安全测试

**目标：** 在实现标准库 API 前，先证明根类型和路径语义可靠。

### 5.1 Import Conformance

至少新增以下正向用例：

| 用例 | 预期 |
|---|---|
| `IMPORT "local/module.bas"` | 解析到 Project Source Root |
| `IMPORT "./local.bas"` | 解析到当前 Project Module 目录 |
| `IMPORT "@std/math/math.bas"` | 解析到 SDK Standard Library Root |
| `EXPORT IMPORT "./integer.bas"` | 正确重导出本地子模块 |
| `EXPORT IMPORT "@std/text/text.bas"` | 正确重导出标准库 Module |
| Project 与 SDK 中同路径文件同时存在 | Root-aware identity 区分二者 |
| 多个 Module 导入同一个 `@std` 文件 | Module Graph 只加载一次 |
| `@std` 内部 `./` Import | 留在 SDK Standard Library Root |
| 多个 `@std` Module 互相导入 | 正常建立图并进行循环检查 |

至少新增以下负向用例：

| 用例 | 预期 |
|---|---|
| `IMPORT "@unknown/x.bas"` | 未知 Root 诊断 |
| `IMPORT "@STD/x.bas"` | 大小写错误 / 未知 Root 诊断 |
| `IMPORT "@std/x"` | 缺少 `.bas`，失败 |
| `IMPORT "@std/../x.bas"` | 拒绝 parent traversal |
| `IMPORT "@std//x.bas"` | 拒绝空路径段 |
| `IMPORT "@std\\x.bas"` | 拒绝反斜杠 |
| 标准库 Module 导入项目 Source Root 文件 | 拒绝 |
| SDK Module 的 `./` 试图跨到 Project Root | 拒绝 |
| SDK manifest 缺失或损坏 | 明确 SDK 资源诊断 |
| `@std` 文件大小写与实际文件不一致 | 拒绝 |
| 标准库目录包含 symlink / reparse traversal | 拒绝 |

### 5.2 Module Graph / Re-export 测试

- [x] `EXPORT IMPORT "@std/math/integer.bas"` 进入普通 Module Graph。
- [x] 重导出同一原始符号不会制造冲突。
- [x] 两个不同原始符号同名时报告冲突。
- [x] 跨 Project / SDK Root 的循环能被检测。
- [x] Module identity 中的 Root_Kind 参与缓存与指纹。
- [x] 标准库公开面变化时接口 fingerprint 变化。
- [x] 标准库私有实现变化但公开面不变时，公开 API fingerprint 按既定规则保持稳定。

### 5.3 Fuzz

扩展现有 `tests/fuzz/import_path_fuzzer.c`，覆盖：

- 普通 Source Root-relative literal；
- leading `./`；
- `@std/`；
- 未知 `@` prefix；
- 极长路径段；
- 重复分隔符；
- 混合 slash / backslash；
- `.` / `..` 组合；
- UTF-8 非法字节；
- 整数长度溢出边界；
- Root kind 变换；
- canonicalization 前后不一致；
- symlink / reparse-point containment 测试。

Fuzzer 应调用真实的 path classifier / resolver，而不只是单独测试 canonical path validator。

### 5.4 SDK relocation 与安装测试

扩展 `tests/tooling/install_sdk_test.py`：

- [x] 安装 SDK。
- [x] 移动 SDK 目录。
- [x] 清除 `OMBASIC_RESOURCE_DIR`。
- [x] 在 SDK 外部创建一个独立项目。
- [x] 编译引用 `@std/math/math.bas` 的最小程序。
- [x] 运行 Native executable 并验证输出。
- [x] 执行只需语义分析的 `check`。
- [x] 删除标准库资源并确认报错，不回退到源码仓库。
- [x] 检查 manifest 不包含源码仓库绝对路径。

### 验收标准

- [x] Conformance manifest 中的每条新规则都有至少一个正向或负向 fixture。
- [x] Fuzz 不出现 assertion failure、越界、未检查溢出或错误 Root 逃逸。
- [x] Linux / Windows 对路径大小写和 symlink/reparse-point 的行为符合规范。
- [x] SDK relocation 测试证明 `@std` 不依赖开发机路径。

---

## Phase 6 — 第一批标准库：Math

**目标：** 用一个几乎不需要 Runtime 支持的纯源码库，验证 `@std` 的端到端链路。

### 6.1 API 设计先行

在写函数前新增一份标准库 API 设计文档，例如：

`docs/stdlib/math.md`

内容应包括：

- 函数名称和参数；
- 参数与返回类型推导；
- 边界行为；
- 整数溢出语义；
- Float / Int 支持范围；
- 错误或特殊值处理；
- 公开 API 与私有 helper 的划分；
- 测试向量。

不要为了让 Math API 看起来像其他语言而假设 OMBasic 已经支持函数重载或任意泛型。如果当前类型系统不能让一个 `abs` 安全地接受多种数值类型，应先明确类型策略，不要偷偷增加隐式转换。

### 6.2 建议的初始范围

优先实现可以由当前语言表达、行为明确、无 Runtime 副作用的函数。

候选 API（最终以 API 设计文档和类型系统能力为准）：

- `abs`；
- `min`；
- `max`；
- `clamp`。

第一阶段可以限定为 Int，或在标准明确支持时再增加 Float 版本。不要用未定义的 overload 语义强行实现一个“通用”函数。

### 6.3 文件布局

```text
stdlib/
└── math/
    ├── math.bas
    └── integer.bas
```

`math.bas` 是 Facade；内部实现使用 `EXPORT IMPORT "./integer.bas"`。只有公开 API 才应导出。

### 6.4 测试

- 正常输入；
- 负数与零；
- 最小 / 最大整数边界；
- `clamp` 的上下界关系；
- 非法范围，例如下界大于上界时的约定；
- 类型推导；
- Facade 公开面；
- 私有 helper 不可从 `math` 外部访问；
- API fingerprint；
- Native AOT executable 行为。

### 验收标准

- [x] `IMPORT "@std/math/math.bas"` 可以直接用于独立项目。
- [x] Math 所有公开函数均为普通 `.bas` 实现，除非某个函数被明确证明需要底层支持。
- [x] 不增加新的 compiler builtin 来实现普通 Math helper。
- [x] API 文档与 Conformance / unit tests 一致。
- [x] `add` 不再被误认为已完成的 Math API 设计；它可以保留为测试 fixture 或按设计文档调整。

---

## Phase 7 — Text：先定 Unicode 与 Native 边界

**目标：** 在扩大标准库之前，明确字符串操作的基本契约。

### 7.1 先做能力审计

审计当前 String 类型支持：

- 字符串相等与比较；
- 字符串插值与格式化；
- UTF-8 存储或边界表示；
- 字符串长度与索引语义；
- Unicode scalar value 与 UTF-8 byte offset 的区别；
- 子串、查找、切分和拼接的现有能力；
- 空字符串与无效 UTF-8 的行为。

不要假设 UTF-8 字节数等于字符数。

### 7.2 Native-backed 功能的原则

当前 `Om_Resolver_Config.native_modules` 可以将 Native metadata 声明加入 Prelude。**不要通过这种全局注入方式暴露整个 Text API。**

若 Text 需要 Runtime / C 实现：

- [x] 先审计现有 `.omi` 导入能力；
- [x] 如果 Native metadata 目前只能形成 Prelude 声明，设计一个显式、模块级 Native import 机制，或一个受控的内部 Runtime binding 模块；
- [x] Native 函数仍使用 `.omi` ABI metadata；
- [x] 明确 String 所有权、UTF-8 合法性、返回值释放、阻塞状态与错误映射；
- [x] 禁止 Native pointer 成为普通 OMBasic 值的隐式后门；
- [x] Native 函数不因属于标准库而绕过显式链接与目标 ABI 检查。

纯源码可实现的 Text helper 仍然应写在 `.bas` 中。

### 7.3 建议的 Text 初始范围

待 String 语义审计后再确定：

- 长度；
- 前后缀判断；
- 查找；
- 子串；
- 分割 / 拼接。

公开函数名和索引单位必须在 API 文档中明确。不要在同一个 API 中混用 UTF-8 byte offset 和 Unicode scalar index。

### 验收标准

- [x] 文本长度、索引与错误行为没有歧义。
- [x] Native-backed API 有明确的 ownership / ABI 说明。
- [x] 普通 Text helper 不新增 compiler special case。
- [x] UTF-8、多字节字符、空字符串和边界条件有测试。

---

## Phase 8 — Console：保留显式 Actor I/O 语义

**目标：** 为标准库提供方便的 Console API，同时不引入隐式等待。

### 8.1 当前边界

当前标准规定 Console I/O 通过系统 Actor 与 completion Message 暴露。`Console_Write` 写入给定文本，不自动追加换行；完成消息携带 `Result<Unit, String>`。

这与 `print` 的现有行为不是同一个接口层次。

### 8.2 建议 API 层次

- 低层：现有系统 Console Actor 协议。
- 标准库：显式导入的 Console 模块，提供清晰命名的 Message / Actor helper。
- 当前 `print`：继续作为保留的 Runtime-backed Prelude primitive，直到独立设计决定其未来。

Console API 不得：

- 把异步写入伪装成同步完成；
- 隐藏 Actor Message 边界；
- 引入 `AWAIT`；
- 通过隐式 continuation 让当前 Actor handler 挂起；
- 把写入失败默默吞掉。

### 8.3 验收标准

- [x] API 明确区分 write payload 与 write-line payload。
- [x] 换行行为固定且测试覆盖。
- [x] completion 与错误处理遵循当前 `Result` 语义。
- [x] 不改变现有 `print` 行为。
- [x] 不新增无必要的编译器特判。

---

## Phase 9 — File_System、Path 与 Time

这些模块应在基础 Import、Math 和必要的 Text 契约稳定后逐步实现。

### 9.1 File_System

- [x] 明确读写是 UTF-8 文本还是二进制；如果只提供文本 API，应在命名与文档中明确。
- [x] 明确 File_Read / File_Write 的 Result 错误语义。
- [x] 明确 File_Write 的覆盖行为。
- [x] 明确路径相对于进程工作目录还是应用提供的根路径。
- [x] 不允许用当前 SDK 标准库 Root 作为用户文件 I/O 的默认目录。
- [x] 文件操作继续通过 Runtime Service Actor 执行。
- [x] 不把阻塞文件操作直接放到 Actor scheduler worker。
- [x] 为文件不存在、受保护目标、非法编码、目录目标、存储错误等补充测试；平台相关错误按 Linux fixture 限定运行。

### 9.2 Path

- [x] 将路径字符串处理与实际文件系统访问分开。
- [x] 明确路径是逻辑路径还是 OS 路径。
- [x] 明确 slash、绝对路径、相对路径、`.`、`..` 的 API 行为。
- [x] Import 路径规则不能直接复用为用户文件路径 API 的全部语义；两者安全目标不同。
- [x] 不隐式读取或修改进程当前目录。

### 9.3 Time

- [x] 区分墙上时钟、单调时钟、持续时间与 Timer；当前只公开基于单调时钟的 Timer，不提供墙上时钟或 Duration API。
- [x] Timer 到期通过 Message 投递，不引入等待中的 call stack。
- [x] 明确单位、溢出、精度、时钟跳变和取消语义。
- [x] 测试定时消息的顺序与 Actor mailbox 交互。

### 验收标准

- [x] I/O 错误使用明确 Result / Message 返回。
- [x] 阻塞操作不隐式阻塞 Actor scheduler。
- [x] 时间概念不混用。
- [x] 所有模块都通过 `@std` 显式导入，且内部依赖遵守 Root 规则。

---

## Phase 10 — JSON：综合验证标准库架构

**目标：** 在前置模块稳定后，通过 JSON 验证复杂的跨模块源码库。

### 任务

- [x] 先定义 JSON Value 的数据模型。
- [x] 明确对象键顺序是否具有语义。
- [x] 明确数字解析范围与浮点精度。
- [x] 明确 UTF-8、转义和 Unicode 处理。
- [x] 明确 Parse Error 的结构、位置和错误信息。
- [x] 明确序列化顺序、转义策略和无效值处理。
- [x] 将 Parser、Value、Stringify 拆成内部 Module。
- [x] 使用 `json/json.bas` 作为 Facade。
- [x] 内部文件使用 `./`；跨标准库依赖使用 `@std/...`。
- [x] 所有公开 API 通过 `EXPORT` / `EXPORT IMPORT` 明确导出。

### 测试

- 标准 JSON 样例；
- 空对象与空数组；
- 嵌套结构；
- 数字边界；
- Unicode 与转义；
- 非法输入；
- 重复键策略；
- 深度与输入大小限制；
- 公开接口 fingerprint；
- Native AOT executable；
- Conformance 与 Fuzz。

### 验收标准

- [x] JSON 作为普通源码 Module 编译。
- [x] Parser 与 Serializer 的错误语义明确。
- [x] 不需要 JSON 专用 compiler builtin。
- [x] Facade 的公共 API 稳定且经过测试。

---

## 8. 编译器与 SDK 需要修改的文件

具体文件名以当前仓库结构为准。以下是主要落点，不代表每个文件都必须新增。

| 区域 | 主要文件 | 预计改动 |
|---|---|---|
| 语言标准 | `OMBasic-Language-Standard.md` | 修订 OM-21.02–21.04、OM-21.14，新增 `@std` 与 Root-aware identity 规则 |
| ADR | `docs/architecture/adr/` | 新增 `@std` ADR；修订 ADR-0020、0025、0026 的冲突决定 |
| Prelude registry | `include/ombasic/semantic/name_resolver.h`、`src/semantic/name_resolver/name_resolver.c` | 统一 Builtin 分类与注册 |
| Type Inference | `src/semantic/type_inference/type_inference.c` | 审计专门 Builtin 分支，避免标准库 API 继续进入特殊路径 |
| HIR / MIR | `src/ir/`，尤其 `src/ir/mir/lower_hir.c` | 保留必要 Builtin lowering；保证标准库普通调用走通用路径 |
| Import path | `include/ombasic/source/import_path.h`、`src/source/import_path/import_path.c` | 添加 Root 分类和 root-aware canonicalization |
| Module Loader | `include/ombasic/module_graph/module_loader.h`、`src/module_graph/module_loader.c` | 管理双 Root、canonical Module Key 和安全检查 |
| SDK manifest | `CMakeLists.txt` 及 SDK 资源生成逻辑 | 写入标准库相对路径、版本与 manifest |
| SDK 布局文档 | `docs/compiler/resource_layout.md` | 说明 `@std` root 与 SDK relocation |
| SDK 安装测试 | `tests/tooling/install_sdk_test.py` | 验证移动 SDK 后的 `@std` 解析与编译 |
| Conformance | `tests/conformance/manifest.json` 和 fixtures | 新增 Import、Prelude、Module identity 测试 |
| Fuzz | `tests/fuzz/import_path_fuzzer.c` | 覆盖 `@std` 与 Root-aware path resolver |
| 标准库源码 | `stdlib/math/` 等 | 将示例逐步替换为经过设计的公开 API |
| 文档 | `docs/stdlib/`、`docs/development/` | 标准库 API 文档、Prelude inventory 和迁移记录 |
| LSP | `tools/ombasic_lsp.py` 及相关测试 | 识别 `@std` 逻辑路径，保证 hover、definition、completion 的模块路径稳定 |

---

## 9. 测试矩阵与质量门槛

每个阶段都必须运行对应的最小测试集，最终阶段运行完整矩阵。

| 测试类别 | 主要验证内容 |
|---|---|
| Unit Tests | Builtin registry、Import path classifier、canonical Module Key |
| Semantic Tests | Prelude 可见性、类型规则、名称冲突、Native metadata |
| Type Inference Tests | Core Prelude 与 `print` 的实际类型行为 |
| Module Graph Tests | Root-aware dedup、cycle detection、Re-export |
| Conformance | OM-21.02–21.20 对应的正向与负向用例 |
| Fuzz | Import literal、Root selection、path validation、checked arithmetic |
| SDK Install Tests | 安装、移动 SDK、清除环境变量后的标准库解析 |
| Native AOT Tests | 从 `@std` 导入的程序生成原生可执行文件并运行 |
| LSP Tests | 标准库 Module 的 completion、hover、definition |
| Linux CI | Clang、LLVM、LLD、Runtime、SDK relocation |
| Windows CI | clang-cl、MSVC ABI、SDK relocation、路径安全 |
| Reproducibility | 标准库 manifest、Module fingerprint、无绝对构建路径 |

### 必须保持的质量属性

- [x] 用户输入错误只能产生诊断，不得触发 assertion failure。
- [x] 路径解析必须使用 checked size arithmetic。
- [x] SDK 资源缺失不得触发隐式 fallback。
- [x] 标准库源码不得通过 Import 读取项目外的任意文件。
- [x] Module identity 不得依赖 SDK 的安装绝对路径。
- [x] 新增标准库 API 不得无故增加编译器专用 lowering。
- [x] Conformance 规则 ID 不得重复或被重用。
- [x] 所有新增错误码必须在 diagnostics 文档中登记。

---

## 10. 建议的 Git 提交顺序

每个提交应尽量保持一个清晰、可独立审查的目的。

1. `docs: audit prelude and builtin boundaries`
   - 新增 Prelude inventory。
   - 只记录现状，不改变行为。

2. `docs: define explicit stdlib import root`
   - 修订语言标准。
   - 新增 `@std` ADR。
   - 修订 ADR-0020、0025、0026 与相关文档。

3. `semantic: centralize builtin registry`
   - 统一 Builtin 分类与注册。
   - 保持现有行为不变。

4. `source: classify project relative and stdlib imports`
   - 实现 Import root 分类。
   - 增加 root-aware canonical Module Key。

5. `module-graph: support sdk stdlib root`
   - Loader、Graph、Re-export、循环检测与身份去重。

6. `sdk: manifest and install standard library sources`
   - SDK manifest 与安装布局。
   - 保证不写入绝对路径。

7. `test: cover stdlib import roots and security`
   - Conformance、Fuzz、SDK relocation 测试。

8. `stdlib: implement math api v1`
   - API 文档、源码实现与测试。

9. `stdlib: add text primitives`
   - 在 Unicode 与 Native 边界完成设计后实施。

10. `stdlib: add console wrappers`
    - 保留现有 `print`，明确 Console Actor API。

11. `stdlib: add file system path and time modules`
    - 按各自 API 设计逐步提交。

12. `stdlib: add json module`
    - 综合验证 Facade、Re-export、Text 与错误模型。

13. `docs: publish prelude and standard library guide`
    - 完成用户文档、迁移说明和 SDK 使用示例。

如果某阶段发现现有架构不支持预期功能，应先提交 ADR / 设计修订，再继续实现，不要在单个大型提交中同时改变规范、Loader、Runtime 和标准库 API。

---

## 11. 风险与缓解措施

### 风险 1：`@std` 与 Project Source Root 的 Module identity 冲突

**原因：** 两个根下可能有相同相对路径。

**缓解：**

- Module Key 必须包含 Root_Kind。
- 绝不以 basename 或相对路径单独去重。
- 增加项目与 SDK 同路径的测试。
- 将 Root_Kind 纳入接口指纹和构建图。

### 风险 2：标准库通过路径引用应用源码

**原因：** 标准库若能使用 Project-root-relative Import，就可能意外依赖调用方项目。

**缓解：**

- 标准库源码禁止普通 Project-root-relative Import。
- 同目录依赖使用 `./`。
- 跨标准库依赖使用 `@std/...`。
- 在 Conformance 中增加标准库依赖隔离测试。

### 风险 3：SDK 资源路径被开发机环境污染

**原因：** 构建树或绝对路径被写入 manifest。

**缓解：**

- manifest 只存相对路径。
- SDK relocation 测试必须通过。
- 缺少标准库资源时明确失败。
- 不搜索源码仓库作为 fallback。

### 风险 4：Prelude 继续膨胀

**原因：** 开发者为了方便，把每个常用函数都注册为 Builtin。

**缓解：**

- 新增 Builtin 必须说明为何普通 `.bas` 或现有 Runtime API 不足。
- 每个 Builtin 必须在 registry 中有分类、Rule ID 和测试。
- 标准库函数默认不进入 Prelude。
- 对新的专门 lowering 设置单独审查门槛。

### 风险 5：`print` 与 Console Actor 语义混淆

**原因：** 同步输出 primitive 与异步 Console Service 的行为不同。

**缓解：**

- 第一阶段保留 `print`。
- 文档分别说明输出 primitive 和 Console Actor 协议。
- 不使用隐式 continuation 或 `AWAIT` 伪装同步行为。
- 未来若替换 `print`，通过独立 ADR 和标准版本变化处理。

### 风险 6：标准库 API 依赖尚未稳定的字符串或泛型能力

**原因：** 为追求熟悉的 API 形状而引入未定义的类型行为。

**缓解：**

- Math 初始 API 限定在现有类型系统能表达的范围。
- Text 先完成 Unicode / 索引 / Native ownership 审计。
- 不假设函数重载、隐式转换或任意泛型。
- 公开 API 先写规范和测试，再实现源码。

### 风险 7：Native metadata 被当作全局 Prelude

**原因：** 当前 `native_modules` 能把 Native 声明变成 Prelude 函数。

**缓解：**

- 只把编译器确实需要的 Native declarations 保留在 Prelude。
- 不通过全局 `prelude_names` 注入整套标准库。
- Text 等需要 Native-backed 功能时，先设计显式模块级边界。
- 保留 `.omi` 与 `.bas` 的职责分离。

---

## 12. 最终 Definition of Done

### Prelude

- [x] 当前所有 Builtin 已完成分类。
- [x] Prelude registry 是内建名称与类别的单一事实来源。
- [x] 类型推导和 lowering 的特殊路径有明确登记。
- [x] `print` 的实际行为被规范和测试覆盖。
- [x] Runtime Service 与 Core Prelude 已区分。
- [x] Prelude 不包含普通标准库函数。

### `@std` Import

- [x] `IMPORT "@std/..."` 已进入语言标准。
- [x] OM-21.02–21.04 与 OM-21.14 不再矛盾。
- [x] ADR-0020、0025、0026 与新 ADR 一致。
- [x] Root-aware Module identity 已实现。
- [x] `./`、Project-relative、`@std` 路径语义一致且无歧义。
- [x] 标准库依赖不能逃到项目 Root。
- [x] 路径安全、大小写、symlink/reparse-point 和循环检测全部通过。

### SDK

- [x] SDK manifest 包含标准库路径和版本身份。
- [x] SDK relocation 后 `@std` 仍可使用。
- [x] manifest 不含开发机绝对路径。
- [x] `check`、`build`、`run` 的资源解析行为一致且错误明确。
- [x] 标准库资源缺失不会静默 fallback。

### 标准库

- [x] Math v1 有稳定 API 文档、实现和测试。
- [x] 至少一个跨模块 Facade / Re-export 用例使用 `@std`。
- [x] 标准库模块由普通 OMBasic 源码实现，除非明确需要 Native 支持。
- [x] Text 的 Unicode / Native 边界在扩展前已完成设计。
- [x] Console API 不隐藏 Actor I/O 的异步语义。
- [x] 后续 File_System、Path、Time、JSON 都有各自的 API 设计和测试门槛。

### 工程质量

- [x] Conformance、Unit、Semantic、Native AOT 与 SDK 安装测试通过。
- [x] Fuzz 未发现崩溃、越界或 Root 逃逸。
- [x] Linux 与 Windows CI 均通过。
- [x] 用户文档、ADR、语言标准和实际实现一致。
- [x] 不引入 Package、包管理器、lockfile 或 VM。

---

## 13. 最终建议

不要把这项工作理解成“往 `stdlib/` 里多放几个 `.bas` 文件”。真正的工程任务是建立三个稳定边界：

1. **Prelude 边界：** 哪些名字自动存在，哪些语义必须由编译器理解。
2. **标准库边界：** 哪些能力通过 `@std` 显式导入，哪些由普通 OMBasic 源码实现。
3. **Runtime / Native 边界：** 哪些操作需要系统服务、Native ABI 或专门 lowering，以及其错误、所有权和阻塞语义。

推荐先完成 Phase 0–2，冻结 Prelude 清单和 `@std` 规范，再实现 Phase 3–5 的路径与 SDK 机制。随后以 Math 验证端到端链路，再逐步实现 Text、Console、File_System、Path、Time 和 JSON。

这样 OMBasic 可以获得实用的标准库，同时保留最初的设计目标：**源码显式、模块边界清楚、控制流可见、局部分析能力强，并且不依赖隐藏的包管理或运行时魔法。**
