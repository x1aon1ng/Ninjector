LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := Ninjector

LOCAL_SRC_FILES := \
    main.cpp \
    symbi/symbi_injector.cpp \
    process/process.cpp \
    ptrace/ptrace_arm64.cpp \
    injector/injector.cpp

LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_CPPFLAGS := -Os -std=c++17 -Werror=format

LOCAL_LDLIBS := -ldl -llog

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_MODULE := local_dobby
LOCAL_SRC_FILES := ../../TInjector-main/jni/core/$(TARGET_ARCH_ABI)/libdobby.a

include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)

LOCAL_MODULE := ncore

LOCAL_SRC_FILES := \
    ncore/ncore.cpp

LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_CPPFLAGS := -Os -std=c++17 -Werror=format
LOCAL_LDLIBS := -llog -ldl
LOCAL_STATIC_LIBRARIES := local_dobby

include $(BUILD_SHARED_LIBRARY)
