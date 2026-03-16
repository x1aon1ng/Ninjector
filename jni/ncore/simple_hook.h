#ifndef NINJECTOR_NCORE_SIMPLE_HOOK_H
#define NINJECTOR_NCORE_SIMPLE_HOOK_H

#include "../common/log.h"
#include "../../../TInjector-main/jni/core/dobby/dobby.h"

#define DECLARE_HOOK(name, ret_type, ...) \
    using name##_t = ret_type(*)(__VA_ARGS__); \
    static name##_t orig_##name = nullptr; \
    static ret_type fake_##name(__VA_ARGS__)

#define INSTALL_HOOK(name, addr) \
    do { \
        if (DobbyHook(reinterpret_cast<void*>(addr), \
                      reinterpret_cast<void*>(fake_##name), \
                      reinterpret_cast<void**>(&orig_##name)) != 0) { \
            LOGE("hook: failed to hook %s", #name); \
        } else { \
            LOGI("hook: hooked %s", #name); \
        } \
    } while (0)

#endif // NINJECTOR_NCORE_SIMPLE_HOOK_H
