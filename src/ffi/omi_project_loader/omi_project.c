#include "ombasic/ffi/omi_project.h"

#include <string.h>

#include "ombasic/foundation/memory.h"

typedef struct Om_Omi_Project_Storage {
    Om_Source_Text *sources;
    Om_Omi_Module *modules;
    size_t module_count;
} Om_Omi_Project_Storage;

void om_omi_project_destroy(Om_Omi_Project *project)
{
    if (project == NULL) {
        return;
    }
    Om_Omi_Project_Storage *storage = project->owned_storage;
    if (storage != NULL) {
        for (size_t index = 0; index < storage->module_count; ++index) {
            om_omi_module_destroy(&storage->modules[index]);
            om_source_text_destroy(&storage->sources[index]);
        }
        om_free(storage->modules);
        om_free(storage->sources);
        om_free(storage);
    }
    *project = (Om_Omi_Project){0};
}

Om_Status om_omi_project_load(
    const Om_Source_Root *source_root,
    const Om_String_View *metadata_paths,
    size_t metadata_path_count,
    uint32_t first_source_id,
    size_t maximum_source_byte_length,
    Om_Omi_Project *project)
{
    if (source_root == NULL || source_root->owned_storage == NULL
        || project == NULL || project->owned_storage != NULL
        || (metadata_path_count != 0 && metadata_paths == NULL)
        || (metadata_path_count != 0 && first_source_id == UINT32_MAX)
        || metadata_path_count > (size_t)(UINT32_MAX - first_source_id)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (metadata_path_count == 0) {
        return OM_STATUS_OK;
    }
    Om_Omi_Project_Storage *storage = om_alloc(sizeof(*storage));
    if (storage == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *storage = (Om_Omi_Project_Storage){0};
    storage->sources = om_alloc_array(metadata_path_count,
                                      sizeof(*storage->sources));
    storage->modules = om_alloc_array(metadata_path_count,
                                      sizeof(*storage->modules));
    if (storage->sources == NULL || storage->modules == NULL) {
        om_free(storage->sources);
        om_free(storage->modules);
        om_free(storage);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memset(storage->sources, 0,
                 metadata_path_count * sizeof(*storage->sources));
    (void)memset(storage->modules, 0,
                 metadata_path_count * sizeof(*storage->modules));
    storage->module_count = metadata_path_count;
    project->sources = storage->sources;
    project->modules = storage->modules;
    project->module_count = metadata_path_count;
    project->owned_storage = storage;

    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < metadata_path_count; ++index) {
        for (size_t prior = 0; prior < index; ++prior) {
            if (om_string_view_equal(metadata_paths[index],
                                     metadata_paths[prior])) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
        }
        if (status != OM_STATUS_OK) {
            break;
        }
        status = om_source_root_load_file(
            source_root,
            (Om_Source_Id){first_source_id + (uint32_t)index},
            metadata_paths[index], (Om_String_View){".omi", 4},
            maximum_source_byte_length, &storage->sources[index]);
        if (status == OM_STATUS_OK) {
            status = om_omi_parse(&storage->sources[index],
                                  &storage->modules[index]);
        }
    }
    if (status != OM_STATUS_OK) {
        om_omi_project_destroy(project);
    }
    return status;
}

const Om_Omi_Module *om_omi_project_module(
    const Om_Omi_Project *project, size_t index)
{
    if (project == NULL || project->owned_storage == NULL
        || index >= project->module_count || project->modules == NULL) {
        return NULL;
    }
    return &project->modules[index];
}

const Om_Source_Text *om_omi_project_source(
    const Om_Omi_Project *project, size_t index)
{
    if (project == NULL || project->owned_storage == NULL
        || index >= project->module_count || project->sources == NULL) {
        return NULL;
    }
    return &project->sources[index];
}
