#ifndef ATTACH_INJECTOR_COMMON_LOG_H
#define ATTACH_INJECTOR_COMMON_LOG_H

#include <android/log.h>
#include <cstdio>

#define TAG "Ninjector"

#define LOGD(...) ((void)__android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__))
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__))

#endif // ATTACH_INJECTOR_COMMON_LOG_H
