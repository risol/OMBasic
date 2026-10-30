# Prelude and Runtime Builtin Inventory

Builtin names, categories, type-rule notes, lowering/protocol notes, and rule
references are declared in `builtin_registry.def`; qualified built-in members
are declared in `builtin_member_registry.def`. The public descriptor views are
exposed by `name_resolver.h`; resolver name and member lookup use those same
registries. This inventory records each identity's human-readable contract.

| Identity | Source name | Category | Semantic/lowering behavior |
|---|---|---|---|
| `OM_BUILTIN_UNIT` | `Unit` | Core Prelude | Unit type; MIR unit value. |
| `OM_BUILTIN_RESULT` | `Result` | Core Prelude | Built-in tagged union; type inference and `TRY` propagation. |
| `OM_BUILTIN_OK` | `Ok` | Core Prelude | Result constructor with inferred payload/error types. |
| `OM_BUILTIN_ERROR` | `Error` | Core Prelude | Result constructor with inferred payload/error types. |
| `OM_BUILTIN_NODE_REF` | `Node_Ref` | Core Prelude | Node identity type. |
| `OM_BUILTIN_ACTOR_ID` | `Actor_Id` | Core Prelude | Actor identity type. |
| `OM_BUILTIN_ACTOR_DOWN_REASON` | `Actor_Down_Reason` | Core Prelude | Nominal lifecycle reason; members below have stable member IDs. |
| `OM_BUILTIN_ACTOR_DOWN` | `Actor_Down` | Core Prelude/system Message | Lifecycle Message injected into Actor protocols. |
| `OM_BUILTIN_PRINT` | `print` | Runtime-backed primitive | Type is `(String) -> Unit`; MIR `OM_MIR_RUNTIME_PRINT` calls the Runtime. |
| `OM_BUILTIN_SYSTEM_ACTOR_CONSOLE` | `Console` | Runtime Service | Predefined Actor protocol; handles `Console_Write`. |
| `OM_BUILTIN_SYSTEM_ACTOR_TIMER` | `Timer` | Runtime Service | Predefined Actor protocol; handles `Timer_After`. |
| `OM_BUILTIN_SYSTEM_ACTOR_FILE_SYSTEM` | `File_System` | Runtime Service | Predefined Actor protocol; handles text file requests. |
| `OM_BUILTIN_SYSTEM_ACTOR_CLOCK` | `Clock` | Runtime Service | Predefined Actor protocol; returns monotonic or UTC millisecond samples. |
| `OM_BUILTIN_SYSTEM_ACTOR_SECURE_RANDOM` | `Secure_Random` | Runtime Service | OS-backed cryptographic random byte requests; no PRNG fallback. |
| `OM_BUILTIN_SYSTEM_ACTOR_LOGGER` | `Logger` | Runtime Service | FIFO stdout writes and flush completion through the bounded blocking pool. |
| `OM_BUILTIN_SYSTEM_ACTOR_NETWORK` | `Network` | Runtime Service | TCP and connected UDP operations over opaque runtime resource IDs. |
| `OM_BUILTIN_SYSTEM_ACTOR_PROCESS` | `Process` | Runtime Service | Spawn and manage child processes, standard streams, termination, and exit status through opaque resource IDs. |
| `OM_BUILTIN_SYSTEM_MESSAGE_CONSOLE_WRITE` | `Console_Write` | Runtime Service Message | Request payload carries output data; completion is asynchronous. |
| `OM_BUILTIN_SYSTEM_MESSAGE_CONSOLE_WRITE_COMPLETED` | `Console_Write_Completed` | Runtime Service Message | Console request completion. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_READ` | `File_Read` | Runtime Service Message | Read request handled by the blocking pool. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_READ_COMPLETED` | `File_Read_Completed` | Runtime Service Message | Read result completion. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_WRITE` | `File_Write` | Runtime Service Message | Write request handled by the blocking pool. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_WRITE_COMPLETED` | `File_Write_Completed` | Runtime Service Message | Write result completion. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_APPEND` | `File_Append` | Runtime Service Message | Append UTF-8 text using the blocking pool. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_APPEND_COMPLETED` | `File_Append_Completed` | Runtime Service Message | Append result completion. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_EXISTS` | `File_Exists` | Runtime Service Message | Query whether a file or directory exists. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_EXISTS_COMPLETED` | `File_Exists_Completed` | Runtime Service Message | Existence result completion. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_DELETE` | `File_Delete` | Runtime Service Message | Remove one file or symbolic link. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_DELETE_COMPLETED` | `File_Delete_Completed` | Runtime Service Message | Delete result completion. |
| `OM_BUILTIN_SYSTEM_MESSAGE_DIRECTORY_CREATE` | `Directory_Create` | Runtime Service Message | Create directory and missing parents. |
| `OM_BUILTIN_SYSTEM_MESSAGE_DIRECTORY_CREATE_COMPLETED` | `Directory_Create_Completed` | Runtime Service Message | Directory creation completion. |
| `OM_BUILTIN_SYSTEM_MESSAGE_DIRECTORY_LIST` | `Directory_List` | Runtime Service Message | List immediate directory child names. |
| `OM_BUILTIN_SYSTEM_MESSAGE_DIRECTORY_LIST_COMPLETED` | `Directory_List_Completed` | Runtime Service Message | Sorted directory listing completion. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_STAT` | `File_Stat` | Runtime Service Message | Read file or directory metadata. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_STAT_COMPLETED` | `File_Stat_Completed` | Runtime Service Message | Returns kind, decimal byte size, and Unix millisecond modification time. |
| `OM_BUILTIN_SYSTEM_MESSAGE_CLOCK_MONOTONIC_NOW` | `Clock_Monotonic_Now` | Runtime Service Message | Request a monotonic millisecond sample. |
| `OM_BUILTIN_SYSTEM_MESSAGE_CLOCK_MONOTONIC_NOW_COMPLETED` | `Clock_Monotonic_Now_Completed` | Runtime Service Message | Monotonic sample completion. |
| `OM_BUILTIN_SYSTEM_MESSAGE_CLOCK_UTC_NOW` | `Clock_Utc_Now` | Runtime Service Message | Request a UTC Unix-millisecond sample. |
| `OM_BUILTIN_SYSTEM_MESSAGE_CLOCK_UTC_NOW_COMPLETED` | `Clock_Utc_Now_Completed` | Runtime Service Message | UTC sample completion. |
| `OM_BUILTIN_SYSTEM_MESSAGE_SECURE_RANDOM_BYTES` | `Secure_Random_Bytes` | Runtime Service Message | Request OS entropy; returns `List<Int>` bytes or stable error code. |
| `OM_BUILTIN_SYSTEM_MESSAGE_LOGGER_WRITE` | `Logger_Write` | Runtime Service Message | Bounded FIFO write to stdout; completion confirms flush. |
| `OM_BUILTIN_SYSTEM_MESSAGE_LOGGER_FLUSH` | `Logger_Flush` | Runtime Service Message | Flush barrier after earlier accepted Logger writes. |
| `OM_BUILTIN_SYSTEM_MESSAGE_TCP_LISTEN` | `Tcp_Listen` | Runtime Service Message | Bind a TCP listener and return an opaque resource ID. |
| `OM_BUILTIN_SYSTEM_MESSAGE_TCP_CONNECT` | `Tcp_Connect` | Runtime Service Message | Connect to an address and return an opaque resource ID. |
| `OM_BUILTIN_SYSTEM_MESSAGE_TCP_ACCEPT` | `Tcp_Accept` | Runtime Service Message | Accept one connection from a listener ID. |
| `OM_BUILTIN_SYSTEM_MESSAGE_TCP_SEND` | `Tcp_Send` | Runtime Service Message | Send byte values and return the number accepted locally. |
| `OM_BUILTIN_SYSTEM_MESSAGE_TCP_RECEIVE` | `Tcp_Receive` | Runtime Service Message | Receive at most the requested bytes; EOF has its own error code. |
| `OM_BUILTIN_SYSTEM_MESSAGE_TCP_CLOSE` | `Tcp_Close` | Runtime Service Message | Idempotently close a TCP resource and interrupt outstanding work. |
| `OM_BUILTIN_SYSTEM_MESSAGE_UDP_CONNECT` | `Udp_Connect` | Runtime Service Message | Bind a local UDP port and connect to one remote peer. |
| `OM_BUILTIN_SYSTEM_MESSAGE_UDP_SEND` | `Udp_Send` | Runtime Service Message | Send one datagram and return its byte count. |
| `OM_BUILTIN_SYSTEM_MESSAGE_UDP_RECEIVE` | `Udp_Receive` | Runtime Service Message | Receive one datagram up to the requested limit. |
| `OM_BUILTIN_SYSTEM_MESSAGE_UDP_CLOSE` | `Udp_Close` | Runtime Service Message | Idempotently close a UDP resource. |
| `OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_SPAWN` | `Process_Spawn` | Runtime Service Message | Spawn an executable with separate argv, explicit environment policy, and working directory. |
| `OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_WRITE_STDIN` | `Process_Write_Stdin` | Runtime Service Message | Write bounded byte data to child stdin. |
| `OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_CLOSE_STDIN` | `Process_Close_Stdin` | Runtime Service Message | Close child stdin. |
| `OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_READ_STDOUT` | `Process_Read_Stdout` | Runtime Service Message | Read a bounded stdout chunk. |
| `OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_READ_STDERR` | `Process_Read_Stderr` | Runtime Service Message | Read a bounded stderr chunk. |
| `OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_TERMINATE` | `Process_Terminate` | Runtime Service Message | Request child termination. |
| `OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_WAIT` | `Process_Wait` | Runtime Service Message | Wait for and return the child exit code. |
| `OM_BUILTIN_SYSTEM_MESSAGE_TIMER_AFTER` | `Timer_After` | Runtime Service Message | Schedules a future mailbox Message. |
| `OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_NORMAL` | `Actor_Down_Reason.normal` | Core Prelude member | Normal Actor termination. |
| `OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_FAILED` | `Actor_Down_Reason.failed` | Core Prelude member | Actor failed. |
| `OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_NODE_UNREACHABLE` | `Actor_Down_Reason.node_unreachable` | Core Prelude member | Remote termination could not be confirmed. |

`print` accepts exactly one String, appends one LF through
`om_runtime_print`, returns Unit, and converts a failed write into the Runtime
fault path. It does not flush and has no recoverable output error channel.
These behaviors are covered by type inference, MIR lowering, Runtime, and
conformance tests. `Console_Write` writes its exact payload, flushes, and
completes through its reply Actor. Concurrent Console requests have no
completion ordering guarantee. These APIs do not share `print`'s synchronous
surface semantics.

The enum-to-name mapping is reviewed in semantic resolver tests. Type inference
special cases are in `src/semantic/type_inference/type_inference.c`; Actor and
Message protocol wiring is in `src/semantic/actor_protocol/actor_protocol.c`;
print lowering is in `src/ir/mir/lower_hir.c` and its Runtime ABI declaration is
in `src/backend/llvm/runtime_calls.c`.
