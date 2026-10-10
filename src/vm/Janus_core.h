#ifndef JANUS_CORE_H
#define JANUS_CORE_H

#include "vm_types.h"

void vm_exec(VM *vm, char *buffer);
void vm_dump(VM *vm);
void vm_free(VM *vm);

#endif /* JANUS_CORE_H */
