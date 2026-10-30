# OMBasic 下一阶段基础库完整开发计划

> **文档类型：** Standard Library / Runtime Foundation Roadmap  
> **版本：** 1.0-draft  
> **日期：** 2026-10-30  
> **适用分支：** `master`  
> **项目：** OMBasic  
> **目标：** 在现有标准库基础上，补齐真实应用开发所需的公共地基，并保持 Actor 为唯一用户级异步模型。

---

## 1. 执行摘要

OMBasic 当前已经拥有较完整的第一层标准库与系统服务基础：

- Math
- Text
- Path
- JSON
- Collections
- Console
- File System
- Time
- Random
- Encoding / Buffer / Hex / Base64
- Network TCP / Connected UDP
- Logging
- Process

同时，标准库已经明确采用以下执行模型：

```text
FUNCTION = 纯同步计算
ACTION   = 显式同步副作用
ACTOR    = 并发、长期状态、异步系统服务
MESSAGE  = 异步请求、通知与结果传递
```

OMBasic 不引入：

```text
async / await
Future / Promise
Coroutine / Task
Callback-based async API
用户级 event loop
隐式 continuation
隐藏挂起 / 恢复
```

因此，本阶段的重点不再是横向增加大量模块，而是补齐后续 HTTP、数据库、CLI、配置、文件处理和网络程序都会重复依赖的底层公共能力。

### 1.1 本计划的核心优先级

推荐实施顺序：

1. `number` / `convert`
2. UTF-8 `String <-> Buffer`
3. Binary File I/O
4. `uri`
5. `hash`
6. `environment` / `platform`
7. typed network address model
8. DNS Actor
9. TLS
10. HTTP/1.1 Client Actor
11. Collections typed-empty-result 前置能力
12. CSV / UUID / Compression 等第二梯队基础库

### 1.2 为什么不是先继续做更多大型 Actor

当前 OMBasic 已经存在：

- File System Actor
- Timer Actor
- Clock Actor
- Console Actor
- Secure Random Actor
- Logger Actor
- Network Actor
- Process Actor

继续增加大型系统 Actor 的边际收益已经下降。

真正缺少的是多个模块之间的共享数据模型：

```text
String <-> Number
String <-> Buffer
Buffer <-> File
URI
Hash
Environment
IP Address
Socket Address
DNS Result
TLS Transport
HTTP Request / Response
```

如果这些公共地基不先稳定，HTTP、TLS、数据库和配置层会各自定义转换、错误和资源语义，最终造成 API 分裂。

---

## 2. 当前基线与缺口

### 2.1 已经具备的能力

当前 SDK 已经具备：

#### Pure / in-memory

- Math integer helpers
- Text Unicode scalar operations
- Path logical-path operations
- JSON parse / query / stringify
- Collections basic List / Map helpers
- Deterministic PRNG
- Buffer
- Hex
- Base64
- Logging event formatting
- Time Duration / Instant / DateTime conversion helpers

#### Actor / system service

- Console output / line input
- File text read / write / append
- File exists / delete / stat
- Directory create / list
- Timer delivery
- Clock monotonic / UTC reads
- Secure random byte generation
- Logging sink
- TCP
- Connected UDP
- External Process lifecycle and streams

### 2.2 当前最重要的缺口

#### 缺口 A — Numeric conversion

尚缺统一的：

```text
String -> Int
String -> Float
String -> Bool
Int -> String
Float -> String
radix parsing / formatting
```

目前不同模块若需要数字转换，会被迫：

- 自己解析；
- 通过 JSON 绕路；
- 依赖 Native；
- 或在 API 中继续保留 String。

这是最应该优先解决的纯基础库能力。

#### 缺口 B — Buffer 与 String 没有完整 UTF-8 桥梁

当前 Buffer 已经存在，但仍缺标准化：

```text
utf8_encode(String) -> Buffer
utf8_decode(Buffer) -> Result<String, ...>
```

这会影响：

- Binary File I/O
- HTTP body
- Network payload
- Process streams
- Hash input
- Compression
- 数据库 Blob
- TLS plaintext/ciphertext boundary

#### 缺口 C — File System 没有 Binary I/O

当前 File System 是 UTF-8 文本 API。

需要新增：

```text
File_Read_Bytes
File_Write_Bytes
File_Append_Bytes
```

并返回 / 接受 Buffer 边界。

#### 缺口 D — URI / URL

后续 HTTP、REST、WebSocket、OAuth、Package Registry、Webhook、Remote API 都需要统一 URI 模型。

#### 缺口 E — Hash / checksum

SHA-256 会被：

- 文件校验
- SDK / cache
- HTTP
- TLS 周边
- 内容寻址
- 构建缓存
- 下载完整性检查
- API 签名

重复依赖。

#### 缺口 F — 当前进程环境

当前已经可以控制外部 Process，但缺少当前程序自身的：

```text
arguments
environment
working directory
executable path
platform
architecture
```

#### 缺口 G — Network 应用层基础

TCP / UDP 已经存在，但还缺：

- typed IP Address
- typed Socket Address
- DNS
- TLS
- HTTP

---

## 3. 不可违反的架构原则

### 3.1 Actor-only async

所有外部等待操作必须通过 Actor + Message 暴露。

例如文件读取：

```basic
SEND File_System, File_Read_Bytes("image.bin", SELF)
```

完成：

```basic
RECEIVE File_Read_Bytes_Completed(outcome)
    SELECT outcome
    CASE Ok(buffer)
        ...
    CASE Error(problem)
        ...
    END SELECT
END RECEIVE
```

不得增加：

```basic
buffer = file.read_bytes("image.bin")
buffer = await file.read_bytes("image.bin")
future = file.read_async("image.bin")
file.read("image.bin", callback)
```

### 3.2 Pure API 优先

只要可以安全表达为内存纯计算，应优先使用普通 `.bas` `FUNCTION`：

```text
number
uri
hash facade
encoding
csv
uuid formatting
collections
```

只有系统资源、I/O 或需要平台能力的部分才进入 Runtime / Native。

### 3.3 Native 不等于 Runtime builtin

新增 Native 实现不意味着必须新增 Compiler Builtin。

推荐顺序：

```text
.bas
  ↓
module-scoped .omi
  ↓
Native implementation
```

只有真正具有语言 / Runtime 调度语义的能力才进入 Builtin Registry。

### 3.4 String 与 Buffer 严格分离

```text
String = 合法 UTF-8 文本
Buffer = 任意字节
```

禁止：

- 使用 String 模拟 binary blob；
- 将无效 UTF-8 塞入 String；
- 为了网络方便而让 String 失去 UTF-8 保证。

### 3.5 资源 ID 不泄漏底层句柄

Network / Process / TLS 等 Runtime 资源继续使用：

```text
opaque resource ID
```

不得将：

```text
SOCKET
HANDLE
fd
SSL*
FILE*
```

暴露成普通 OMBasic 可任意操作的值。

---

## 4. 总体依赖图

```text
                  +----------------+
                  |     Number     |
                  +--------+-------+
                           |
            +--------------+---------------+
            |                              |
         JSON                           Environment
            |                              |
            +------------------------------+

String
  |
  v
UTF-8 Conversion
  |
  v
Buffer ---------> Hash
  |
  +-----------> Binary File I/O
  |
  +-----------> Process Streams
  |
  +-----------> Network
                  |
                  +--> Typed IP/Socket Address
                  |
                  +--> DNS
                  |
                  +--> TLS
                         |
                         +--> HTTP/1.1
                                 |
                                 +--> REST clients
                                 +--> Package / Registry
                                 +--> WebSocket later

URI ---------------------------------------> HTTP

Secure_Random -------> UUID

Buffer + Hash -------> Compression / Archive
```

---

## 5. Phase 0 — API 与错误模型收敛

### 5.1 目标

在新增模块前，先冻结共同约定。

### 5.2 任务

- [ ] 定义 Parse Error 通用策略；
- [ ] 定义 byte offset / scalar index 的使用边界；
- [ ] 明确所有 public error code 必须是稳定 ASCII identifier；
- [ ] 明确 Native error 与 Runtime Fault 区别；
- [ ] 明确 binary API 使用 Buffer；
- [ ] 明确 Network / Process / File resource limit 文档格式；
- [ ] 更新 `docs/stdlib/api_review_checklist.md`；
- [ ] 明确所有异步 API 都必须存在 Actor Protocol 文档；
- [ ] 明确所有资源服务在 Runtime Shutdown 下的行为。

### 5.3 推荐错误码风格

统一使用：

```text
lower_snake_case
```

例如：

```text
invalid_argument
invalid_format
out_of_range
resource_exhausted
not_found
permission_denied
io_error
unsupported
closed
end_of_stream
```

错误 detail 可以不同，但用于业务分支的 `code` 必须稳定。

---

## 6. Phase 1 — Number / Convert

**优先级：P0**

### 6.1 模块布局

建议：

```text
stdlib/
└── number/
    ├── number.bas
    ├── integer.bas
    ├── float.bas
    └── parse_error.bas
```

公共入口：

```basic
IMPORT "@std/number/number.bas"
```

### 6.2 初始 API

#### Integer

```text
parse_int(text)
parse_int_radix(text, radix)

format_int(value)
format_int_radix(value, radix)
```

返回：

```text
parse_int -> Result<Int, Number_Error>
```

#### Float

```text
parse_float(text)
format_float(value)
```

#### Bool

可以放入单独 `convert`，或者在 `number` 之外：

```text
parse_bool(text)
format_bool(value)
```

更推荐：

```text
@std/convert/convert.bas
```

但如果不希望模块碎片化，v1 可先放入 number facade 的基础转换层。

### 6.3 Number_Error

建议：

```basic
RECORD Number_Error(code, byte_offset, detail)
```

稳定 code：

```text
empty
invalid_digit
invalid_format
invalid_radix
out_of_range
non_finite
```

### 6.4 Int 规则

- Int = signed Int64；
- 十进制默认；
- radix 支持至少：

```text
2
8
10
16
```

可考虑完整：

```text
2..36
```

但 v1 不必过度扩展。

- 不允许 locale separators；
- 是否允许 `_` 必须明确；
- 是否允许前导 `+` 必须明确；
- 是否接受 `0x` / `0b` 必须明确；
- `parse_int_radix` 与 source literal 语法不必完全相同。

推荐 v1：

```text
"-42"
"+42"       可接受
"0042"      可接受
" 42 "      默认拒绝
"1_000"     默认拒绝
```

这样 API 行为简单且与 trim 解耦。

### 6.5 Float 规则

必须定义：

- decimal grammar；
- exponent；
- `NaN`；
- `Infinity`；
- `-0.0`；
- overflow；
- underflow；
- round-trip formatting。

推荐：

```text
format_float(parse_float(format_float(x)))
```

对于有限 Float 尽量达到 round-trip。

建议采用 shortest-roundtrip 算法，而不是 printf 默认 `%f`。

### 6.6 实现建议

优先：

- `.bas` facade；
- Native C parser / formatter；
- 不新增 compiler builtin。

### 6.7 测试

- Int64 min/max；
- overflow；
- invalid character；
- radix boundary；
- Float subnormal；
- ±0；
- exponent；
- NaN / Infinity 规则；
- round-trip；
- locale independence；
- fuzz arbitrary UTF-8 input。

### 6.8 Definition of Done

- [ ] JSON 可复用 Number 转换；
- [ ] Environment / CLI 可复用；
- [ ] 无 locale 隐式依赖；
- [ ] 不使用 Runtime Fault 表示普通 parse failure；
- [ ] Linux / Windows 结果一致。

---

## 7. Phase 2 — UTF-8 String / Buffer Bridge

**优先级：P0**

### 7.1 API

建议添加到：

```text
@std/encoding.bas
```

```text
utf8_encode(text)
utf8_decode(buffer)
```

返回建议：

```text
utf8_encode(String) -> Result<Buffer, Encoding_Error>
utf8_decode(Buffer) -> Result<String, Encoding_Error>
```

如果 String 已保证合法 UTF-8，`utf8_encode` 理论上不会因编码失败，但仍可能：

```text
resource_exhausted
buffer_too_large
```

### 7.2 必须解决 embedded NUL

当前 CString Native 边界对 U+0000 有限制。

新的 Buffer UTF-8 实现不应继续依赖 NUL-terminated CString。

需要定义：

```c
pointer + length
```

形式的 Native byte/string slice ABI，或者使用 Runtime 内部 String byte view。

目标：

```text
"a\u0000b"
```

必须能够：

```text
UTF-8 encode -> [97, 0, 98]
decode -> same String
```

### 7.3 Native ABI 扩展建议

若当前 `.omi` 不支持 length-delimited borrowed String / bytes，可设计：

```text
Utf8Slice
ByteSlice
```

但不要暴露成 BASIC 普通类型。

它们只作为 Native ABI metadata 类型。

### 7.4 测试

- ASCII；
- Japanese；
- emoji；
- U+0000；
- maximum scalar；
- invalid UTF-8 Buffer；
- truncated multi-byte；
- overlong encoding；
- surrogate encoded bytes；
- 64 MiB boundary。

---

## 8. Phase 3 — Binary File System

**优先级：P0**

### 8.1 新协议

建议：

```text
File_Read_Bytes
File_Read_Bytes_Completed

File_Write_Bytes
File_Write_Bytes_Completed

File_Append_Bytes
File_Append_Bytes_Completed
```

### 8.2 数据边界

Request / Completion 应使用 Buffer facade 或 Runtime 可安全转换的：

```text
List<Int>
```

长期建议公共 API 统一使用：

```text
Buffer
```

如果 Runtime Builtin Message 暂时不能直接引用 stdlib Record，则可以保持：

```text
List<Int>
```

然后由：

```text
file_system.buffer_from_read_bytes(...)
```

包装。

但文档必须明确这是 ABI / protocol bridge，而不是新的 byte model。

### 8.3 限制

建议继续：

```text
64 MiB per operation
```

并与 Encoding / Network 上限保持一致，除非有充分理由不同。

### 8.4 错误

复用 File System stable codes：

```text
not_found
permission_denied
already_exists
not_directory
is_directory
invalid_path
resource_exhausted
unsupported_operation
io_error
```

Binary I/O 不产生：

```text
invalid_utf8
```

因为 bytes 不要求 UTF-8。

### 8.5 测试

- empty file；
- all byte values 0..255；
- embedded zero；
- 1 MiB / 64 MiB boundary；
- append；
- directory error；
- permission error；
- `/dev/full`；
- concurrent operations；
- Runtime Shutdown；
- GC pressure。

---

## 9. Phase 4 — URI / URL

**优先级：P1**

### 9.1 模块

建议：

```text
stdlib/
└── uri/
    ├── uri.bas
    ├── parser.bas
    ├── percent.bas
    └── uri_error.bas
```

### 9.2 数据模型

建议：

```basic
RECORD Uri(
    scheme,
    user_info,
    host,
    port,
    path,
    query,
    fragment
)
```

但需避免把不存在与空字符串混淆。

如果 0.1 类型系统不适合 optional fields，可采用明确 flags：

```basic
RECORD Uri(
    scheme,
    authority_present,
    user_info,
    host,
    port_present,
    port,
    path,
    query_present,
    query,
    fragment_present,
    fragment
)
```

或者先做功能型 facade，不暴露完整 Record。

### 9.3 API

```text
parse
format

percent_encode
percent_decode

query_encode
query_decode

resolve_reference

is_absolute
default_port
```

### 9.4 语义范围

优先遵循 RFC 3986。

第一版不要混入：

- DNS；
- network connect；
- filesystem paths；
- browser URL normalization；
- IDNA / Unicode hostname 的复杂策略。

Host 在 v1 可以保持文本，不自动做 DNS。

### 9.5 Percent Encoding

必须定义不同 component 的 safe set：

```text
path
query
fragment
userinfo
```

不要提供一个模糊的：

```text
url_encode_everything()
```

### 9.6 测试

- absolute URI；
- relative reference；
- IPv4；
- bracketed IPv6；
- port；
- empty query；
- empty fragment；
- percent errors；
- dot segments；
- UTF-8 percent encoding；
- fuzz parser。

---

## 10. Phase 5 — Hash / Checksum

**优先级：P1**

### 10.1 模块布局

建议：

```text
stdlib/
└── hash/
    ├── hash.bas
    ├── sha256.bas
    └── checksum.bas
```

### 10.2 v1 范围

#### Cryptographic hash

```text
sha256(buffer) -> Buffer
sha256_hex(buffer) -> String
```

#### Checksum

```text
crc32(buffer) -> Int
```

明确：

```text
CRC32 is not cryptographic.
```

### 10.3 为什么先只做 SHA-256

避免一开始维护：

```text
MD5
SHA-1
SHA-2 family
SHA-3
BLAKE2
BLAKE3
HMAC
PBKDF2
Argon2
```

v1 只需要一个明确的现代通用 hash。

### 10.4 实现

优先 Native：

- 使用内置小实现或平台无关受审查实现；
- 不依赖 OpenSSL 作为基础库的强制运行依赖；
- 不新增 Compiler Builtin。

### 10.5 后续 HMAC

只有当 HTTP Auth / API signing 真正需要时增加：

```text
hmac_sha256
```

并单独定义 secret handling。

### 10.6 测试

使用标准 Known Answer Tests：

```text
empty
"abc"
large input
binary zeros
incremental-equivalent vectors
```

若 v1 不提供 streaming API，则只验证 one-shot。

---

## 11. Phase 6 — Environment / Platform

**优先级：P1**

### 11.1 不建议做一个巨大 `os` 模块

拆分为：

```text
@std/platform/platform.bas
@std/environment/environment.bas
```

### 11.2 Platform

平台事实属于编译目标 / Runtime facts。

API 候选：

```text
os_name()
architecture()
target_triple()
path_separator()
line_separator()
```

但注意：

`path_separator` 是否属于 Path 模块更合理，需要 API Review。

### 11.3 Environment

需要：

```text
command_line_arguments
get_environment_variable
current_working_directory
executable_path
```

#### Effect 模型

这些值不是纯语言事实。

不能设计成：

```basic
FUNCTION get_env(name)
```

却宣称纯函数。

推荐方案 A：

建立：

```text
Environment
```

System Actor。

Message：

```text
Environment_Arguments
Environment_Arguments_Completed

Environment_Get
Environment_Get_Completed

Environment_Current_Directory
Environment_Current_Directory_Completed

Environment_Executable_Path
Environment_Executable_Path_Completed
```

推荐方案 B：

如果这些值在 Runtime 启动后被冻结，可以把它们定义为显式 Runtime Snapshot，并通过 Action 调用。

但为了保持统一，本计划更建议 System Actor。

### 11.4 暂不做

v1 不建议立即支持：

```text
setenv
unsetenv
chdir
```

这些是进程级全局可变状态，会降低 Actor 局部可分析性。

如果未来需要，应通过单独 ADR 决定。

---

## 12. Phase 7 — Typed IP / Socket Address

**优先级：P1**

### 12.1 当前问题

Network v1 仍主要使用：

```text
address String
port Int
opaque resource ID
```

这对低层 API 足够，但 HTTP、DNS、TLS 会需要更稳定的数据模型。

### 12.2 建议类型

```basic
RECORD Ip_Address(family, bytes)
RECORD Socket_Address(address, port)
```

如果 nested Record 当前能力会带来公共 API 问题，也可以：

```basic
RECORD Ip_Address(family, bytes)
RECORD Socket_Address(family, bytes, port)
```

family：

```text
4
6
```

或稳定字符串：

```text
ipv4
ipv6
```

### 12.3 Pure API

```text
parse_ip
format_ip

socket_address
format_socket_address
```

### 12.4 规则

- IPv4 不接受模糊 octal；
- IPv6 formatter 定义 canonical compression；
- port = 0..65535；
- host name 不属于 IP Address；
- DNS name 与 numeric address 严格区分。

---

## 13. Phase 8 — DNS Actor

**优先级：P2**

### 13.1 Actor

推荐继续复用：

```text
Network
```

Actor，而不是新建另一套调度服务。

Request：

```text
Dns_Resolve(host, service_or_port, reply)
```

Completion：

```text
Dns_Resolve_Completed(Result<List<Socket_Address>, String>)
```

### 13.2 错误码

```text
invalid_host
not_found
temporary_failure
resource_exhausted
io_error
unsupported
```

不要把 `gai_strerror()` 文本作为稳定错误值。

### 13.3 顺序

必须明确结果顺序：

- 可保留平台 resolver 顺序；
- 但必须记录这是 preference order，而不是 deterministic lexical order。

测试不应要求 Windows / Linux 返回完全相同排序，除非 Runtime 主动规范化。

### 13.4 缓存

v1：

```text
不提供用户可见 DNS cache
```

以后若增加缓存，应放 Network Actor 内并定义 TTL。

### 13.5 测试

- localhost；
- numeric literal；
- invalid host；
- IPv4/IPv6；
- temporary failure；
- request saturation；
- Runtime Shutdown。

外网 DNS 不应作为稳定 CI 前提。

---

## 14. Phase 9 — TLS Transport

**优先级：P2**

### 14.1 原则

TLS 是 Network 上的 transport resource。

不要增加：

```text
Future<TlsConnection>
callback
event loop
```

### 14.2 资源模型

推荐：

```text
Network Actor
  |
  +-- Tcp resource
  |
  +-- Tls resource
```

TLS resource 依赖已连接 TCP resource。

### 14.3 v1 Client API

```text
Tls_Client_Start
Tls_Client_Start_Completed

Tls_Send
Tls_Send_Completed

Tls_Receive
Tls_Receive_Completed

Tls_Close
Tls_Close_Completed
```

### 14.4 配置

最小：

```text
server_name
verify_peer
CA source policy
ALPN list
```

推荐 v1：

- 默认必须验证证书；
- 默认验证 hostname；
- 不提供一键 `insecure = true` 的普通快捷路径，或至少必须显式命名危险性；
- TLS 1.2+；
- 优先 TLS 1.3。

### 14.5 CA 信任

必须定义平台策略：

Windows：

```text
Windows certificate store
```

Linux：

需要明确：

```text
system CA bundle discovery
```

不要偷偷依赖开发机环境。

### 14.6 Backend

可评估：

- OpenSSL
- Schannel
- mbedTLS
- wolfSSL

但不要让 BASIC API 与 backend 绑定。

### 14.7 Error

```text
certificate_invalid
hostname_mismatch
handshake_failed
protocol_error
closed
io_error
resource_exhausted
unsupported
```

### 14.8 测试

CI 不依赖公共互联网。

建立本地测试 CA / server fixture：

- valid cert；
- wrong host；
- expired；
- untrusted；
- abrupt close；
- fragmented TLS records；
- large payload；
- Runtime Shutdown。

---

## 15. Phase 10 — HTTP/1.1 Client

**优先级：P2**

**实施状态（2026-10-31）：** 已实现 Network-backed HTTP/1.1 Client Actor、
Content-Length / chunked 响应解析、TCP/TLS 请求、同源 keep-alive 复用和显式
`Http_Close` 生命周期。Conformance 覆盖本地 TCP、TLS 和同一连接上的连续请求；
直接 HTTP 响应 parser fuzz target 仍待补齐。

### 15.1 为什么只先做 Client

Server 会引入：

- Listener lifecycle
- routing
- request body streaming
- backpressure
- response ownership
- keep-alive scheduling

第一阶段先建立 Client，可以验证 URI + DNS + TCP + TLS + Buffer 的完整组合。

### 15.2 模块

```text
stdlib/
└── http/
    ├── http.bas
    ├── request.bas
    ├── response.bas
    ├── headers.bas
    └── http_error.bas
```

### 15.3 数据模型

示例：

```basic
RECORD Header(name, value)

RECORD Http_Request(
    method,
    uri,
    headers,
    body
)

RECORD Http_Response(
    status,
    headers,
    body
)
```

body：

```text
Buffer
```

不要默认 String。

Text convenience helper 可在 facade 中：

```text
body_utf8
```

显式解码。

### 15.4 Actor

可以新建：

```text
Http
```

System Actor，

也可以做普通 Actor library 建在 Network 上。

本计划更推荐：

**先尝试普通 OMBasic Actor library。**

只有当需要 Runtime 深度控制连接池 / TLS / socket 时，再引入 System Actor。

目标是避免所有网络协议都膨胀 Builtin Registry。

### 15.5 v1 支持

- GET
- HEAD
- POST
- PUT
- DELETE
- arbitrary method token
- Content-Length
- chunked response
- redirects：可选，建议显式 policy
- keep-alive
- connection close
- HTTP/1.1

### 15.6 暂不做

```text
HTTP/2
HTTP/3
WebSocket
proxy
cookie jar
automatic auth
multipart builder
server
```

这些全部后续独立规划。

### 15.7 Header 规则

- field name case-insensitive；
- 保留原始 value；
- 防止 CRLF injection；
- 合法 token validation；
- 限制 header count / total bytes。

### 15.8 资源限制

建议：

```text
max header count
max header bytes
max body bytes
max redirects
max active requests
```

必须全部有 stable `resource_exhausted` 路径。

---

## 16. Phase 11 — Collections 类型前置问题

**优先级：P2，但属于语言基础修复**

### 16.1 当前阻塞

这些 API 很自然：

```text
take
drop
slice
map
filter
flat_map
```

但当输出为空时，需要构造：

```basic
[]
```

而当前缺少稳定方式让空 List 保留 expected element type。

### 16.2 不建议的 workaround

不要：

- 用 `[dummy]` 再删除；
- Native 构造泛型集合；
- 给每个 Collections helper 增加 Compiler Builtin；
- 使用 Null / Any 逃避类型系统。

### 16.3 推荐解决方向

让 expected type 能传播到空 collection literal：

概念上：

```text
expected List<T>
   ↓
[]
   ↓
List<T>
```

同样适用于：

```text
{}
```

如果 expected Map<K,V> 已知。

### 16.4 需要验证

- local binding；
- function return；
- branch join；
- generic specialization；
- imported function；
- public API interface；
- nested List；
- Map values；
- Result<List<T>, E>；
- higher-order function output。

### 16.5 完成后 Collections 可扩展

```text
take
drop
slice
map
filter
flat_map
partition
zip
enumerate
any
all
fold
```

不要一次全部加入；优先常用且语义清晰的。

---

## 17. Phase 12 — UUID

**优先级：P3**

依赖：

```text
Secure_Random
Buffer
Encoding
```

v1：

```text
uuid_v4
uuid_parse
uuid_format
```

若 `uuid_v4` 需要 secure random，则它是 Actor operation：

```text
Uuid_Generate_V4
Uuid_Generate_V4_Completed
```

或者由调用方先取得 secure random bytes，再 Pure 构造 UUID。

本计划更推荐第二种：

```text
uuid.v4_from_random_bytes(buffer)
```

这样 UUID 本身保持纯库，Secure Random Actor 仍是唯一系统随机入口。

---

## 18. Phase 13 — CSV

**优先级：P3**

纯标准库即可。

模块：

```text
@std/csv/csv.bas
```

v1：

```text
parse
stringify
```

明确：

- RFC 4180 baseline；
- quote escaping；
- CRLF / LF；
- embedded newline；
- UTF-8 String input；
- malformed quote error offset；
- maximum field / row limit。

对于超大文件 streaming CSV，后续可结合 File Actor 分块协议，第一版不必实现。

---

## 19. Phase 14 — Compression / Archive

**优先级：P3**

**实施状态（2026-10-31）：** 已实现 one-shot gzip 与 zlib-wrapped DEFLATE
的 Buffer API、64 MiB 输出限制、checksum 校验、conformance 和 ASan/UBSan
解码 fuzz target。ZIP/TAR、流式 API 和可配置压缩级别仍未实现。

先做：

```text
gzip
deflate
```

再考虑：

```text
zip
tar
```

数据边界：

```text
Buffer -> Buffer
```

大数据 streaming 需要 Actor / resource 模型，v1 可先 one-shot 且限制大小。

错误：

```text
invalid_format
checksum_mismatch
resource_exhausted
unsupported
```

---

## 20. Phase 15 — Regex

**优先级：P3 / P4**

**实施状态（2026-10-31）：** 已实现受限 Regex v1：UTF-8 模式解析、Thompson
NFA、Unicode 标量索引 `find`、稳定错误码、资源限制、conformance 和 ASan/UBSan
fuzzer。Capture 提取、替换和 PCRE 扩展仍不支持。

不要太早做。

原因：

- Unicode 语义复杂；
- backtracking DoS；
- capture model；
- replacement syntax；
- engine dependency；
- pattern compilation lifetime。

如果做，推荐：

```text
RE2 style
```

即：

```text
linear-time
no backreferences
```

优先保证资源上限和可预测性，而不是 PCRE 全兼容。

---

## 21. Phase 16 — SQLite / Database

**优先级：P4**

**实施状态（2026-10-31）：** 已实现 SQLite 3.54.1 amalgamation 与
`stdlib/sqlite` Database Actor v1；覆盖 open、单语句 execute/query、JSON
行结果、BLOB Base64、关闭、稳定错误码、资源限制和 conformance。绑定参数、
事务便利 API、异步 Runtime worker offload 和连接池仍待后续扩展。

不要把数据库加入 Core Runtime Builtin。

推荐：

```text
stdlib/sqlite/
```

通过：

```text
module-scoped Native interface
```

或：

```text
database Actor library
```

如果操作可能阻塞：

```text
Database Actor
```

负责连接状态。

第一阶段 SQLite 最合适，因为：

- 单一成熟 C ABI；
- 不需要网络；
- 容易做端到端测试；
- 可验证 Native Record / String / Buffer / Actor 边界。

---

## 22. 不建议近期加入的模块

以下并非不需要，而是不应现在优先：

```text
XML
YAML
TOML
WebSocket
HTTP/2
HTTP/3
SMTP
FTP
SSH
full crypto suite
GUI
image codecs
database drivers
package registry
reflection
dynamic loading
plugin runtime
```

理由：

当前应先稳定共同基础，而不是扩大 API 表面积。

---

## 23. 推荐 Git Commit / PR 顺序

### PR 1

```text
stdlib: add number parsing and formatting
```

内容：

- Number_Error
- Int parse / format
- radix
- Float parse / format
- tests
- docs

### PR 2

```text
encoding: add length-delimited utf8 buffer conversion
```

内容：

- embedded NUL
- Buffer encode/decode
- Native ABI slice if required
- fuzz

### PR 3

```text
runtime: add binary file system operations
```

内容：

- read bytes
- write bytes
- append bytes
- Runtime Actor messages
- tests

### PR 4

```text
stdlib: add uri parsing and percent encoding
```

### PR 5

```text
stdlib: add sha256 and crc32
```

### PR 6

```text
stdlib: add platform and environment contracts
```

优先先做 Platform pure facts；Environment Actor 可单独 PR。

### PR 7

```text
network: add typed ip and socket address values
```

### PR 8

```text
network: add dns actor protocol
```

### PR 9

```text
network: add tls transport
```

### PR 10

```text
stdlib: add http 1.1 client
```

### PR 11

```text
type-inference: propagate expected collection types to empty literals
```

### PR 12

```text
stdlib: expand collection transforms
```

### PR 13+

```text
uuid
csv
compression
regex
sqlite
```

---

## 24. Testing Matrix

### 24.1 Number

```text
unit
conformance
fuzz
roundtrip
cross-platform
```

### 24.2 Encoding

```text
UTF-8 malformed bytes
embedded zero
all scalar classes
64 MiB
ASan/UBSan
fuzz
```

### 24.3 Binary File

```text
all 256 byte values
empty file
large file
append
permission error
storage error
shutdown
TSan
```

### 24.4 URI

```text
RFC vectors
percent malformed input
IPv6
relative resolution
fuzz
```

### 24.5 Hash

```text
known answer tests
large buffer
binary zero
cross-platform deterministic
```

### 24.6 DNS

```text
localhost
numeric host
invalid host
IPv4/IPv6
shutdown
queue saturation
TSan
```

### 24.7 TLS

```text
local CA
valid host
hostname mismatch
expired cert
untrusted cert
large payload
abrupt close
ASan/UBSan
TSan
```

### 24.8 HTTP

```text
content-length
chunked
empty body
binary body
large headers
malformed response
redirect
keepalive
connection close
TLS
fuzz parser
```

---

## 25. Sanitizer 与 Fuzz 要求

### ASan + UBSan

必须覆盖：

- Number Native parser；
- UTF-8 Native boundary；
- Binary File；
- URI parser；
- Hash implementation；
- DNS resolver bridge；
- TLS backend；
- HTTP parser。

### TSan

必须覆盖：

- File System Actor；
- Environment Actor；
- Network DNS；
- TLS resources；
- HTTP Actor；
- Runtime shutdown；
- concurrent Network requests。

### Fuzz

建议新增：

```text
number_parser_fuzzer
utf8_decoder_fuzzer
uri_parser_fuzzer
http_response_parser_fuzzer
```

如果 Compression / Regex 后续加入：

```text
compression_decoder_fuzzer
regex_parser_fuzzer
```

---

## 26. Resource Limit Policy

建议建立统一文档：

```text
docs/stdlib/resource_limits.md
```

记录：

| 能力 | 限制 |
|---|---:|
| Buffer | 64 MiB |
| File read/write | 64 MiB |
| Network send/receive | 当前协议限制 |
| Process stream request | 当前协议限制 |
| URI length | 建议 1 MiB 以下 |
| HTTP headers | 建议 64 KiB |
| HTTP header count | 建议 100-256 |
| HTTP body | configurable bounded |
| DNS results | bounded |
| TLS resources | bounded |

所有上限必须：

- 使用 checked size arithmetic；
- 报 `resource_exhausted`；
- 不通过 assertion 失败；
- 不允许分配整数溢出。

---

## 27. API Review Checklist

每个新模块合入前必须回答：

### Execution

- [ ] Pure 还是 side-effecting？
- [ ] 是否等待外部资源？
- [ ] 如果等待，是否通过 Actor？
- [ ] 是否错误引入 Future / Callback？
- [ ] Blocking Pool 是否仅存在于 Runtime 内部？

### Types

- [ ] String 与 Buffer 是否正确区分？
- [ ] 是否引入不必要的 Any？
- [ ] Optional state 是否有明确表示？
- [ ] 是否可以跨 Actor 安全发送？

### Error

- [ ] 普通失败使用 Result / Completion Message？
- [ ] error code 是否稳定？
- [ ] 是否依赖 OS localized message？
- [ ] Runtime Fault 是否只用于真正无法继续当前操作的语言级错误？

### Resource

- [ ] 最大输入多大？
- [ ] 最大输出多大？
- [ ] active resource 最大数量？
- [ ] 队列满时如何失败？
- [ ] Shutdown 如何处理？

### Portability

- [ ] Windows 行为？
- [ ] Linux 行为？
- [ ] 是否依赖 locale？
- [ ] 是否依赖 host-specific path？
- [ ] SDK relocation 是否受影响？

### Tests

- [ ] positive
- [ ] negative
- [ ] boundary
- [ ] stress
- [ ] ASan/UBSan
- [ ] TSan if concurrent
- [ ] fuzz if parsing external input

---

## 28. Definition of Done — Foundation Layer

当以下项目完成时，可以认为 OMBasic 第二阶段基础库已经形成：

### Conversion

- [ ] Int / Float / Bool conversion 完整；
- [ ] Number parse error 稳定；
- [ ] round-trip format 有定义。

### Binary

- [ ] String <-> Buffer UTF-8；
- [ ] embedded NUL 正常处理；
- [ ] Binary File I/O；
- [ ] Buffer 保持唯一 binary model。

### Resource Identity

- [ ] IP Address；
- [ ] Socket Address；
- [ ] URI。

### Integrity

- [ ] SHA-256；
- [ ] CRC32；
- [ ] Known Answer Tests。

### Process Context

- [ ] Arguments；
- [ ] Environment；
- [ ] Working Directory；
- [ ] Executable Path；
- [ ] Platform info。

### Network

- [ ] TCP；
- [ ] UDP；
- [ ] DNS；
- [ ] TLS；
- [ ] HTTP/1.1 Client。

### Collections

- [ ] typed empty List inference；
- [ ] map / filter / slice / take / drop 可自然实现；
- [ ] 不依赖 Native workaround。

### Quality

- [ ] Windows Release；
- [ ] Linux Release；
- [ ] ASan / UBSan；
- [ ] TSan；
- [ ] fuzz；
- [ ] SDK install / relocation；
- [ ] API docs；
- [ ] ADR；
- [ ] conformance。

---

## 29. 推荐里程碑

### Milestone A — Conversion & Binary Foundation

包含：

```text
Number
UTF-8 Buffer bridge
Binary File I/O
Hash
```

完成后，OMBasic 获得可靠的数据边界。

### Milestone B — Application Identity Foundation

包含：

```text
URI
Environment
Platform
IP Address
Socket Address
```

完成后，CLI 和网络应用具有稳定基础模型。

### Milestone C — Internet Foundation

包含：

```text
DNS
TLS
HTTP/1.1 Client
```

完成后，可以真实访问 HTTPS API。

### Milestone D — Collection Completion

包含：

```text
typed empty collections
map/filter/take/drop/slice
```

完成后，纯函数式数据处理能力明显提升。

### Milestone E — Utility Layer

包含：

```text
UUID
CSV
Compression
Regex
SQLite
```

这些不属于最底层，但会显著扩大真实应用覆盖面。

---

## 30. 最终建议

下一阶段不要继续以“有多少个标准库模块”为成功标准。

更合理的目标是：

> **让已有标准库之间共享一套稳定的数据、错误、资源和 Actor 模型。**

优先顺序应为：

```text
1. Number
2. UTF-8 Buffer
3. Binary File
4. URI
5. Hash
6. Environment / Platform
7. IP / Socket Address
8. DNS
9. TLS
10. HTTP
```

这十项完成后，OMBasic 会具备一条完整的真实应用链：

```text
CLI
  |
Environment
  |
File + Buffer
  |
JSON / Number
  |
URI
  |
DNS
  |
TCP
  |
TLS
  |
HTTP
```

整个过程仍然保持：

```text
FUNCTION = pure
ACTION = explicit side effect
ACTOR = async / mutable service state
MESSAGE = async communication
```

这是目前 OMBasic 最值得保护的架构优势。

---

## 31. 当前仓库基线参考

本计划基于当前 `master` 的以下模块与文档状态编制：

- `docs/stdlib/overview.md`
- `docs/stdlib/collections.md`
- `docs/stdlib/encoding.md`
- `docs/stdlib/file_system.md`
- `docs/stdlib/network.md`
- `docs/stdlib/process.md`
- `docs/stdlib/random.md`
- `docs/stdlib/time.md`
- `include/ombasic/semantic/builtin_registry.def`

GitHub repository:

https://github.com/risol/OMBasic

本计划中提到的未来 API 名称属于实施建议，不表示当前 SDK 已经提供这些符号。正式合入前应通过 ADR、API Review 和 Conformance 固化。
