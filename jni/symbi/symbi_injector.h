#ifndef NINJECTOR_SYMBI_INJECTOR_H
#define NINJECTOR_SYMBI_INJECTOR_H

#include <sys/types.h>
#include <vector>

bool inject_spawn_symbi_by_package(pid_t zygote_pid,
                                   const char* package_name,
                                   const char* so_path);
bool inject_spawn_symbi_by_pids(const std::vector<pid_t>& pids,
                                const char* package_name,
                                const char* so_path);

#endif // NINJECTOR_SYMBI_INJECTOR_H
