#include "injector.h"
#include "../common/log.h"
#include "../ptrace/ptrace_arm64.h"

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

static void* remote_alloc_string(pid_t pid, const char* str) {
    if (pid <= 0 || str == nullptr || str[0] == '\0') {
        LOGE("remote_alloc_string: invalid args");
        return nullptr;
    }

    size_t len = strlen(str) + 1;
    void* remote_buf = call_remote_function<void*, size_t>(
        pid,
        reinterpret_cast<void*>(malloc),
        len
    );
    if (remote_buf == nullptr) {
        LOGE("remote_alloc_string: remote malloc failed");
        return nullptr;
    }

    ptrace_write(pid, reinterpret_cast<long>(remote_buf), const_cast<char*>(str), len);
    LOGD("remote_alloc_string: wrote string to remote addr=%p", remote_buf);
    return remote_buf;
}

static void* inject_so_handle_by_pid(pid_t pid, const char* so_path) {
    if (pid <= 0 || so_path == nullptr || so_path[0] == '\0') {
        LOGE("inject_so_handle_by_pid: invalid args");
        return nullptr;
    }

    bool attached = false;
    void* remote_path = nullptr;
    void* handle = nullptr;

    if (!attach_process(pid)) {
        LOGE("inject_so_handle_by_pid: attach failed, pid=%d", pid);
        return nullptr;
    }
    attached = true;
    LOGI("inject_so_handle_by_pid: attached to pid=%d", pid);

    remote_path = remote_alloc_string(pid, so_path);
    if (remote_path == nullptr) {
        LOGE("inject_so_handle_by_pid: remote_alloc_string failed");
        goto fail;
    }

    handle = call_remote_function<void*, const char*, int>(
        pid,
        reinterpret_cast<void*>(dlopen),
        reinterpret_cast<const char*>(remote_path),
        RTLD_NOW | RTLD_GLOBAL
    );
    if (handle == nullptr) {
        LOGE("inject_so_handle_by_pid: remote dlopen failed");

        char* remote_error = call_remote_function<char*>(
            pid,
            reinterpret_cast<void*>(dlerror)
        );
        if (remote_error != nullptr) {
            char error_buf[512] = {0};
            ptrace_read(pid,
                        reinterpret_cast<long>(remote_error),
                        reinterpret_cast<uint8_t*>(error_buf),
                        sizeof(error_buf) - 1);
            LOGE("inject_so_handle_by_pid: dlerror=%s", error_buf);
        } else {
            LOGE("inject_so_handle_by_pid: dlerror returned null");
        }

        goto fail;
    }

    LOGI("inject_so_handle_by_pid: remote dlopen success, handle=%p", handle);

    call_remote_function<void, void*>(
        pid,
        reinterpret_cast<void*>(free),
        remote_path
    );
    remote_path = nullptr;

    if (!detach_process(pid)) {
        LOGE("inject_so_handle_by_pid: detach failed after success");
        return nullptr;
    }

    LOGI("inject_so_handle_by_pid: inject success");
    return handle;

fail:
    if (remote_path != nullptr) {
        call_remote_function<void, void*>(
            pid,
            reinterpret_cast<void*>(free),
            remote_path
        );
    }

    if (attached) {
        detach_process(pid);
    }
    return nullptr;
}

bool inject_so_by_pid(pid_t pid, const char* so_path) {
    return inject_so_handle_by_pid(pid, so_path) != nullptr;
}

bool prepare_spawn_in_zygote(pid_t zygote_pid,
                             const char* ncore_path,
                             const char* package_name,
                             const char* so_path) {
    if (zygote_pid <= 0 ||
        ncore_path == nullptr || ncore_path[0] == '\0' ||
        package_name == nullptr || package_name[0] == '\0' ||
        so_path == nullptr || so_path[0] == '\0') {
        LOGE("prepare_spawn_in_zygote: invalid args");
        return false;
    }

    bool attached = false;
    void* handle = nullptr;
    void* remote_sym_name = nullptr;
    void* remote_pkg = nullptr;
    void* remote_so = nullptr;
    void* remote_ainject = nullptr;
    long params[2] = {0, 0};

    handle = inject_so_handle_by_pid(zygote_pid, ncore_path);
    if (handle == nullptr) {
        LOGE("prepare_spawn_in_zygote: inject ncore failed");
        return false;
    }

    if (!attach_process(zygote_pid)) {
        LOGE("prepare_spawn_in_zygote: re-attach zygote failed");
        return false;
    }
    attached = true;

    remote_sym_name = remote_alloc_string(zygote_pid, "ainject");
    remote_pkg = remote_alloc_string(zygote_pid, package_name);
    remote_so = remote_alloc_string(zygote_pid, so_path);
    if (remote_sym_name == nullptr || remote_pkg == nullptr || remote_so == nullptr) {
        LOGE("prepare_spawn_in_zygote: remote string allocation failed");
        goto fail;
    }

    remote_ainject = call_remote_function<void*, void*, const char*>(
        zygote_pid,
        reinterpret_cast<void*>(dlsym),
        handle,
        reinterpret_cast<const char*>(remote_sym_name)
    );
    if (remote_ainject == nullptr) {
        LOGE("prepare_spawn_in_zygote: dlsym(ainject) failed");
        goto fail;
    }

    params[0] = reinterpret_cast<long>(remote_pkg);
    params[1] = reinterpret_cast<long>(remote_so);
    call_remote_call<void>(zygote_pid, reinterpret_cast<long>(remote_ainject), 2, params);

    call_remote_function<void, void*>(
        zygote_pid,
        reinterpret_cast<void*>(free),
        remote_sym_name
    );
    call_remote_function<void, void*>(
        zygote_pid,
        reinterpret_cast<void*>(free),
        remote_pkg
    );
    call_remote_function<void, void*>(
        zygote_pid,
        reinterpret_cast<void*>(free),
        remote_so
    );

    if (!detach_process(zygote_pid)) {
        LOGE("prepare_spawn_in_zygote: detach failed");
        return false;
    }

    LOGI("prepare_spawn_in_zygote: ncore prepared in zygote pid=%d", zygote_pid);
    return true;

fail:
    if (remote_sym_name != nullptr) {
        call_remote_function<void, void*>(
            zygote_pid,
            reinterpret_cast<void*>(free),
            remote_sym_name
        );
    }
    if (remote_pkg != nullptr) {
        call_remote_function<void, void*>(
            zygote_pid,
            reinterpret_cast<void*>(free),
            remote_pkg
        );
    }
    if (remote_so != nullptr) {
        call_remote_function<void, void*>(
            zygote_pid,
            reinterpret_cast<void*>(free),
            remote_so
        );
    }
    if (attached) {
        detach_process(zygote_pid);
    }
    return false;
}

bool clear_spawn_in_zygote(pid_t zygote_pid, const char* ncore_path) {
    if (zygote_pid <= 0 || ncore_path == nullptr || ncore_path[0] == '\0') {
        LOGE("clear_spawn_in_zygote: invalid args");
        return false;
    }

    bool attached = false;
    void* handle = nullptr;
    void* remote_sym_name = nullptr;
    void* remote_aclear = nullptr;

    handle = inject_so_handle_by_pid(zygote_pid, ncore_path);
    if (handle == nullptr) {
        LOGE("clear_spawn_in_zygote: inject ncore failed");
        return false;
    }

    if (!attach_process(zygote_pid)) {
        LOGE("clear_spawn_in_zygote: re-attach zygote failed");
        return false;
    }
    attached = true;

    remote_sym_name = remote_alloc_string(zygote_pid, "aclear");
    if (remote_sym_name == nullptr) {
        LOGE("clear_spawn_in_zygote: alloc sym name failed");
        goto fail;
    }

    remote_aclear = call_remote_function<void*, void*, const char*>(
        zygote_pid,
        reinterpret_cast<void*>(dlsym),
        handle,
        reinterpret_cast<const char*>(remote_sym_name)
    );
    if (remote_aclear == nullptr) {
        LOGE("clear_spawn_in_zygote: dlsym(aclear) failed");
        goto fail;
    }

    call_remote_call<void>(zygote_pid, reinterpret_cast<long>(remote_aclear), 0, nullptr);

    call_remote_function<void, void*>(
        zygote_pid,
        reinterpret_cast<void*>(free),
        remote_sym_name
    );

    if (!detach_process(zygote_pid)) {
        LOGE("clear_spawn_in_zygote: detach failed");
        return false;
    }

    LOGI("clear_spawn_in_zygote: ncore cleared in zygote pid=%d", zygote_pid);
    return true;

fail:
    if (remote_sym_name != nullptr) {
        call_remote_function<void, void*>(
            zygote_pid,
            reinterpret_cast<void*>(free),
            remote_sym_name
        );
    }
    if (attached) {
        detach_process(zygote_pid);
    }
    return false;
}
