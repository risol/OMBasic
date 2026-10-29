# Module graph capability

The module graph assigns stable typed IDs to modules and stores deduplicated import edges. A module identity includes its root kind (`project` or `stdlib`) and its canonical root-relative `.bas` path. This keeps a project module such as `math/math.bas` distinct from the SDK module `@std/math/math.bas`. Paths are validated before they become graph nodes.

Graph ordering uses an iterative depth-first traversal to avoid recursion on deep or adversarial import chains. Successful order places each dependency before its importer. A detected cycle returns the complete repeated-ID chain so the compiler can render every module path in the diagnostic.

`om_module_project_load` loads project-root imports. `om_module_project_load_with_stdlib` and `om_module_project_load_with_entry_source_and_stdlib` additionally accept the SDK root. The import scanner decodes standard string escapes and classifies project-root-relative, explicit `./` current-module-relative, and `@std/` SDK-root-relative literals. Project-relative forms resolve only against the project root; `@std/` resolves only against the SDK root and retains that prefix in the logical canonical path. An absent SDK root or module is an error: there is no search or fallback across roots. Only a single leading `./` is allowed for current-module-relative paths; other `.`/`..` path segments and omitted `.bas` extensions are rejected.

Each canonical module source is loaded once and the graph is built iteratively. The scanner stops at the first declaration; later syntax validation is responsible for rejecting an `IMPORT` that appears after a declaration. A load error retains its module ID and source span so the diagnostic layer can point to either the failed import or malformed import text.

The project owns its graph, source texts, imports, and topological order. Call `om_module_project_destroy` after every load attempt. A cycle result keeps the graph and cycle chain available; `om_module_graph_cycle_message` renders the full path chain with ` -> ` separators.
