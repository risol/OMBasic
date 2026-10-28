#ifndef OMBASIC_TOOLCHAIN_RESOURCES_H
#define OMBASIC_TOOLCHAIN_RESOURCES_H

#include "ombasic/foundation/status.h"

#define OM_TOOLCHAIN_RUNTIME_ARCHIVE_COUNT 5

typedef struct Om_Toolchain_Resources {
    char *resource_root;
    char *target_triple;
    char *target_runtime_root;
    char *runtime_archive;
    char *runtime_services_archive;
    char *runtime_distribution_archive;
    char *distribution_archive;
    char *foundation_archive;
    char *missing_archive_path;
} Om_Toolchain_Resources;

/* Resolve a runtime from an explicit root, OMBASIC_RESOURCE_DIR, or the
   executable-relative install/build layout. */
Om_Status om_toolchain_runtime_resources_resolve(
    const char *resource_dir_override,
    const char *target_triple,
    Om_Toolchain_Resources *resources);

/* Resolve a non-runtime SDK file (for example share/ombasic/tools/...). */
Om_Status om_toolchain_resource_path(const char *relative_path,
                                     char **path_out);

void om_toolchain_resources_destroy(Om_Toolchain_Resources *resources);

#endif
