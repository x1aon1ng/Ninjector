#include "ncore.h"
#include "simple_hook.h"
#include "../common/log.h"

#include <jni.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>

#define NINJECTOR_RESULT_FILE "/data/local/tmp/Ninjector/spawn_result.json"

static char* g_target_package = nullptr;
static char* g_target_so = nullptr;
static void* g_android_os_Process_setArg = nullptr;
static void* g_selinux_android_setcontext = nullptr;
static bool g_payload_loaded = false;
static bool g_spawn_hooks_installed = false;

static void send_status_to_injector(const char* package_name, const char* so_path) {
    char payload[512] = {0};
    snprintf(payload,
             sizeof(payload),
             "{\"pid\":%d,\"pkg\":\"%s\",\"so\":\"%s\"}",
             getpid(),
             package_name != nullptr ? package_name : "",
             so_path != nullptr ? so_path : "");

    int fd = open(NINJECTOR_RESULT_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LOGE("ncore: open result file failed");
        return;
    }

    ssize_t written = write(fd, payload, strlen(payload));
    if (written < 0) {
        LOGE("ncore: write result file failed");
        close(fd);
        return;
    }

    LOGI("ncore: sent callback payload=%s", payload);
    close(fd);
}

static bool matches_target(const char* name) {
    bool matched = g_target_package != nullptr &&
                   g_target_so != nullptr &&
                   name != nullptr &&
                   strcmp(name, g_target_package) == 0;
    if (name != nullptr) {
        LOGD("ncore: compare target current=%s target=%s matched=%d",
             name,
             g_target_package != nullptr ? g_target_package : "(null)",
             matched ? 1 : 0);
    }
    return matched;
}

static void unload_target_state() {
    if (g_target_package != nullptr) {
        free(g_target_package);
        g_target_package = nullptr;
    }
    if (g_target_so != nullptr) {
        free(g_target_so);
        g_target_so = nullptr;
    }
    g_payload_loaded = false;
}

static void unhook_all() {
    DobbyDestroy(reinterpret_cast<void*>(fork));
    DobbyDestroy(reinterpret_cast<void*>(vfork));
    if (g_android_os_Process_setArg != nullptr) {
        DobbyDestroy(g_android_os_Process_setArg);
        g_android_os_Process_setArg = nullptr;
    }
    if (g_selinux_android_setcontext != nullptr) {
        DobbyDestroy(g_selinux_android_setcontext);
        g_selinux_android_setcontext = nullptr;
    }
    g_spawn_hooks_installed = false;
}

extern "C" void aclear() {
    LOGI("ncore: aclear");
    unhook_all();
    unload_target_state();
}

static bool load_payload_if_needed(const char* package_name) {
    if (!matches_target(package_name)) {
        return false;
    }

    if (g_payload_loaded) {
        LOGI("ncore: payload already loaded for %s", package_name);
        return true;
    }

    LOGI("ncore: target matched, loading payload package=%s so=%s",
         package_name != nullptr ? package_name : "(null)",
         g_target_so != nullptr ? g_target_so : "(null)");

    unhook_all();

    void* handle = dlopen(g_target_so, RTLD_NOW | RTLD_NODELETE | RTLD_GLOBAL);
    if (handle == nullptr) {
        LOGE("ncore: dlopen failed for %s: %s", g_target_so, dlerror());
        return false;
    }

    g_payload_loaded = true;
    LOGI("ncore: payload loaded for %s => %s", package_name, g_target_so);
    send_status_to_injector(package_name, g_target_so);
    return true;
}

DECLARE_HOOK(selinux_android_setcontext, int, uid_t uid, bool isSystemServer, const char* seinfo, const char* name) {
    int result = orig_selinux_android_setcontext(uid, isSystemServer, seinfo, name);
    LOGD("ncore: selinux_android_setcontext name=%s", name != nullptr ? name : "(null)");
    load_payload_if_needed(name);
    return result;
}

DECLARE_HOOK(android_os_Process_setArgV0, void, JNIEnv* env, jobject obj, jstring arg) {
    const char* package_name = env != nullptr && arg != nullptr ? env->GetStringUTFChars(arg, nullptr) : nullptr;
    if (orig_android_os_Process_setArgV0 != nullptr) {
        orig_android_os_Process_setArgV0(env, obj, arg);
    }

    LOGD("ncore: android_os_Process_setArgV0 arg=%s", package_name != nullptr ? package_name : "(null)");
    load_payload_if_needed(package_name);

    if (env != nullptr && arg != nullptr && package_name != nullptr) {
        env->ReleaseStringUTFChars(arg, package_name);
    }
}

static void install_child_hooks() {
    LOGI("ncore: installing child hooks pid=%d", getpid());

    g_android_os_Process_setArg = DobbySymbolResolver(
        "libandroid_runtime.so",
        "_Z27android_os_Process_setArgV0P7_JNIEnvP8_jobjectP8_jstring");
    if (g_android_os_Process_setArg != nullptr) {
        INSTALL_HOOK(android_os_Process_setArgV0, g_android_os_Process_setArg);
    } else {
        LOGE("ncore: android_os_Process_setArgV0 not found");
    }

    g_selinux_android_setcontext = DobbySymbolResolver("libselinux.so", "selinux_android_setcontext");
    if (g_selinux_android_setcontext != nullptr) {
        INSTALL_HOOK(selinux_android_setcontext, g_selinux_android_setcontext);
    } else {
        LOGE("ncore: selinux_android_setcontext not found");
    }
}

DECLARE_HOOK(fork, pid_t, void) {
    pid_t pid = orig_fork();
    if (pid == 0) {
        LOGI("ncore: child forked pid=%d", getpid());
        install_child_hooks();
    } else if (pid > 0) {
        LOGD("ncore: parent observed fork child=%d", pid);
    }
    return pid;
}

DECLARE_HOOK(vfork, pid_t, void) {
    LOGD("ncore: vfork called");
    return fake_fork();
}

extern "C" void ainject(const char* package_name, const char* so_path) {
    unload_target_state();

    if (package_name != nullptr && package_name[0] != '\0') {
        g_target_package = strdup(package_name);
    }
    if (so_path != nullptr && so_path[0] != '\0') {
        g_target_so = strdup(so_path);
    }

    LOGI("ncore: ainject package=%s so=%s",
         g_target_package != nullptr ? g_target_package : "(null)",
         g_target_so != nullptr ? g_target_so : "(null)");

    if (g_spawn_hooks_installed) {
        LOGI("ncore: spawn hooks already installed, skip");
        return;
    }

    INSTALL_HOOK(fork, fork);
    INSTALL_HOOK(vfork, vfork);
    g_spawn_hooks_installed = true;
    LOGI("ncore: spawn hooks installed");
}

__attribute__((destructor()))
static void ncore_cleanup() {
    LOGD("ncore: cleanup");
    unhook_all();
    unload_target_state();
}
