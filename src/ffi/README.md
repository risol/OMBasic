# FFI capability

The FFI capability parses `.omi` files independently from `.bas` syntax and
keeps C ABI declarations separate from OMBasic value types. `Om_Omi_Module`
contains borrowed identifier views into the input source, owned decoded string
literals, flat parameter/field tables, and source-spanned E0801–E0808 metadata
diagnostics. Destroy the module before destroying its source.

`abi_type` reports the current process C ABI size and alignment for every
supported metadata type. Native Record layout uses natural C struct alignment
and rejects empty records and `CVoid` fields. Cross-compiling layout for a
different target ABI is not supported by this API.

`dynamic_library` loads UTF-8 paths with `LoadLibraryW` on Windows and `dlopen`
on POSIX. Symbol lookup returns an opaque address; callers must use the
function signature already checked against `.omi` metadata. `c_string` copies
UTF-8 values across the C boundary with explicit byte limits, and owned pointer
cleanup uses the release symbol declared by the metadata.

The current tests load C and C++ `extern "C"` fixtures, call typed symbols,
check CString ownership, and verify callbacks from foreign threads. Runtime
callback trampolines submit copied scalar Message fields through
`om_vm_native_callback_post`; the Runtime queues them without executing Actor
handlers on the foreign thread. Resolver configuration accepts parsed `.omi`
modules, imports their supported function signatures and effects, and emits
fixed-arity external bytecode functions. `om_vm_bind_native_module` validates
bindings for those declarations after the compiler integration layer checks
callback Message types. `Om_Omi_Project` loads an explicit list of canonical
`.omi` paths under a source root and keeps the parsed modules and source buffers
alive together; callers pass those modules to resolver configuration. Actor
calls marked `BLOCKING` run in the bounded pool; their Actor context resumes
with the returned value when the task completes.

Native Records are supported as by-value parameters and results by the FFI
binding API. The binder builds libffi struct types from the metadata fields
and checks the resulting size and alignment against the host C ABI. Aggregate
roundtrip coverage includes a padded `{ int32_t, double }` record. This
marshalling is not yet exposed through BASIC source-level Native imports.
Nested Native Records and Record-valued callbacks are unsupported. Pointer
fields copy addresses only; the caller and Native wrapper remain responsible
for pointee lifetime and ownership.

The generated `.omi` from `ombasic bind` is a starting point, not an authority
for effect, blocking behavior, or pointer ownership. Review it against the C
API, then validate it with `ombasic check-omi`.
