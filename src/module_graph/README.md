# Module graph capability

The module graph assigns stable typed IDs to source-root-relative module paths and stores deduplicated import edges. Paths are validated by the Source capability before they become graph nodes.

Graph ordering uses an iterative depth-first traversal to avoid recursion on deep or adversarial import chains. Successful order places each dependency before its importer. A detected cycle returns the complete repeated-ID chain so the compiler can render every module path in the diagnostic.

`om_module_project_load` scans the module preamble for `IMPORT` declarations, decodes standard string escapes, classifies Source-Root-relative and explicit `./` Current-Module-relative literals, and normalizes both to a Source-Root-relative canonical path before validation and graph insertion. It loads each canonical source once and builds the graph iteratively. The scanner stops at the first declaration; later syntax validation is responsible for rejecting an `IMPORT` that appears after a declaration. A load error retains its module ID and source span so the diagnostic layer can point to either the failed import or malformed import text.

The project owns its graph, source texts, imports, and topological order. Call `om_module_project_destroy` after every load attempt. A cycle result keeps the graph and cycle chain available; `om_module_graph_cycle_message` renders the full path chain with ` -> ` separators.
