# Runtime capability

The runtime executes verified OMBC v3 images. The root action runs on the
calling thread; Actor handlers run in a bounded worker pool after the root
action returns.
`om_vm_init` validates the borrowed image before the VM retains it, and the
image must remain alive until `om_vm_destroy`. `om_vm_execute` starts a named
function by index and returns either its `Om_VM_Value` or a structured runtime
error with the function, bytecode position, and source span when available.

Values use an explicit tag and union. Heap values are immutable Strings, Lists,
Maps, Records, Messages, Function references, Results, and internal iterators.
Root execution values live in a VM-owned heap until VM destruction. Each Actor
has a separate heap that is traced at instruction safe points and released when
the Actor stops. Mailbox nodes own transit heaps; dispatch copies a message into
the receiving Actor's heap, so queued messages do not cross heap ownership.

Each VM has limits for operand-stack values, call frames, total heap bytes,
Actor-local GC threshold, total executed instructions, Actor instances,
queued mailbox messages, worker count, and per-turn instruction quantum.
Worker count defaults to four and is capped at 64; the instruction quantum
defaults to 256 bytecode instructions. Integer operations detect overflow;
invalid types, division by zero, bad indexes, bad Result access, and limit
violations produce structured errors without `longjmp`.

The Actor runtime provides VM-local `ActorRef` IDs, isolated state arrays, FIFO
mailboxes, local `SPAWN`, asynchronous `SEND`, `WATCH`, and `Actor_Down`. A
global ready queue and scheduler mutex ensure an Actor runs on at most one
worker at a time. Instruction quanta save and resume long-running handlers so
other Actors can progress. Set `worker_count` to one for deterministic
scheduling tests. `STOP` drops the Actor's queued messages; propagating an Error
from a Receive stops the Actor with reason `FAILED`.

Messages are copied into mailbox-owned transit heaps when sent, then copied
into the recipient Actor heap when dispatched. Actor heaps are collected at
instruction safe points and released when the Actor stops. Remote placement
(`SPAWN ... AT node`) allocates an Actor on a connected peer. Its initializer
is queued and runs when the peer host next calls `om_vm_execute`; remote `SEND`,
`WATCH`, Actor-down, and disconnect notifications use the ordinary mailbox
path. See `src/distribution/README.md` for transport and protocol limits.

## System Actors

`Console`, `File_System`, and `Timer` are built-in ActorRefs loaded by the
`LOAD_SYSTEM_ACTOR` bytecode instruction. Their request messages use the same
mailbox representation as user messages:

- `Console_Write(text, reply_to)` writes the exact UTF-8 text and replies with
  `Console_Write_Completed(Result<Unit, String>)`.
- `File_Read(path, reply_to)` reads UTF-8 text and replies with
  `File_Read_Completed(Result<String, String>)`.
- `File_Write(path, contents, reply_to)` overwrites a file with UTF-8 text and
  replies with `File_Write_Completed(Result<Unit, String>)`.
- `Timer_After(delay_milliseconds, message, recipient)` delivers the supplied
  Message to its recipient when the monotonic deadline expires.

Console and file operations run on the bounded blocking pool; Actor workers
only copy request data and submit work. The Timer uses a separate ordered
deadline queue, so a long delay does not occupy a blocking-pool worker. Native
tasks retain copied buffers and Actor IDs, never pointers into an Actor heap.
Their completions build a transit-heap Message and enqueue it through the
ordinary mailbox path. The scheduler stays alive while an external completion
is pending. On normal shutdown it drains work; after a scheduler failure it
cancels queued blocking tasks, waits for active native calls, and cancels
pending timers before releasing VM state.

## Native callback ingress

`om_vm_native_callback_post` is the thread-safe ingress for a Native callback
trampoline. It accepts a Message symbol and copied Bool, Int, and Float fields,
builds a transit-heap Message, and queues it through the ordinary mailbox path.
It never runs an Actor handler on the calling thread. Calls are accepted only
while `om_vm_execute` is active; the VM and callback target must remain alive
until the Native function has completed all callbacks. Pointer and heap-object
values are rejected at this boundary.
