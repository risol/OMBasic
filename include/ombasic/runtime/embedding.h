#ifndef OMBASIC_RUNTIME_EMBEDDING_H
#define OMBASIC_RUNTIME_EMBEDDING_H

#include <stddef.h>

#include "ombasic/runtime/abi/base.h"

/* A zero threshold selects the Runtime default. Contexts are thread-confined
   unless an API explicitly creates a posting context for an Actor system. */
Om_Execution_Context *om_runtime_context_create(size_t collection_threshold);
/* Copies the process arguments into a context for Environment_Arguments. */
Om_Status om_runtime_context_set_command_line(
    Om_Execution_Context *context, int argc, const char *const argv[]);
void om_runtime_context_destroy(Om_Execution_Context *context);

/* Runs an entry through a recoverable Runtime fault boundary. */
int om_runtime_run(Om_Execution_Context *context,
                   Om_Runtime_Entry_Function entry);

#endif
