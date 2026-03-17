#ifndef ATTACH_INJECTOR_INJECTOR_INJECTOR_H
#define ATTACH_INJECTOR_INJECTOR_INJECTOR_H

#include <sys/types.h>

bool inject_so_by_pid(pid_t pid, const char* so_path);
bool prepare_spawn_in_zygote(pid_t zygote_pid,
                             const char* ncore_path,
                             const char* package_name,
                             const char* so_path);
bool clear_spawn_in_zygote(pid_t zygote_pid, const char* ncore_path);

#endif // ATTACH_INJECTOR_INJECTOR_INJECTOR_H
