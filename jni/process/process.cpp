#include "process.h"
#include "../common/log.h"

#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int get_pid(const char* process_name) {
    if (process_name == nullptr || process_name[0] == '\0') {
        LOGE("get_pid: invalid process name");
        return -1;
    }

    DIR* dir = opendir("/proc");
    if (dir == nullptr) {
        LOGE("get_pid: failed to open /proc");
        return -1;
    }

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        int pid = atoi(entry->d_name);
        if (pid <= 0) {
            continue;
        }

        char path[256] = {0};
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

        FILE* fp = fopen(path, "r");
        if (fp == nullptr) {
            continue;
        }

        char cmdline[256] = {0};
        fgets(cmdline, sizeof(cmdline), fp);
        fclose(fp);

        char* name = strrchr(cmdline, '/');
        if (name == nullptr) {
            name = cmdline;
        } else {
            name++;
        }

        if (strcmp(process_name, name) == 0) {
            closedir(dir);
            LOGI("get_pid: found pid=%d for process=%s", pid, process_name);
            return pid;
        }
    }

    closedir(dir);
    LOGE("get_pid: process not found: %s", process_name);
    return -1;
}

long get_module_base(pid_t pid, const char* module_name) {
    if (module_name == nullptr || module_name[0] == '\0') {
        LOGE("get_module_base: invalid module name");
        return 0;
    }

    char maps_path[64] = {0};
    if (pid == -1) {
        snprintf(maps_path, sizeof(maps_path), "/proc/self/maps");
    } else {
        snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    }

    FILE* fp = fopen(maps_path, "r");
    if (fp == nullptr) {
        LOGE("get_module_base: failed to open %s", maps_path);
        return 0;
    }

    char line[512] = {0};
    long base_addr = 0;
    while (fgets(line, sizeof(line), fp) != nullptr) {
        if (strstr(line, module_name) != nullptr) {
            char* start = strtok(line, "-");
            if (start != nullptr) {
                base_addr = strtoul(start, nullptr, 16);
                break;
            }
        }
    }

    fclose(fp);

    if (base_addr == 0) {
        LOGE("get_module_base: module not found: %s, pid=%d", module_name, pid);
    } else {
        LOGD("get_module_base: module=%s pid=%d base=%lx", module_name, pid, base_addr);
    }

    return base_addr;
}

const char* get_module_name(pid_t pid, uintptr_t addr) {
    char maps_path[64] = {0};
    if (pid == -1) {
        snprintf(maps_path, sizeof(maps_path), "/proc/self/maps");
    } else {
        snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    }

    FILE* fp = fopen(maps_path, "r");
    if (fp == nullptr) {
        LOGE("get_module_name: failed to open %s", maps_path);
        return nullptr;
    }

    char line[1024] = {0};
    while (fgets(line, sizeof(line), fp) != nullptr) {
        uintptr_t start = 0;
        uintptr_t end = 0;
        if (sscanf(line, "%lx-%lx", &start, &end) != 2) {
            continue;
        }

        if (addr >= start && addr < end) {
            char* path = strchr(line, '/');
            if (path != nullptr) {
                char* newline = strchr(path, '\n');
                if (newline != nullptr) {
                    *newline = '\0';
                }
                fclose(fp);
                LOGD("get_module_name: addr=%lx module=%s", (unsigned long) addr, path);
                return strdup(path);
            }
        }
    }

    fclose(fp);
    LOGE("get_module_name: module not found for addr=%lx", (unsigned long) addr);
    return nullptr;
}

long get_remote_addr(pid_t pid, void* local_func) {
    if (local_func == nullptr) {
        LOGE("get_remote_addr: local_func is null");
        return 0;
    }

    const char* module_path = get_module_name(-1, reinterpret_cast<uintptr_t>(local_func));
    if (module_path == nullptr) {
        LOGE("get_remote_addr: failed to get local module name");
        return 0;
    }

    long local_base = get_module_base(-1, module_path);
    long remote_base = get_module_base(pid, module_path);
    if (local_base == 0 || remote_base == 0) {
        LOGE("get_remote_addr: failed to get module base, module=%s local=%lx remote=%lx",
             module_path, local_base, remote_base);
        return 0;
    }

    long remote_addr = reinterpret_cast<long>(local_func) - local_base + remote_base;
    LOGD("get_remote_addr: module=%s local_func=%lx local_base=%lx remote_base=%lx remote_addr=%lx",
         module_path, reinterpret_cast<long>(local_func), local_base, remote_base, remote_addr);
    return remote_addr;
}
