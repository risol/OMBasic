#ifndef OMBASIC_RUNTIME_EMBEDDING_H
#define OMBASIC_RUNTIME_EMBEDDING_H

#include <stddef.h>

#include "ombasic/runtime/abi/base.h"

/* A zero threshold selects the Runtime default. Contexts are thread-confined
   unless an API explicitly creates a posting context for an Actor system. */
Om_Execution_Context *om_runtime_context_create(size_t collection_threshold);
void om_runtime_context_destroy(Om_Execution_Context *context);

/* Runs an entry through a recoverable Runtime fault boundary. */
int om_runtime_run(Om_Execution_Context *context,
                   Om_Runtime_Entry_Function entry);

#endif
