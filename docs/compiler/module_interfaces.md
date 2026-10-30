# Module interfaces

Each `.bas` file remains one module. Its public interface is the deterministic
set of declarations marked `EXPORT`, plus public declarations explicitly
included through `EXPORT IMPORT`. Re-exports retain the original symbol ID,
type scheme, effect data, protocol information, and definition location; they
do not add runtime declarations or calls.

The compiler exposes this public surface through interface metadata and uses
the same export table for module member lookup, LSP completion, hover,
go-to-definition, and diagnostics. A Facade entry uses the exported name while
definition links point to the source declaration that owns the symbol.

The interface fingerprint includes the module's public names, symbol kinds,
inferred types, effects, serializability properties, and Actor protocol
contracts. It does not include private implementation source content. Changing
a private function body therefore leaves the public fingerprint stable, while
changing a public contract changes it.

An Actor can receive an exported message through an imported module alias. For
example, `RECEIVE library.Ping(value)` resolves `Ping` from the public interface
of `library` and adds that message to the receiving Actor's protocol. The
message must be exported by the imported module or one of its facades.

The fingerprint is an incremental tooling contract, not a package version or a
replacement for pinning source revisions. Source libraries remain ordinary
files under the consuming project's Source Root.
