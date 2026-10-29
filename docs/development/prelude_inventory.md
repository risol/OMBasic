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
| `OM_BUILTIN_SYSTEM_ACTOR_FILE_SYSTEM` | `File_System` | Runtime Service | Predefined Actor protocol; handles read and write requests. |
| `OM_BUILTIN_SYSTEM_MESSAGE_CONSOLE_WRITE` | `Console_Write` | Runtime Service Message | Request payload carries output data; completion is asynchronous. |
| `OM_BUILTIN_SYSTEM_MESSAGE_CONSOLE_WRITE_COMPLETED` | `Console_Write_Completed` | Runtime Service Message | Console request completion. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_READ` | `File_Read` | Runtime Service Message | Read request handled by the blocking pool. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_READ_COMPLETED` | `File_Read_Completed` | Runtime Service Message | Read result completion. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_WRITE` | `File_Write` | Runtime Service Message | Write request handled by the blocking pool. |
| `OM_BUILTIN_SYSTEM_MESSAGE_FILE_WRITE_COMPLETED` | `File_Write_Completed` | Runtime Service Message | Write result completion. |
| `OM_BUILTIN_SYSTEM_MESSAGE_TIMER_AFTER` | `Timer_After` | Runtime Service Message | Schedules a future mailbox Message. |
| `OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_NORMAL` | `Actor_Down_Reason.normal` | Core Prelude member | Normal Actor termination. |
| `OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_FAILED` | `Actor_Down_Reason.failed` | Core Prelude member | Actor failed. |
| `OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_NODE_UNREACHABLE` | `Actor_Down_Reason.node_unreachable` | Core Prelude member | Remote termination could not be confirmed. |

`print` accepts exactly one String, appends one LF through
`om_runtime_print`, returns Unit, and converts a failed write into the Runtime
fault path. It has no flush operation and no recoverable output error channel.
These behaviors are covered by type inference, MIR lowering, Runtime, and
conformance tests. Other console I/O uses explicit Actor messages and completion
messages; it does not share `print`'s synchronous surface semantics.

The enum-to-name mapping is reviewed in semantic resolver tests. Type inference
special cases are in `src/semantic/type_inference/type_inference.c`; Actor and
Message protocol wiring is in `src/semantic/actor_protocol/actor_protocol.c`;
print lowering is in `src/ir/mir/lower_hir.c` and its Runtime ABI declaration is
in `src/backend/llvm/runtime_calls.c`.
