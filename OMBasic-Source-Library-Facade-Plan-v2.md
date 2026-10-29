# OMBasic 源码库、目录 Facade、相对 Import 与标准库重构计划

-   **项目：** OMBasic
-   **计划日期：** 2026-10-29
-   **审计基线：** `d2fdb646cd6705bfcb61181c0bf25300e9abd8e5`
-   **核心功能提交：** `897db21b496ff87d3c3052a27847a771befad2f5`
-   **状态：** 已实现（Facade、Re-export、相对 Import、SDK 标准库交付）；ADR-0026 Accepted。Release Candidate gate 尚需维护者按发布文档完成最终审查；可选的 `ombasic init --stdlib` 不属于核心交付。
-   **目标：** 在不引入
    Package、包管理器、注册表、依赖求解器或锁文件的前提下，建立简单、显式、可分析的源码库组织和公共
    API 导出机制。

------------------------------------------------------------------------

## 1. 执行摘要

本计划把 OMBasic 的源码复用模型明确为：

> **文件是 Module，目录组织源码，同名 `.bas` 是目录的公共入口，`EXPORT`
> 定义公共 API，`IMPORT` 声明源码依赖。**

建议采用以下约定和语义：

1.  一个 `.bas` 文件始终是一个独立的语言级 Module；目录本身不是
    Module，也不是 Package。
2.  如果目录 `json/` 中存在 `json.bas`，那么 `json/json.bas`
    按约定是该目录的 **Facade Module（门面模块）**。
3.  调用方显式导入 Facade 文件，例如 `IMPORT "vendor/json/json.bas"`。
4.  当前编译器按被导入文件的文件名（去掉 `.bas`）生成 Module
    Alias，因此上述导入后使用 `json.parse(...)`。不引入 Import Alias
    语法。
5.  新增 `EXPORT IMPORT "./parser.bas"`，表示导入该
    Module，并将它的公开声明显式重新导出到当前 Module 的公共 API。
6.  Re-export
    是**显式、平铺、按符号名**的；不扫描目录，不自动导出目录中的所有文件，也不自动公开子模块别名。
7.  保持唯一 Source Root、路径大小写和 `.bas` 扩展名规则。普通 Import
    路径相对于 Source Root；唯一前导 `./` 路径相对于当前 `.bas` 文件，最终
    都规范化为 Source-Root-relative canonical identity。
8.  源码库通过 Git 仓库、Git tag/commit、Git
    submodule/subtree、源码归档或手工 vendoring
    分发。编译器不负责下载、安装、更新或解析版本。
9.  标准库本身也采用相同的 `.bas` 和 Facade 规则。为遵守单一 Source
    Root，第一版不增加 `@std` 第二导入根；需要使用的标准库源码被复制或
    vendored 到项目 Source Root 下。
10. `print` 目前仍然是编译器/Runtime
    特殊路径。本计划不把它贸然伪装成普通 `.bas` 函数；标准库和 Console
    API 的迁移另设明确阶段。

**核心目标示例：**

``` text
src/
├── main.bas
└── vendor/
    └── json/
        ├── json.bas
        ├── value.bas
        ├── parser.bas
        └── stringify.bas
```

`main.bas`：

``` basic
IMPORT "vendor/json/json.bas"

ACTION main()
    value = json.parse("{\"ok\": true}")
    text = json.stringify(value)
    print(text)
END ACTION
```

`vendor/json/json.bas`：

``` basic
EXPORT IMPORT "./value.bas"
EXPORT IMPORT "./parser.bas"
EXPORT IMPORT "./stringify.bas"
```

这样调用方只依赖 `json/json.bas`
这个公共入口；目录内部的模块划分不会要求调用方逐一 Import。

> 注意：当前标准要求 Import 路径包含 `.bas`，因此规范写法是
> `IMPORT "vendor/json/json.bas"`，不是省略扩展名的
> `IMPORT "vendor/json/json"`。

------------------------------------------------------------------------

## 2. 当前源码基线分析

### 2.1 已经存在的架构决策

#### ADR-0020：严格 Source Root 路径

文件：`docs/architecture/adr/ADR-0020-strict-source-tree-paths.md`

当前约束包括：

-   Source Root 在加载期间被规范化并固定。
-   Import 路径必须在 Source Root 下。
-   不允许通过符号链接或 reparse point 绕过 Source Root 边界。
-   路径段必须与文件系统实际大小写完全一致。
-   不允许搜索目录或隐式 fallback。

语言标准第 21.2--21.4 节规定唯一 Source Root 和两种显式 Import literal
形式：Source-Root-relative 与带唯一前导 `./` 的 Current-Module-relative。

**本计划保留唯一 Source Root、安全检查和无搜索规则。** 普通路径保持
Root-relative；仅显式的 `./` 前缀使用当前 Module 相对路径，随后归一到同一
Root-relative canonical identity。不加入 `@std` 这种隐式第二根路径。

#### ADR-0025：源码库与 Native 二进制库分离

文件：`docs/architecture/adr/ADR-0025-source-and-native-library-distribution.md`

当前决定已经覆盖：

-   OMBasic 源码库是普通目录子树。
-   源码复用使用 `.bas` 和 `IMPORT`。
-   源码库可通过 Git、源码归档或 vendoring 分发。
-   不引入 `Package`、`package.toml`、注册表、依赖求解器或锁文件。
-   Native 二进制库通过 `.omi`、显式链接配置和平台 ABI 使用。
-   `.a`、`.lib`、`.so`、`.dylib`、`.dll` 等不通过 `IMPORT` 导入。
-   OMBasic 0.1 不定义一般用途的预编译 OMBasic 二进制模块 ABI。

本计划应在 ADR-0025 的基础上增加 Facade/Re-export
约定，而不是另造一个包系统。

### 2.2 当前 Import 解析实现

  ----------------------------------------------------------------------------------------------------------------------------------------------
  文件                                                            当前职责                                  对本计划的影响
  --------------------------------------------------------------- ----------------------------------------- ------------------------------------
  `include/ombasic/source/import_path.h`                          声明 Import 路径验证 API                  保留 `.bas` 路径契约

  `src/source/import_path/import_path.c`                          校验相对路径、路径段、扩展名和            不增加目录搜索或无扩展名语法
                                                                  `lower_snake_case`                        

  `include/ombasic/source/file_loader.h`                          定义 Source Root 和安全文件加载 API       Source Root 安全边界保持不变

  `src/source/file_loader/file_loader.c`                          加载 Source Root                          Re-export 的目标仍走同一安全加载路径
                                                                  下的源文件并执行路径/大小写/逃逸检查      

  `include/ombasic/module_graph/module_loader.h`                  定义 `Om_Module_Project`、Module Import   可能需要扩充 Import 元数据查询能力
                                                                  信息                                      

  `src/module_graph/module_loader.c`                              在完整语义分析前扫描 Module 前缀、加载    当前扫描器只识别 `IMPORT`，必须识别
                                                                  Import、构建依赖图                        `EXPORT IMPORT`

  `src/module_graph/module_graph.c`                               建立 Module 节点、Import                  Re-export
                                                                  边和拓扑顺序，检测循环                    继续使用同一依赖图，不创建第二张图

  `src/syntax/parser/parser.c`                                    解析顶层 `IMPORT` 和声明                  当前 `EXPORT` 只修饰声明；需要新增
                                                                                                            `EXPORT IMPORT` 语法

  `include/ombasic/syntax/ast.h`                                  定义                                      可复用 Import 节点，但建议新增专用
                                                                  `OM_AST_IMPORT`、`OM_AST_FLAG_EXPORTED`   `OM_AST_FLAG_REEXPORT`
                                                                  等 AST                                    

  `src/semantic/name_resolver/name_resolver.c`                    生成 Module Alias、声明符号、解析         当前只在目标 Module 自己的 scope
                                                                  `module.member` 并检查可见性              查找符号，需要增加导出表

  `include/ombasic/semantic/name_resolver.h`                      定义 `Om_Symbol`、`Om_Resolution_Result`  需要公开可查询的 Module 导出面
                                                                  和诊断码                                  

  `tests/conformance/06_modules/`                                 当前模块导入和可见性用例                  扩充 Facade/Re-export 的正反例

  `tests/module_graph/`、`tests/semantic/name_resolver_tests.c`   Module graph 和符号解析测试               分别覆盖加载阶段和语义阶段

  `OMBasic-Language-Standard.md`                                  规范性语言规则                            新语法必须先写入标准和 Conformance

  `docs/compiler/resource_layout.md`                              SDK 安装资源布局                          后续标准库源码交付需要更新安装布局
  ----------------------------------------------------------------------------------------------------------------------------------------------

### 2.3 审计基线（`d2fdb646`）与目标之间的差距

本节记录计划编写时的基线状态，不代表当前实现状态。相关差距已由后续 Phase 0–7 工作关闭。

#### 已经符合目标的部分

-   `.bas` 文件路径是 Module 的身份。
-   `IMPORT` 必须给出明确的带引号路径。
-   `IMPORT "storage/database.bas"` 的 Alias 为 `database`。
-   `IMPORT "vendor/json/json.bas"` 的 Alias 为 `json`。
-   Alias 来自文件名 stem，而不是完整目录路径。
-   同名 Alias 指向不同 Module 时会产生冲突诊断。
-   Module 依赖图已经支持拓扑排序和循环检测。
-   `EXPORT` 已用于函数、动作、Record、Message 和 Actor 等声明。
-   AST 已经有 `OM_AST_IMPORT` 和 `OM_AST_FLAG_EXPORTED`。
-   当前公开声明和私有声明的可见性由 Name Resolver 检查。

#### 审计基线中尚未实现的部分

-   没有目录 Facade 的规范约定。
-   没有 `EXPORT IMPORT` 语法。
-   Module Loader 的前置扫描器不识别 `EXPORT IMPORT`。
-   Resolver 没有"某 Module 的最终公共导出面"这一独立数据结构。
-   `json.parse` 目前只能解析 `json/json.bas`
    自己的声明；不会自动搜索或合并 `json/` 目录下其他文件的导出。
-   类型推导、HIR、LSP 和接口 metadata 没有统一的 Re-export 查询接口。
-   仓库当前没有独立的标准库源码树；SDK `share/ombasic/`
    当前主要放置工具和资源 metadata。
-   现有 `print`
    仍有专用编译路径，不应被当作以后所有标准库函数的实现模板。

### 2.4 语言冻结注意事项

当前语言标准状态是
`0.1 Language Freeze Candidate`。`docs/development/language_freeze_policy.md`
规定，语法或静态语义变化属于语义变更；在 `0.1.0-rc1`
之后，除标准冲突修复外需要正式例外并重新启动 Release Candidate 周期。

因此：

-   应在 0.1.0-rc1 之前完成 ADR、标准规则和 Conformance。
-   如果已经发布或标记
    `0.1.0-rc1`，先按冻结政策处理，不得只改实现而不改规范。
-   不得让实现先支持 `EXPORT IMPORT`，再把它当作"实现细节"补进标准。

------------------------------------------------------------------------

## 3. 目标设计与不变量

### 3.1 目标模型

``` text
Source Root
   │
   ├── main.bas
   │     └── IMPORT "vendor/json/json.bas"
   │
   └── vendor/json/
         ├── json.bas        Facade Module
         ├── value.bas       Supporting Module
         ├── parser.bas      Supporting Module
         └── stringify.bas   Supporting Module
```

语言级实体仍然只有：

-   `.bas` 文件；
-   `IMPORT`；
-   `EXPORT`；
-   新增的 `EXPORT IMPORT`；
-   由 Import 建立的 Module Alias；
-   Module 之间的静态依赖图。

`vendor/json/` 是目录组织，不是编译器实体。它没有 Package
ID、版本求解、隐式初始化或独立运行时身份。

### 3.2 必须保持的不变量

1.  一个 `.bas` 文件只有一个 Module 身份。
2.  一个规范源文件路径只对应一个 Module。
3.  Module Alias 仍由文件名 stem 决定。
4.  `IMPORT` 只导入 `.bas`，不导入 `.omi` 或平台二进制。
5.  所有导入路径仍相对于唯一 Source Root。
6.  导入路径仍使用 `/`、精确大小写、`lower_snake_case` 路径段和 `.bas`
    扩展名。
7.  不自动扫描目录。
8.  不自动导出目录内的其他 Module。
9.  不自动把子模块 Alias 变成父 Facade 的公共成员。
10. `EXPORT IMPORT` 只公开目标 Module 的公开符号。
11. 私有声明不能因为 Re-export 而变为公开。
12. 重导出不生成运行时初始化，不复制代码，也不创建包装函数。
13. 所有 Re-export 依赖都进入现有 Module Graph，并接受现有循环检测。
14. 一个公共名字在一个 Module 的导出面中必须具有唯一来源。
15. Native 库继续使用 `.omi` 和显式链接配置；不混入源码库导入系统。

------------------------------------------------------------------------

## 4. 目录 Facade 约定

### 4.1 定义

**Directory Facade Convention（目录门面约定）**

如果目录名为 `X`，且该目录下存在 `X.bas`，则 `X/X.bas`
是该目录按约定提供的公共入口 Module。

例如：

``` text
json/
├── json.bas
├── value.bas
├── parser.bas
└── stringify.bas
```

其中 `json/json.bas` 是 Facade Module。

该规则是源码组织约定，不把目录升级为语言实体，也不要求每个目录都必须有
Facade。

### 4.2 使用方式

``` basic
IMPORT "vendor/json/json.bas"

ACTION main()
    result = json.parse(text)
END ACTION
```

Alias `json` 来自 `json.bas`，不是来自 `vendor/json/`
的完整路径。此行为与当前 `module_alias_name()` 实现一致。

### 4.3 Facade 公开哪些内容

Facade 的公开 API 由两部分组成：

1.  Facade 自己显式声明的 `EXPORT` 声明；
2.  Facade 使用 `EXPORT IMPORT` 显式重新导出的声明。

**不包括**：

-   目录中未被 Import 的文件；
-   仅被普通 `IMPORT` 导入的 Module 的符号；
-   被导入 Module 的私有声明；
-   作为内部 Alias 存在的子 Module 名称。

例如：

``` basic
IMPORT "vendor/json/parser.bas"

EXPORT FUNCTION parse_text(text)
    RETURN parser.parse(text)
END FUNCTION
```

这里只导出 `parse_text`，不自动导出 `parser.parse`。

如果希望调用方使用 `json.parse`，应明确写：

``` basic
EXPORT IMPORT "./parser.bas"
```

### 4.4 不做目录自动聚合

禁止以下隐式行为：

-   因为目录里有 `parser.bas` 就自动公开 `parser`；
-   因为目录里有 `EXPORT FUNCTION` 就自动把该函数加入 Facade；
-   因为文件被复制进目录就改变公共 API；
-   因为新建一个 `.bas` 文件就改变 Facade 的导出集合。

目录内容变化不得在没有 Facade 源码变化的情况下，静默改变 Facade 的公开
API。

------------------------------------------------------------------------

## 5. 新增 `EXPORT IMPORT` 语义

### 5.1 语法

新增一种顶层 Module 声明：

``` basic
EXPORT IMPORT "./parser.bas"
```

语法上，它是 `EXPORT` 与 `IMPORT` 的组合，不新增关键字。`EXPORT` 和
`IMPORT` 都是现有关键字。

普通导入保持不变：

``` basic
IMPORT "vendor/json/parser.bas"
```

### 5.2 语义

`EXPORT IMPORT "path/module.bas"` 同时执行两个明确动作：

1.  像普通 `IMPORT` 一样导入目标 Module，使当前 Module 可以通过文件名
    Alias 引用它；
2.  将目标 Module 的**公开导出面**重新导出到当前 Module 的公开导出面。

假设 `parser.bas`：

``` basic
EXPORT FUNCTION parse(text)
    ...
END FUNCTION

FUNCTION parse_internal(text)
    ...
END FUNCTION
```

而 `json.bas`：

``` basic
EXPORT IMPORT "./parser.bas"
```

调用方：

``` basic
IMPORT "vendor/json/json.bas"

ACTION main()
    value = json.parse(text)
END ACTION
```

`json.parse` 合法，`json.parse_internal` 不合法。

`EXPORT IMPORT` 不创建包装函数；`json.parse` 最终指向 `parser.bas`
中的原始声明。

### 5.3 Re-export 是平铺的

`EXPORT IMPORT` 将公开符号加入当前 Module 的导出面，而不是导出目标
Module 的 Alias。

例如：

``` basic
EXPORT IMPORT "./parser.bas"
```

外部 API 是：

``` basic
json.parse(text)
```

而不是：

``` basic
json.parser.parse(text)
```

子模块 Alias 只在 Facade 自己的 Module Scope 中可用。

### 5.4 支持传递式 Re-export

如果：

``` text
json/json.bas
    EXPORT IMPORT parser.bas

parser.bas
    EXPORT IMPORT lexer.bas

lexer.bas
    EXPORT FUNCTION tokenize(...)
```

则 `json` 的最终导出面包含 `tokenize`，前提是所有路径都符合现有 Source
Root 规则。

理由：

-   Module Graph 已经是无环图；
-   导出面按依赖优先的拓扑顺序计算；
-   传递式导出可以明确计算，不需要运行时机制；
-   Facade 只暴露其明确导入链上的公开符号。

### 5.5 导出冲突规则

以下情况必须产生编译错误：

-   两个不同的原始声明通过 Re-export 导出同名符号；
-   一个 Facade 自己声明了 `parse`，同时又 Re-export 另一个来源的
    `parse`；
-   Facade 的本地声明与 Re-export 的公开名字冲突；
-   Re-export 后发生大小写或普通名称规则冲突。

不得按声明顺序、Import 顺序或"本地优先"规则静默覆盖。

同一个原始符号通过多个有效路径到达同一导出面时，可按原始 `Om_Symbol_Id`
去重；只有不同原始符号的同名冲突才构成歧义。

诊断应包含：

-   冲突名称；
-   两个原始声明的位置；
-   两条对应的 Re-export 路径或直接声明位置。

建议新增语义诊断
`E0308`：`Re-exported public name conflicts with another export`。实现前必须核对诊断注册表，并同步更新
`src/tooling/check.c` 和诊断测试；不得复用已有的
`E0307`（当前用于不可变绑定赋值错误）。

### 5.6 可见性规则

-   `EXPORT IMPORT` 只重导出目标 Module 的公开导出面。
-   私有符号不会被重导出。
-   不支持按名称选择重导出。
-   不支持重命名导出。
-   不支持 Import Alias。
-   不支持 `IMPORT "json"` 或省略 `.bas`。
-   不支持通过目录名自动选择 Facade。
-   `EXPORT IMPORT` 仍必须出现在 Module 的顶层 Import 前缀区。
-   `EXPORT IMPORT` 的目标必须是有效的 `.bas` Module。
-   Re-export 不能绕过现有 Source Root、大小写、符号链接和路径逃逸检查。

### 5.7 普通 `IMPORT` 与 `EXPORT IMPORT` 的区别

  -----------------------------------------------------------------------------------------------
  写法                                                     当前 Module       目标公开符号可被当前
                                                  内部可使用目标 Alias        Module 的调用方访问
  ------------------------------------------ ------------------------- --------------------------
  `IMPORT "vendor/json/parser.bas"`                                 是        否，除非当前 Module
                                                                            自己声明/导出包装 API

  `EXPORT IMPORT "./parser.bas"`                          是            是，目标 Module
                                                                         的公开导出面被平铺重导出

  仅仅在同一目录放置 `parser.bas`                                   否                         否

  仅仅在目录里声明 `EXPORT FUNCTION`                                否                         否
  -----------------------------------------------------------------------------------------------

------------------------------------------------------------------------

## 6. Import 路径与可移植性

### 6.1 路径解释

普通 Import 路径仍相对于唯一 Source Root。以 `./` 开头的路径相对于当前
`.bas` Module 所在目录解析。这让库内依赖可以随源码子树一起移动，同时保留
项目入口和其他路径的现有 Root-relative 解释。

例如，项目结构：

``` text
src/
├── main.bas
└── vendor/
    └── json/
        ├── json.bas
        └── parser.bas
```

`json.bas` 内写：

``` basic
EXPORT IMPORT "./parser.bas"
```

而不是：

``` basic
EXPORT IMPORT "parser.bas"
```

### 6.2 明确的代价

`./` 路径仍受固定 Source Root 安全边界约束，并禁止 `..`、绝对路径和
符号链接逃逸。普通 Import 保持 Root-relative；编译器不搜索目录或其他根。
源码库推荐采用稳定的目录布局：

-   标准库：`std/<library>/<library>.bas`
-   第三方源码库：`vendor/<library>/<library>.bas`
-   项目私有子系统：使用项目自己的固定目录前缀。

Loader 应将两类路径先规范化为 Source-Root-relative canonical path，再用于
Module identity。不得把 `./` 字面量保留为独立 Module 身份。

### 6.3 路径安全不变

继续覆盖：

-   `..`、绝对路径和非规范空路径段拒绝；仅允许一个前导 `./` 表示当前
    Module 相对路径；
-   空路径段拒绝；
-   精确大小写；
-   `.bas` 后缀；
-   symlink/reparse point 逃逸拒绝；
-   文件不存在时的准确诊断；
-   Windows 与 POSIX 行为一致；
-   Import path fuzzing。

`EXPORT IMPORT`
不应复制或弱化现有路径验证逻辑。它只改变语义可见性，不改变路径安全模型。

------------------------------------------------------------------------

## 7. 编译器内部设计

### 7.1 Module Loader：识别 Re-export Import

文件：`src/module_graph/module_loader.c`

当前 Loader 在语义分析前扫描 Module 前缀，并识别普通
`IMPORT`。这是为了先加载依赖、构建图，再对所有 Module
进行解析和语义分析。

必须扩展扫描器，识别：

``` basic
IMPORT "vendor/json/parser.bas"
EXPORT IMPORT "./parser.bas"
```

同时确保它不会把以下声明误认为 Import：

``` basic
EXPORT FUNCTION parse(text)
    ...
END FUNCTION
```

要求：

1.  继续只扫描顶层前缀；
2.  `EXPORT IMPORT` 必须是同一声明中的两个关键字；
3.  路径解码与验证继续走现有 helper；
4.  Re-export 和普通 Import 都加入同一 Module Graph；
5.  继续只加载每个规范路径一次；
6.  继续依赖优先拓扑顺序；
7.  循环 Import 检测继续覆盖 Re-export 边；
8.  Loader 与 Parser 对 `EXPORT IMPORT`
    的识别必须有一致性测试，避免扫描器和语法解析器接受不同的源码。

不应为了 Re-export 再建第二个 Module Graph。

### 7.2 Source Import Path Resolver

主要文件：

- `include/ombasic/source/import_path.h`
- `src/source/import_path/import_path.c`
- `include/ombasic/source/file_loader.h`
- `src/source/file_loader/file_loader.c`

建议不要让 `module_loader.c` 自己拼字符串处理 `./`。应把 Import path classification 和 canonicalization 放回 Source capability。

概念 API：

```c
typedef enum Om_Import_Path_Kind {
    OM_IMPORT_PATH_SOURCE_ROOT_RELATIVE = 0,
    OM_IMPORT_PATH_MODULE_RELATIVE
} Om_Import_Path_Kind;
```

并提供等价于：

```text
classify(import_literal)
resolve(importer_canonical_path, import_literal)
→ canonical source-root-relative path
```

的公共契约。

要求：

- `./` 只在 literal 起始位置有效；
- `../` 永远非法；
- canonicalization 不允许逃逸 Source Root；
- 结果不包含 `.`、`..`、重复 `/` 或 `./`；
- 结果继续符合 `lower_snake_case` + `.bas` 规则；
- 相同最终路径必须产生相同 Module identity；
- Windows/POSIX exact casing 仍由现有 File Loader 验证；
- API 使用 checked size arithmetic；
- Import path fuzzer 同时覆盖两种 path kind。

### 7.3 Parser 与 AST

文件：

-   `include/ombasic/syntax/ast.h`
-   `src/syntax/parser/parser.c`

建议：

-   保留现有 `OM_AST_IMPORT` 节点；
-   新增
    `OM_AST_FLAG_REEXPORT`，不要把声明导出和导入重导出混为一个标志；
-   `parse_import` 能区分普通 Import 与 `EXPORT IMPORT`；
-   AST 的 Import 节点保留路径、完整源码 Span 和 Re-export 标记；
-   `EXPORT IMPORT` 仍只能出现在顶层 Import 前缀区；
-   `EXPORT FUNCTION` 等现有语法不改变。

语法错误需要准确定位在 `EXPORT`、`IMPORT`
或路径字面量，而不是只报告整行。

### 7.4 Name Resolver：建立 Module Export Surface

文件：

-   `include/ombasic/semantic/name_resolver.h`
-   `src/semantic/name_resolver/name_resolver.c`

当前 Resolver 会为每个 Import 建立 Module Alias，并在 `module.member`
中查找目标 Module 自己的 Module Scope。这个实现不足以表示 Facade 对其他
Module 的重导出。

新增内部数据结构，表达每个 Module 的最终公开导出面。概念上：

``` c
typedef struct Om_Module_Export {
    Om_Module_Id public_module;
    Om_String_View public_name;
    Om_Symbol_Id origin_symbol;
    Om_Module_Id origin_module;
    Om_Source_Span origin_span;
    Om_Source_Span exposure_span;
    bool reexported;
} Om_Module_Export;
```

这段结构是设计示意，实际字段应与现有 ID、内存所有权和诊断风格一致。

要求：

1.  先创建所有 Module Scope。
2.  预声明所有 Module Alias 和本地声明，避免结果依赖 Module 枚举顺序。
3.  为每个 Module 建立直接公开声明集合。
4.  按 `Om_Module_Project.order` 的依赖优先顺序计算 Re-export surface。
5.  合并每个 `EXPORT IMPORT` 目标 Module 的公开导出面。
6.  对冲突做确定性检测并记录来源 Span。
7.  对相同原始 Symbol ID 去重。
8.  对不同原始 Symbol ID 的同名公开项报错。
9.  普通内部名称解析仍然只使用当前词法 Scope；Re-export
    不会把外部函数变成当前 Module 的无限定名称。
10. `module.member` 查询先处理目标 Module 自己的声明和可见性，再查询它的
    Re-export surface。
11. Re-export 解析最终返回原始 `Om_Symbol_Id`，而不是合成一个假的函数或
    Record 声明。
12. 私有符号诊断仍然有效；Re-export 不得绕过 `E0302`。

### 7.5 公共查询接口

LSP、接口 metadata 和工具不能自行遍历 Resolver 的私有数组来猜测导出面。

应为 `Om_Resolution_Result` 增加导出面查询 API，概念上包括：

``` text
om_resolution_module_export_count(...)
om_resolution_module_export_at(...)
om_resolution_module_export_lookup(...)
```

具体函数签名遵循当前 C API 的 ID、`Om_String_View` 和输出参数约定。

导出面记录必须区分：

-   **公开名称**：调用方通过哪个 Facade 名称访问，例如 `json.parse`；
-   **原始符号**：真正声明 `parse` 的 Module 和 AST 节点；
-   **暴露来源**：通过哪个 `EXPORT IMPORT` 进入 Facade；
-   **静态契约**：原始符号推导出的类型、Effect、Record/Message schema 和
    Actor protocol。

### 7.6 Type Inference、HIR、Specialization 和 MIR

Re-export 不应该创建运行时包装函数或新的 ABI。

要求：

-   `json.parse` 最终绑定到 `parser.bas` 中的原始 `parse` Symbol。
-   参数、返回类型、泛型实例和 Effect 仍从原始声明推导。
-   `json.Json_Value` 解析到原始 Record 声明。
-   `json` 导出的 Actor、Message 和 Record
    使用原始实体身份，避免同一个类型被误认为两个不同类型。
-   HIR
    记录调用位置和公开访问路径需要的来源信息，但不创建无意义的中间调用。
-   MIR 与 LLVM 不应新增专门的 Re-export 指令。
-   Re-export 只影响静态名字可见性和接口导出面，不改变运行时行为。

### 7.7 接口 metadata、Fingerprint 与增量编译

标准第 21.12 节规定了 Module 接口 metadata 的方向。实现 Re-export
时必须检查所有公开 API 消费者。

目标规则：

-   Facade 的接口 metadata 必须包含它最终公开的名字，而不只是 Facade
    自己的直接声明。
-   Re-export 的符号类型、Effect、泛型关系、Record/Message schema、Actor
    protocol 和可序列化信息必须与原始声明一致。
-   导出项的序列化顺序必须确定，不依赖哈希表遍历或文件系统枚举顺序。
-   仅改变内部实现文件、但不改变 Facade 公共契约时，不应无理由地改变
    Facade 的 API fingerprint。
-   公开名称、类型、Effect 或协议发生变化时，相关 fingerprint 必须变化。
-   如果当前 fingerprint 算法将 Module
    源路径作为公开契约的一部分，先明确其语义，再实现
    Re-export；不能在无测试的情况下静默改变分布式 Actor 的兼容性判断。

至少审计：

-   `docs/compiler/` 中的接口契约；
-   `src/tooling/interface_cache.c` 及其调用点；
-   程序/接口 fingerprint 的生成路径；
-   LSP completion、hover、definition 和 diagnostics；
-   分布式 Actor Program/Message contract 相关代码。

### 7.8 Runtime 与 Native Backend

本功能不应该修改 Runtime 的对象布局或 Native ABI。

-   不新增 Runtime Re-export API。
-   不新增 MIR Re-export opcode。
-   不改变 `.omi`。
-   不改变 C ABI 或共享库部署规则。
-   Re-export 的代码仍然由原始 `.bas` Module 编译成 Native 代码。
-   Re-export 的 API 解析完成后，LLVM 后端只处理已有的 HIR/MIR 语义。

------------------------------------------------------------------------

## 8. 标准库设计与分发

### 8.1 四层边界

标准库体系应区分四种职责。

  --------------------------------------------------------------------------------------------------------------------
  层次                    职责                                           示例
  ----------------------- ---------------------------------------------- ---------------------------------------------
  Language Builtin /      语言无法直接表达或启动环境必需的少量语义原语   `Result`、Actor 系统实体、当前的 `print`
  Prelude                                                                

  OMBasic Source Library  普通 `.bas` 模块；优先用 OMBasic 实现          String、Math、List、Map、Text

  System Services         具有明确 Effect 的 OS/外部服务                 Console、File System、Timer、Network

  Native Library          通过明确 C ABI 提供的外部能力                  `.omi`
                                                                         声明、`.a`、`.lib`、`.so`、`.dylib`、`.dll`
  --------------------------------------------------------------------------------------------------------------------

原则：

> **Builtin 是语言语义原语，不是"常用函数"的同义词。**

能够用普通 OMBasic 实现的功能，不应该因为常用就加入 Resolver、Type
Inference、HIR、MIR 和 LLVM 的专用分支。

### 8.2 标准库采用相同 Facade 规则

建议的标准库源码布局：

``` text
std/
├── string/
│   ├── string.bas
│   ├── search.bas
│   └── unicode.bas
├── math/
│   ├── math.bas
│   └── float.bas
├── list/
│   ├── list.bas
│   └── algorithms.bas
├── map/
│   ├── map.bas
│   └── internal.bas
└── text/
    ├── text.bas
    └── utf8.bas
```

例如：

``` basic
IMPORT "std/string/string.bas"

ACTION main()
    cleaned = string.trim(input)
END ACTION
```

每个目录是否需要 Facade 由其公开 API 决定；如果有
`string/string.bas`，则它是该目录的 Facade。

### 8.3 不新增 `@std` 隐式导入根

Import 路径共用唯一 Source Root 作为 Module identity 和安全边界。普通路径从 Source Root 解析；唯一前导 `./` 从当前 Module 目录解析，之后仍规范化到该 Source Root。新增：

``` basic
IMPORT "@std/string/string.bas"
```

并让它解析到 SDK 中另一个目录，就等于增加第二个隐式 Source Root，与现有
ADR-0020 和 ADR-0025 的约束不一致。

因此本计划建议：

-   标准库以源码形式随 SDK 分发；
-   安装后的 SDK 可以在 `share/ombasic/stdlib/` 下提供源码；
-   项目需要使用时，将所需的 `std/` 子树复制或 vendored 到项目 Source
    Root；
-   项目代码仍然写普通路径，例如 `IMPORT "std/string/string.bas"`；
-   编译器不自动搜索 SDK 的 stdlib 目录。

可选的后续便利功能是 `ombasic init --stdlib`，只负责将 SDK
中的标准库源码复制到用户项目，不负责版本求解、联网下载或依赖升级。它不是本次核心编译器功能的前置条件。

### 8.4 第三方源码库的版本管理

不引入编译器理解的包清单。建议采用传统源码管理方式：

-   Git submodule：用仓库和 commit 固定源码版本；
-   Git subtree：把库源码纳入主仓库历史；
-   Vendored source：直接复制源码，并通过主项目 Git commit 固定版本；
-   Source
    archive：解压到约定目录，提交到版本控制或由发布流程固定其来源。

每个被 vendored 的库可以包含普通 `README.md`、`LICENSE`
和来源说明。这些文件只是文档，不是编译器包 metadata。

版本与依赖关系的来源：

-   版本：Git tag/commit、归档版本或主项目的提交；
-   依赖：源码中的 `IMPORT` / `EXPORT IMPORT`；
-   依赖图：编译器从实际 Import 构建；
-   可复现构建：由主项目 Git commit 和工具链/SDK 版本保证。

本计划不添加：

-   `package.toml`；
-   `Package` 关键字或实体；
-   包注册表；
-   自动下载；
-   版本范围求解；
-   lockfile；
-   预编译 OMBasic 模块格式。

### 8.5 Native 库继续单独处理

Native 库继续使用现有 `.omi`、目标 ABI、显式链接配置和 SDK
target/sysroot 规则。

禁止把以下能力放进 `EXPORT IMPORT`：

-   导入 `.omi`；
-   自动查找 `.dll` 或 `.so`；
-   自动发现系统库；
-   自动决定静态或动态链接；
-   自动下载 Native 依赖。

源码库与 Native
库是两种不同的边界，不应因为都叫"库"而统一成同一种解析机制。

### 8.6 `print` 的处理

当前 `print` 已经注册为 `OM_BUILTIN_PRINT`，HIR/MIR lowering
有专门路径，并最终通过 Runtime 输出字符串和换行。

本计划的处理原则：

1.  不把 `print` 的当前特殊实现当作所有标准库函数的模板。
2.  不在 Re-export 功能中改写 `print`。
3.  保持 `print(String) -> Unit` 的现有行为，直到 Console API
    的同步/异步 Effect 边界有独立、明确的设计。
4.  后续可以单独设计 Prelude 的公开声明与内部 Console primitive
    的边界，但不能仅把 `print` 写进 `.bas`
    就宣称它已经不再依赖编译器特殊处理。
5.  Console 的长时间/阻塞工作继续遵循现有 System Actor / Blocking Pool
    设计；短小同步输出是否允许直接调用 Runtime，应由独立 ADR 明确。

------------------------------------------------------------------------


## 9. 已完成的语言标准变更

本方案不是单纯的工程约定；它改变了 OMBasic 0.1 的规范性 Import 语义。以下变更已经写入 `OMBasic-Language-Standard.md`，并由 Conformance 覆盖。

### 9.1 修订 OM-21.02 — 唯一源码根目录

原有含义：

> 所有 `IMPORT` literal 都从 Source Root 解释。

现行规则：

> 每个编译目标仍然只有一个 canonical Source Root。所有 Module identity 都必须规范化为相对于该 Source Root 的 canonical path。Import literal 可以显式选择 Source Root 或当前 Module 目录作为解析起点。

必须明确：

- Source Root 数量仍然是 1；
- `./` 不创建第二个 Source Root；
- `./` 只影响路径解析 base；
- Module identity 始终落回 Source Root。

### 9.2 修订 OM-21.03 — Import 直接使用文件路径

正式定义两种形式：

```basic
IMPORT "storage/database.bas"
IMPORT "./parser.bas"
```

语义分别是：

```text
"storage/database.bas"
    → Source Root relative

"./parser.bas"
    → Current Module Directory relative
```

两种形式都必须包含 `.bas` 扩展名。

### 9.3 修订 OM-21.04 — Import 路径规则

现行规则：

1. Import 必须是带引号的显式 `.bas` 路径。
2. 不以 `./` 开头时，从 Source Root 解析。
3. 以 `./` 开头时，从当前 Module 所在目录解析。
4. `./` 只允许出现在路径开头，并且必须后接至少一个普通路径段。
5. `..` 在任何位置都非法。
6. 不允许绝对路径。
7. 不允许反斜杠路径分隔符。
8. 不允许空段或重复 `/`。
9. 各目录段和文件 stem 继续符合 `lower_snake_case`。
10. 最终结果必须位于 Source Root 内，并转换为唯一 canonical Source-Root-relative path。
11. 编译器不得通过搜索路径、环境变量、工作目录或隐式 fallback 猜测文件。
12. canonical path 的 filesystem casing 必须精确匹配。

规范示例：

```text
"foo/bar.bas"             合法
"./bar.bas"               合法
"./internal/bar.bas"      合法

"../bar.bas"              非法
"./../bar.bas"            非法
"foo/../bar.bas"          非法
"/foo/bar.bas"            非法
"foo\\bar.bas"            非法
"foo/bar"                 非法
```

### 9.4 新增 OM-21.13 — Directory Facade Convention

定义 `X/X.bas` 是目录 `X/` 的约定 Facade Module。

必须明确：

- 这是 convention，不是新的 Package/Namespace 实体；
- Compiler 不自动扫描目录；
- 不存在 `IMPORT "X"` 自动解析 `X/X.bas`；
- Facade 仍然只是普通 `.bas` Module。

### 9.5 新增 OM-21.14 — Current-Module-relative Import Resolution

规范化示例：

```text
current module:
    vendor/json/json.bas

IMPORT "./parser.bas"

resolved canonical Module path:
    vendor/json/parser.bas
```

并明确：

```text
IMPORT "./parser.bas"
```

和：

```text
IMPORT "vendor/json/parser.bas"
```

如果最终指向同一 canonical path，则是同一个 Module identity。

### 9.6 新增 OM-21.15 — Explicit Re-export

定义：

```basic
EXPORT IMPORT "./parser.bas"
```

同时建立依赖与公共 Re-export。

### 9.7 新增 OM-21.16 — Re-export Visibility and Conflicts

定义：

- 只 Re-export 目标 Module 已经公开的声明；
- 私有声明不提升；
- 不导出 Module Alias；
- 不同原始 Symbol 的同名 public name 是 compile error；
- 不存在“后出现者覆盖前出现者”。

### 9.8 新增 OM-21.17 — Facade Public Surface and Canonical Identity

明确：

- Facade 的公共 API 是直接 `EXPORT` + `EXPORT IMPORT` 合并后的确定集合；
- Re-export 不改变原始 Symbol/type/entity identity；
- `./` 路径 literal 不进入 Module identity；
- 公共 metadata、LSP 和 fingerprint 应使用同一导出面。

---

## 10. ADR 与语言标准变更

### 10.1 已接受的 ADR

`docs/architecture/adr/ADR-0026-module-facades-and-explicit-reexports.md`

当前状态：Accepted，见 ADR-0026。

ADR 应记录：

-   Directory Facade 是约定，不是 Package/Module 新实体；
-   `X/X.bas` 的 Facade 命名规则；
-   `EXPORT IMPORT` 的公开 API 语义；
-   Re-export 是平铺而不是子模块 namespace；
-   不自动扫描目录；
-   不自动导出私有声明；
-   冲突报错，不做静默优先级；
-   现有 Source Root 和路径安全规则保持不变；
-   Source Library 和 Native Library 分离；
-   不引入包管理器或二进制 OMBasic module ABI；
-   标准库源码必须放入项目 Source Root 才能通过普通 Import 使用。

ADR-0025 已经接受源码库作为普通目录子树的模型。ADR-0026 应补充
Facade/Re-export 规则，不应把 ADR-0025 改造成包管理方案。

### 10.2 语言标准 Rule ID

现有 Rule ID 不重编号、不复用。第 21 章现有新增规则为：

-   **OM-21.13 --- Directory Facade Convention**
-   **OM-21.14 --- Current-Module-relative Import Resolution**
-   **OM-21.15 --- Explicit Re-export**
-   **OM-21.16 --- Re-export Visibility and Conflicts**
-   **OM-21.17 --- Facade Public Surface and Canonical Identity**

这些规则已经写入 `OMBasic-Language-Standard.md`，不只存在于 ADR
或开发计划中。

标准至少要精确定义：

-   `X/X.bas` 的约定含义；
-   `EXPORT IMPORT` 的语法位置；
-   Alias 和公开成员的解析规则；
-   传递式 Re-export；
-   私有符号不被重导出；
-   名称冲突；
-   循环依赖；
-   路径规则；
-   目录本身不是语言实体；
-   Facade 不自动聚合目录内容。

### 10.3 Conformance

新增的语言行为必须进入：

-   `tests/conformance/06_modules/`；
-   `tests/conformance/manifest.json`；
-   对应的标准 Rule ID；
-   `tests/conformance/README.md`（如有必要）。

`tools/conformance_check/check.py` 应能验证新增 Rule ID
已有正向和负向用例。不要只依靠 C 单元测试来证明语言语义。

------------------------------------------------------------------------

## 11. 分阶段实施计划

### Phase 0 --- 基线确认与设计冻结

**目标：** 在写代码之前确认规则，避免 Parser、Loader 和 Resolver
对语义有不同理解。

任务：

-   [x] 以 `d2fdb646cd6705bfcb61181c0bf25300e9abd8e5` 为源码审计基线。
-   [x] 确认 `0.1.0-rc1` 状态；若已经标记，遵循语言冻结政策。
-   [x] 评审本计划中的目录 Facade 规则。
-   [x] 评审 `EXPORT IMPORT` 的平铺 Re-export 语义。
-   [x] 明确普通 Import 保持 Root-relative，`./` Import 使用当前 Module
    相对路径，并归一到同一 canonical identity。
-   [x] 确认 `@std` 不作为特殊导入根。
-   [x] 确认 `print` 不在本次改动中迁移。
-   [x] 建立 `E0308` 诊断的注册/使用审计。
-   [x] 明确接口 metadata 和 fingerprint 的预期变化。

**退出条件：**

-   ADR-0026 的决定已经明确；
-   新 Rule ID 和 Conformance 设计已评审；
-   不存在未决的路径解释或导出冲突优先级。

### Phase 1 --- ADR、Standard 和 Conformance 先行

**目标：** 先定义可观察语义，再实现。

任务：

-   [x] 新建 ADR-0026。
-   [x] 在 `OMBasic-Language-Standard.md` 更新 OM-21.02 至 OM-21.04，并定义
    OM-21.13 至 OM-21.17。
-   [x] 加入 `EXPORT IMPORT` 的规范示例。
-   [x] 加入私有声明不可重导出的规则。
-   [x] 加入重导出冲突规则。
-   [x] 加入传递式 Re-export 和循环依赖规则。
-   [x] 更新 `tests/conformance/README.md`。
-   [x] 在 manifest 中登记最小正向/负向用例。
-   [x] 运行 `tools/spec_check/check.py`。
-   [x] 运行 `tools/conformance_check/check.py`。

**退出条件：**

-   规范没有歧义；
-   Rule ID 稳定；
-   Conformance manifest 可验证；
-   不存在只有实现、没有规范的语义。

### Phase 2 --- Parser 与 AST

**目标：** 解析 `EXPORT IMPORT`，但不改变现有 `IMPORT` 与
`EXPORT declaration`。

主要文件：

-   `include/ombasic/syntax/ast.h`
-   `src/syntax/parser/parser.c`
-   `tests/syntax/parser_tests.c`
-   `tests/conformance/06_modules/`

任务：

-   [x] 为 Import AST 增加明确的 Re-export 标记，例如
    `OM_AST_FLAG_REEXPORT`。
-   [x] 扩展顶层解析分支，使 `EXPORT IMPORT "path.bas"` 进入 Import
    解析。
-   [x] 确保 `EXPORT FUNCTION` 等现有语法不受影响。
-   [x] 确保 `EXPORT IMPORT` 只能位于 Import 前缀区。
-   [x] 保留路径字面量和完整源码 Span。
-   [x] 测试关键字大小写、缺少路径、缺少引号、尾随 token。
-   [x] 测试声明之后出现 `EXPORT IMPORT` 必须失败。
-   [x] 测试 `EXPORT` 后换行再写 `IMPORT` 不被意外解析为同一声明。

**退出条件：**

-   Parser 能稳定识别两种 Import；
-   AST 不需要通过字符串重新推断 Re-export；
-   原有语言用例无回归。

### Phase 3 --- Module Loader 与依赖图

**目标：** 在语义解析前正确加载 Re-export 的目标 Module。

主要文件：

-   `src/module_graph/module_loader.c`
-   `include/ombasic/module_graph/module_loader.h`（若查询契约确需扩展）
-   `tests/module_graph/module_loader_tests.c`
-   `tests/module_graph/module_graph_tests.c`
-   `tests/fuzz/import_path_fuzzer.c`

任务：

-   [x] 扩展前缀扫描器以识别 `IMPORT` 和 `EXPORT IMPORT`。
-   [x] 普通 `EXPORT FUNCTION` 必须结束 Import 前缀扫描，而不是误读成
    Re-export。
-   [x] 继续调用同一 Import path decoder 和 validator。
-   [x] Re-export 目标加入现有 Module Graph。
-   [x] 保持同一路径只加载一次。
-   [x] 保持依赖优先拓扑顺序。
-   [x] 保持循环链诊断，包括由 `EXPORT IMPORT` 引入的循环。
-   [x] 增加 Scanner 与 Parser 语法识别一致性的测试。
-   [x] 对恶意路径、超长路径、错误转义和路径逃逸执行现有安全测试。

**退出条件：**

-   `EXPORT IMPORT` 的目标一定会被加载；
-   Loader 和 Parser 不存在接受集合差异；
-   原有路径安全测试全部通过；
-   不新增搜索路径或第二 Source Root。

### Phase 4 --- Resolver 导出面

**目标：** 建立每个 Module 的公开导出表，并实现平铺 Re-export。

主要文件：

-   `include/ombasic/semantic/name_resolver.h`
-   `src/semantic/name_resolver/name_resolver.c`
-   `src/tooling/check.c`
-   `tests/semantic/name_resolver_tests.c`
-   `tests/semantic/fixtures/`

任务：

-   [x] 为每个 Module 记录直接导出声明。
-   [x] 在所有 Module 的直接声明完成预声明后，再计算 Re-export surface。
-   [x] 使用依赖优先的拓扑顺序计算传递式导出。
-   [x] 保留原始 Symbol ID 和声明 Span。
-   [x] 在 Resolver 结果中暴露导出面查询 API。
-   [x] 将 `module.member` 查询扩展为直接公开声明或 Re-export surface
    查询。
-   [x] 保持普通无限定名称只在本地 Scope 中查找。
-   [x] 保持 `E0302` 私有符号访问诊断。
-   [x] 对不同来源的同名导出生成 `E0308`。
-   [x] 在诊断中标注两处来源。
-   [x] 对同一原始 Symbol ID 的重复暴露去重。
-   [x] 不生成合成函数或合成类型。
-   [x] 测试本地导出与 Re-export 冲突。
-   [x] 测试不同 Module 同名导出冲突。
-   [x] 测试多层 Facade 的传递式 Re-export。
-   [x] 测试 Module Graph 顺序变化不会改变解析结果。

**退出条件：**

-   公开导出面唯一且确定；
-   冲突都显式报错；
-   Resolver 结果不依赖文件系统枚举顺序；
-   原有普通 Import 解析行为保持稳定。

### Phase 5 --- 类型系统、HIR、接口 metadata 与工具

**目标：** 确保 Re-export
不仅能通过名字解析，也能完整参与类型系统、编辑器和接口契约。

主要文件/区域：

-   `src/semantic/type_inference/`
-   `src/semantic/semantic_analysis/`
-   `src/ir/hir/`
-   `src/ir/specialization/`
-   `src/ir/mir/`
-   `src/tooling/interface_cache.c`
-   `tools/ombasic_lsp.py`

任务：

-   [x] 验证通过 Facade 调用原始 Function。
-   [x] 验证泛型函数 Re-export 后仍正确推导并 Specialize。
-   [x] 验证 Record 类型可通过 Facade 访问并构造。
-   [x] 验证 Message、Actor 和 Action 的公开访问规则。
-   [x] 验证 Effect 信息来自原始声明。
-   [x] 验证 Re-export 不创建额外 MIR 指令。
-   [x] 验证接口 metadata 使用 Facade 的公开名称，同时保留原始定义位置。
-   [x] 验证 API fingerprint 对公开契约变化敏感。
-   [x] 验证只改变私有实现文件不造成不必要的公开 API fingerprint 变化。
-   [x] 验证 LSP completion 在 `json.` 后显示 Facade 导出的成员。
-   [x] 验证 hover 展示正确类型、Effect 和定义位置。
-   [x] 验证 go-to-definition 跳转到原始声明。
-   [x] 验证 Re-export 冲突诊断的源码位置。
-   [x] 验证跨模块泛型、Actor 协议和序列化规则不因 Re-export 被绕过。

**退出条件：**

-   Facade 的静态行为与直接引用原始 Module 的静态行为一致；
-   LSP 和 metadata 能查询完整导出面；
-   fingerprint 的输入和输出契约有测试；
-   不需要 Runtime 或 LLVM 特殊支持。

### Phase 6 --- 端到端 Conformance 与跨平台验证

**目标：** 从语言规则到 Native executable 完整验证。

任务：

-   [x] 完成第 12 节所列 Conformance 正向和负向矩阵。
-   [x] 运行 CTest 全套。
-   [x] 运行 `tools/spec_check/check.py`。
-   [x] 运行 `tools/conformance_check/check.py`。
-   [x] 运行 architecture checker。
-   [x] 运行 Linux Native AOT 构建和执行测试。
-   [x] 运行 Windows `clang-cl` / LLD 构建和执行测试。
-   [x] 运行 sanitizer 与 fuzz smoke tests。
-   [x] 检查 Release Candidate 的 Standard/Conformance 覆盖报告。

**退出条件：**

-   所有支持平台通过；
-   规范覆盖完整；
-   Re-export 没有破坏 Native AOT、Actor、Runtime ABI 或分布式协议；
-   Release Gate 报告通过。

### Phase 7 --- 标准库源码布局和交付

**目标：** 用真实的多文件源码库验证 Facade
模型，并建立最小标准库源码分发方式。

建议增加的源码布局：

``` text
std/
├── string/
│   ├── string.bas
│   └── ...
├── math/
│   ├── math.bas
│   └── ...
└── text/
    ├── text.bas
    └── ...
```

任务：

-   [x] 先建立一个最小、可编译的示例库，验证 Facade/Re-export。
-   [x] 确定标准库公开 API 后再逐个增加模块，不先承诺尚未实现的函数。
-   [x] SDK 将标准库源码安装到 `share/ombasic/stdlib/`。
-   [x] 更新 `docs/compiler/resource_layout.md`。
-   [x] 更新 SDK 安装测试，确认 stdlib 源码在安装后仍存在。
-   [x] 文档明确说明：使用标准库时把 `std/` 子树复制或 vendored 到项目
    Source Root。
-   [x] 不让 Compiler 隐式搜索 SDK 的 stdlib 目录。
-   [ ] 可选：添加 `ombasic init --stdlib`，只复制 SDK
    源码到项目，不做依赖解析或更新。
-   [x] 为第三方源码库写出 Git submodule/subtree/vendor 的推荐流程。
-   [x] 明确标准库版本与 SDK release 对应；项目复制后的版本由项目 Git
    commit 固定。

**退出条件：**

-   标准库是普通 `.bas` 源码；
-   公开入口遵循目录同名 Facade 规则；
-   用户可以通过普通 `IMPORT` 使用；
-   不引入新的导入根、包清单或依赖解析器。

### Phase 8 --- 后续独立事项：Console 与 `print`

**本阶段不属于 Facade/Re-export 的阻塞项。**

后续可另开 ADR 研究：

-   当前 `print` builtin 的保留期限；
-   `Console.write` / `Console.write_line` 的 Effect 契约；
-   同步输出与 System Actor 消息接口之间的边界；
-   Prelude 声明如何映射到一个内部 Runtime primitive；
-   是否将 `print` 变成标准 Prelude 声明。

在没有明确 Console 语义前，不应让本次源码库改造顺带重写 `print`。

------------------------------------------------------------------------

## 12. 测试矩阵

### 12.1 Parser 测试

  场景                             预期
  -------------------------------- ---------------------------------
  普通 `IMPORT "x/x.bas"`          解析成功
  `EXPORT IMPORT "x/x.bas"`        解析成功，AST 有 Re-export 标记
  `EXPORT FUNCTION f()`            仍按普通导出声明解析
  `EXPORT` 后换行再写 `IMPORT`     不当作 `EXPORT IMPORT`
  `EXPORT IMPORT` 缺少路径         语法错误
  `EXPORT IMPORT "x/x"`            Import path 规则拒绝
  声明后出现 `EXPORT IMPORT`       顶层顺序错误
  `EXPORT IMPORT` 后带额外 token   语法错误

### 12.2 Module Loader / Graph 测试

  场景                                        预期
  ------------------------------------------- --------------------------
  普通 Import                                 目标 Module 加入图
  Re-export Import                            目标 Module 加入同一张图
  两处 Import 指向同一路径                    只加载一个 Module
  Re-export 链 A → B → C                      拓扑顺序正确
  Re-export 引入循环                          循环错误包含完整路径链
  `EXPORT FUNCTION` 出现在前缀扫描位置        不误判成 Import
  路径大小写错误                              失败
  路径逃逸 Source Root                        失败
  symlink/reparse point 逃逸                  失败
  Loader 与 Parser 对 Import 前缀认知不一致   测试失败

### 12.3 Resolver 正向测试

至少包括：

-   Facade 直接导出函数；
-   Facade 通过 `EXPORT IMPORT` 导出函数；
-   Facade 通过 Re-export 导出 Record；
-   Facade 通过 Re-export 导出 Message；
-   Facade 通过 Re-export 导出 Actor；
-   多层 Facade 传递式 Re-export；
-   泛型函数 Re-export；
-   同一个原始符号通过多个路径到达后去重；
-   内部普通 Import 的 Alias 仍可在 Facade 内部使用；
-   Re-export 不让子模块 Alias 自动成为 Facade 的公共成员；
-   `IMPORT "vendor/json/json.bas"` 的 Alias 是 `json`；
-   `json.parse` 最终解析到原始 `parser.bas` Symbol。

### 12.4 Resolver 负向测试

至少包括：

-   重导出目标不存在；
-   重导出目标是非法路径；
-   访问目标 Module 的私有声明；
-   两个不同来源导出同名函数；
-   本地 `EXPORT FUNCTION` 与 Re-export 名冲突；
-   不同 Re-export 链最终暴露不同来源的同名符号；
-   Re-export 循环；
-   试图省略 `.bas`；
-   试图用 `IMPORT "vendor/json"` 导入目录；
-   把 `.omi` 当作源码 Import；
-   两个路径的文件名 stem 相同导致 Module Alias 冲突；
-   Facade 未 Re-export 某个内部模块时，调用方访问该成员失败。

### 12.5 类型和接口测试

-   公开 Function 的参数/返回类型正确；
-   泛型实例化结果与直接导入原始 Module 一致；
-   Record 构造和字段访问正确；
-   Actor/Message 协议检查不能被 Re-export 绕过；
-   Effect 检查使用原始声明的 Effect；
-   接口 metadata 包含 Facade 对外公开的名称；
-   fingerprint 对公开 API 改动发生变化；
-   只改内部私有实现不造成不必要的公开 API fingerprint 变化；
-   LSP completion、hover、definition 和诊断位置正确。

### 12.6 端到端测试

增加真实目录：

``` text
tests/conformance/06_modules/facade/
├── main.bas
└── vendor/
    └── json/
        ├── json.bas
        ├── value.bas
        └── parser.bas
```

成功用例应当：

1.  只 Import `vendor/json/json.bas`；
2.  使用 `json.parse`；
3.  使用 `json` Facade 暴露的 Record；
4.  完成 Native AOT 编译；
5.  执行程序并验证结果。

同时添加冲突版本和私有符号的负向 fixture。

### 12.7 Conformance Rule 覆盖

每条新增 Rule ID 至少需要：

-   一个正向用例；
-   一个负向用例；
-   对路径/可见性等边界规则的专项用例；
-   在 `tests/conformance/manifest.json` 中显式列出 Rule ID；
-   稳定诊断码和主要 Span 断言。

------------------------------------------------------------------------

## 13. 建议的诊断契约

已有诊断语义不能被重新利用为不同含义：

-   `E0301`：未定义名称；
-   `E0302`：访问非公开声明；
-   `E0303`：重复声明；
-   `E0304`：仅大小写不同的名称冲突；
-   `E0305`：遮蔽外层或 Prelude 名称；
-   `E0306`：Module Alias 无效或冲突；
-   `E0307`：给不可变绑定赋值。

建议新增：

-   `E0308`：Re-export 导出面存在同名冲突。

`E0308` 应包含：

-   主位置：发生冲突的 Facade 声明或 Re-export；
-   相关位置：另一公开声明的来源；
-   信息：冲突的公开名称；
-   可选上下文：两条 Re-export 路径。

路径解析失败继续使用现有 Loader/Source 诊断机制，不应把文件不存在误报为
`E0308`。

诊断码加入前必须检查当前所有诊断 enum、CLI 映射、JSON
diagnostics、Conformance fixture 和文档，确保 `E0308` 未被占用。

------------------------------------------------------------------------

## 14. 文件变更清单

### 14.1 已修改

  ------------------------------------------------------------------------------------------------------------------
  文件/目录                                                                      变更
  ------------------------------------------------------------------------------ -----------------------------------
  `OMBasic-Language-Standard.md`                                                 修订 OM-21.02 至 OM-21.04，定义 OM-21.13 至 OM-21.17

  `docs/architecture/adr/ADR-0026-module-facades-and-explicit-reexports.md`      已接受的架构决策

  `include/ombasic/syntax/ast.h`                                                 增加 Re-export AST flag

  `src/syntax/parser/parser.c`                                                   解析 `EXPORT IMPORT`

  `src/module_graph/module_loader.c`                                             扫描并加载 Re-export 依赖

  `include/ombasic/semantic/name_resolver.h`                                     导出面查询 API 和结果结构

  `src/semantic/name_resolver/name_resolver.c`                                   计算导出面、冲突和公开成员查找

  `src/tooling/check.c`                                                          新增语义诊断文本/映射

  `tests/conformance/manifest.json`                                              增加新 Rule ID 用例

  `tests/conformance/06_modules/`                                                新增 Facade 正向/负向案例

  `tests/module_graph/`                                                          新增 Loader 和 Graph 用例

  `tests/semantic/name_resolver_tests.c`                                         新增 Re-export 测试

  `docs/README.md`                                                               加入新 ADR/设计文档索引（如适用）
  ------------------------------------------------------------------------------------------------------------------

### 14.2 已审计并按需修改

  ------------------------------------------------------------------------
  文件/区域                            审计内容
  ------------------------------------ -----------------------------------
  `src/semantic/type_inference/`       确保原始符号类型可用于 Re-export

  `src/semantic/semantic_analysis/`    确保公开契约和 Effect 检查完整

  `src/ir/hir/`                        保留原始 Symbol 身份和来源

  `src/ir/specialization/`             泛型实例化不重复或丢失

  `src/ir/mir/`                        不生成无意义的 Re-export 调用

  `src/tooling/interface_cache.c`      导出面和 fingerprint 的缓存键

  `tools/ombasic_lsp.py`               completion、hover、definition

  `docs/compiler/`                     Module 接口与 fingerprint 文档

  `docs/compiler/resource_layout.md`   SDK 标准库源码安装布局

  CMake SDK install rules / SDK        标准库源码是否被正确安装
  install tests                        
  ------------------------------------------------------------------------

### 14.3 保持未修改的区域

除非审计发现真实缺陷，本计划不应改动：

-   Runtime 对象布局；
-   GC 和 Shadow Root 规则；
-   Actor Scheduler；
-   分布式传输协议；
-   Native C ABI；
-   `.omi` 格式；
-   LLVM 目标选择；
-   Native linking；
-   `print` 的 Runtime 行为；
-   现有 Source Root 路径安全规则。

------------------------------------------------------------------------

## 15. 建议提交顺序

建议每个提交都能独立审查，且避免先引入无法通过的语言行为。

1.  `docs: record directory facade and re-export decision`
    -   ADR-0026。
2.  `spec: define directory facades and export imports`
    -   标准新增 Rule ID、规范示例。
3.  `test: add facade and re-export conformance cases`
    -   Conformance manifest 和 fixtures。
4.  `parser: parse export import declarations`
    -   AST flag、Parser 测试。
5.  `module-graph: load export import dependencies`
    -   Loader、Graph、路径安全测试。
6.  `semantic: build module export surfaces`
    -   Resolver 导出面和 `E0308`。
7.  `semantic: resolve facade re-exports`
    -   Type Inference/HIR 的来源和公开名处理。
8.  `tooling: expose re-exported members`
    -   LSP、metadata、fingerprint 和 completion。
9.  `test: cover native facade builds and diagnostics`
    -   Native AOT 端到端与跨平台测试。
11. `stdlib: add source library layout and SDK install`
    -   标准库源码树、安装布局和文档。
12. `docs: document source library versioning and vendoring`
    -   Git、subtree、submodule、源码归档使用方式。

如果某一提交引入了规范行为，必须在同一 PR 中更新 Standard 和
Conformance；提交顺序不能成为绕过规范要求的理由。

------------------------------------------------------------------------

## 16. Definition of Done

只有同时满足以下条件，才算完成。

### 语言与规范

-   [x] ADR-0026 已 Accepted。
-   [x] 标准新增的 Rule ID 不重编号、不复用。
-   [x] `EXPORT IMPORT` 的语法和语义无歧义。
-   [x] Facade 约定明确说明目录不是语言实体。
-   [x] 标准明确禁止自动扫描目录和自动导出目录成员。
-   [x] 普通 `IMPORT` 的既有行为没有意外变化。

### Loader 与 Resolver

-   [x] Loader 能加载 `EXPORT IMPORT` 的依赖。
-   [x] Loader 与 Parser 对 Import 前缀的认知一致。
-   [x] Re-export 使用同一 Module Graph。
-   [x] 循环依赖仍在编译期被拒绝。
-   [x] 每个 Module 的公开导出面确定且唯一。
-   [x] 私有声明不会被 Re-export。
-   [x] 冲突会给出准确诊断和相关位置。
-   [x] 不创建合成运行时函数或额外 MIR 指令。

### 类型系统与工具

-   [x] Function、Action、Record、Message、Actor 和泛型 Re-export
    通过测试。
-   [x] Effect、Actor protocol 和序列化约束保持有效。
-   [x] Interface metadata 与 Facade 的公开 API 一致。
-   [x] Fingerprint 有稳定性测试。
-   [x] LSP completion、hover 和 go-to-definition 正确。

### 分发与标准库

-   [x] 标准库使用普通 `.bas` 源码和 Facade 约定。
-   [x] SDK 可以交付标准库源码。
-   [x] 使用标准库不要求编译器搜索第二个隐式 Source Root。
-   [x] 第三方源码库可以通过 Git/subtree/submodule/vendor 方式固定版本。
-   [x] 不引入包管理器、Package 关键字、包清单或锁文件。
-   [x] Native 库继续使用 `.omi` 和显式链接配置。

### 工程质量

-   [x] CTest 全套通过。
-   [x] `tools/spec_check/check.py` 通过。
-   [x] `tools/conformance_check/check.py` 通过。
-   [x] Architecture checker 通过。
-   [x] Linux Native AOT 端到端测试通过。
-   [x] Windows Native AOT 端到端测试通过。
-   [x] Sanitizer 和 Fuzz smoke tests 通过。
-   [x] Conformance Rule coverage 报告没有缺口。
-   [ ] Release Candidate gate 通过。

------------------------------------------------------------------------

## 17. 风险与缓解措施

### 风险 1：Loader 和 Parser 的语法识别不一致

**原因：** 当前 Module Loader 在 Parser 之前扫描
Import，存在独立的前缀扫描逻辑。

**缓解：**

-   两者共享相同的 Import 路径验证；
-   明确 `EXPORT IMPORT` 的同一行语法；
-   加入 Loader/Parser parity 测试；
-   保持前缀扫描逻辑小而确定。

### 风险 2：Re-export 冲突隐藏在传递依赖中

**原因：** Facade 的公开名称可能来自多个深层 Module。

**缓解：**

-   以原始 Symbol ID 作为身份；
-   以确定的拓扑顺序构建导出面；
-   保存 Re-export 来源链；
-   发现不同原始 Symbol 的同名冲突立即报错；
-   不按 Import 顺序覆盖。

### 风险 3：Resolver 结果与 LSP 看到的 API 不一致

**原因：** 原始符号属于内部 Module，而 Facade 的公开名称属于另一
Module。

**缓解：**

-   将公开导出面作为显式数据结构；
-   提供公共查询 API；
-   LSP、metadata 和 fingerprint 都使用同一导出面；
-   不要求各消费者自行递归扫描 AST。

### 风险 4：公共 API fingerprint 被内部目录结构变化影响

**原因：** Re-export 的原始声明可能移动到另一个内部文件。

**缓解：**

-   区分公开名称、原始声明位置和接口契约；
-   明确 fingerprint 对哪些内容敏感；
-   对仅改变实现位置的情况增加测试；
-   对公开类型、Effect、协议或名字变化增加正向变化测试。

### 风险 5：两种 Import 解析起点导致 identity 混乱

**原因：** 同一文件可能被 Root-relative 和 `./` 两种 literal 引用。

**缓解：**

- 两种 literal 都先归一到 Source-Root-relative canonical path；
- Module Graph 只以 canonical path 建立 identity；
- `./` 不进入 Module identity；
- Root-relative 与 `./` 指向同一文件必须复用同一 `Om_Module_Id`；
- 禁止 `..` 和隐式搜索；
- 为同文件双路径引用增加 Source、Module Graph 和 Conformance 测试。

### 风险 6：语言冻结期间引入新语法

**原因：** `EXPORT IMPORT` 是语义变更，不是纯实现重构。

**缓解：**

-   在 `0.1.0-rc1` 之前完成规范和测试；
-   如果已经进入 RC，遵循冻结政策并重新启动候选周期；
-   不允许只更新实现或只更新标准。

------------------------------------------------------------------------

## 18. 关键决策总结

本计划最终选择：

-   **Module：** 仍然是一个 `.bas` 文件。
-   **目录：** 只是文件组织方式。
-   **Facade：** `X/X.bas` 是目录 `X/` 的约定公共入口。
-   **Import：** 使用 Source-Root-relative 或显式前导 `./` 的
    Current-Module-relative `.bas` literal；两者归一到同一 canonical identity。
-   **Alias：** 由文件名 stem 决定。
-   **Export：** 由声明上的 `EXPORT` 决定。
-   **Re-export：** 用 `EXPORT IMPORT` 显式平铺导出目标 Module
    的公开符号。
-   **目录聚合：** 不自动扫描、不自动导出。
-   **版本：** 由 Git commit/tag、源码归档和主项目版本控制管理。
-   **标准库：** 普通 `.bas` 源码，遵循同一 Facade 规则。
-   **SDK stdlib：** 可以随 SDK 分发，但必须复制或 vendored 到 Source
    Root 才能通过普通 Import 使用。
-   **Native 库：** `.omi` + 显式链接配置，与源码 Import 分离。
-   **`print`：** 本次保持当前 builtin/Runtime 特殊路径，后续另行设计。
-   **包管理器：** 不引入。

最终认知保持为：

``` text
文件决定 Module
目录组织源码
同名 .bas 约定 Facade
Root-relative IMPORT 引用源码树位置
./ IMPORT 保持库内部可搬运
canonical path 决定 Module identity
EXPORT 声明公开 API
EXPORT IMPORT 显式组合公开 API
Git 管理源码版本
Native ABI 管理外部二进制边界
```

------------------------------------------------------------------------

## 19. 源码参考

以下链接指向本计划分析时的 `master` 文件；实施前应再次确认目标提交。

-   Repository: https://github.com/risol/OMBasic
-   Baseline commit:
    https://github.com/risol/OMBasic/commit/d2fdb646cd6705bfcb61181c0bf25300e9abd8e5
-   Language Standard:
    https://github.com/risol/OMBasic/blob/master/OMBasic-Language-Standard.md
-   ADR-0020 --- Strict source tree paths:
    https://github.com/risol/OMBasic/blob/master/docs/architecture/adr/ADR-0020-strict-source-tree-paths.md
-   ADR-0025 --- Source and native library distribution:
    https://github.com/risol/OMBasic/blob/master/docs/architecture/adr/ADR-0025-source-and-native-library-distribution.md
-   Language freeze policy:
    https://github.com/risol/OMBasic/blob/master/docs/development/language_freeze_policy.md
-   Import path validator:
    https://github.com/risol/OMBasic/blob/master/src/source/import_path/import_path.c
-   Source file loader:
    https://github.com/risol/OMBasic/blob/master/src/source/file_loader/file_loader.c
-   Module loader:
    https://github.com/risol/OMBasic/blob/master/src/module_graph/module_loader.c
-   Module graph:
    https://github.com/risol/OMBasic/blob/master/src/module_graph/module_graph.c
-   Parser:
    https://github.com/risol/OMBasic/blob/master/src/syntax/parser/parser.c
-   AST definitions:
    https://github.com/risol/OMBasic/blob/master/include/ombasic/syntax/ast.h
-   Name Resolver:
    https://github.com/risol/OMBasic/blob/master/src/semantic/name_resolver/name_resolver.c
-   Name Resolver API:
    https://github.com/risol/OMBasic/blob/master/include/ombasic/semantic/name_resolver.h
-   Conformance manifest:
    https://github.com/risol/OMBasic/blob/master/tests/conformance/manifest.json
-   SDK resource layout:
    https://github.com/risol/OMBasic/blob/master/docs/compiler/resource_layout.md
-   Interface cache:
    https://github.com/risol/OMBasic/blob/master/src/tooling/interface_cache.c
