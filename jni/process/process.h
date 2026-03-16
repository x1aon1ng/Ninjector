#ifndef ATTACH_INJECTOR_PROCESS_PROCESS_H
#define ATTACH_INJECTOR_PROCESS_PROCESS_H

#include <sys/types.h>
#include <cstdint>

int get_pid(const char* process_name);
long get_module_base(pid_t pid, const char* module_name);
const char* get_module_name(pid_t pid, uintptr_t addr);
long get_remote_addr(pid_t pid, void* local_func);

#endif // ATTACH_INJECTOR_PROCESS_PROCESS_H
