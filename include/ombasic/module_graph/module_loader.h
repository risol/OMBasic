#ifndef OMBASIC_MODULE_GRAPH_MODULE_LOADER_H
#define OMBASIC_MODULE_GRAPH_MODULE_LOADER_H

#include <stdbool.h>
#include <stddef.h>

#include "ombasic/module_graph/module_graph.h"
#include "ombasic/source/file_loader.h"
#include "ombasic/source/span.h"

typedef struct Om_Module_Project {
    Om_Module_Graph graph;
    Om_Module_Graph_Order order;
    Om_Source_Span error_span;
    Om_Module_Id failed_module;
    void *owned_storage;
} Om_Module_Project;

typedef struct Om_Module_Import_Info {
    Om_Module_Id target_module;
    Om_String_View path;
    Om_Source_Span span;
    bool reexport;
} Om_Module_Import_Info;

/* Pass a zero-initialized project. Destroy it after every load attempt. */
Om_Status om_module_project_load(
    const Om_Source_Root *source_root,
    Om_String_View entry_path,
    size_t maximum_source_byte_length,
    Om_Module_Project *project
);
/* Like om_module_project_load, but uses the borrowed bytes for the entry file. */
Om_Status om_module_project_load_with_entry_source(
    const Om_Source_Root *source_root,
    Om_String_View entry_path,
    Om_Const_Byte_Slice entry_source,
    size_t maximum_source_byte_length,
    Om_Module_Project *project
);
/* Loads Project modules from source_root and @std modules from stdlib_root. */
Om_Status om_module_project_load_with_stdlib(
    const Om_Source_Root *source_root,
    const Om_Source_Root *stdlib_root,
    Om_String_View entry_path,
    size_t maximum_source_byte_length,
    Om_Module_Project *project
);
Om_Status om_module_project_load_with_entry_source_and_stdlib(
    const Om_Source_Root *source_root,
    const Om_Source_Root *stdlib_root,
    Om_String_View entry_path,
    Om_Const_Byte_Slice entry_source,
    size_t maximum_source_byte_length,
    Om_Module_Project *project
);
void om_module_project_destroy(Om_Module_Project *project);

/* Returns NULL for an invalid module ID or a source that failed to load. */
const Om_Source_Text *om_module_project_source(
    const Om_Module_Project *project,
    Om_Module_Id module_id
);
size_t om_module_project_module_count(const Om_Module_Project *project);
size_t om_module_project_import_count(
    const Om_Module_Project *project,
    Om_Module_Id importer
);
Om_Status om_module_project_import_at(
    const Om_Module_Project *project,
    Om_Module_Id importer,
    size_t index,
    Om_Module_Import_Info *import_info
);

#endif
