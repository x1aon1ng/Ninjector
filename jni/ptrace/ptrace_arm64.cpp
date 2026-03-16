#include "ptrace_arm64.h"
#include "../common/log.h"

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdarg>

long xptrace(int request, ...) {
    va_list args;
    va_start(args, request);

    pid_t pid = va_arg(args, pid_t);
    void* addr = va_arg(args, void*);
    void* data = va_arg(args, void*);

    errno = 0;
    long result = ptrace(request, pid, addr, data);

    va_end(args);

    if (result == -1 && errno != 0) {
        LOGE("ptrace failed: request=%d pid=%d errno=%d (%s)",
             request, pid, errno, strerror(errno));
    }

    return result;
}

bool attach_process(pid_t pid) {
    if (pid <= 0) {
        LOGE("attach_process: invalid pid=%d", pid);
        return false;
    }

    if (xptrace(PTRACE_ATTACH, pid, nullptr, nullptr) == -1) {
        LOGE("attach_process: PTRACE_ATTACH failed, pid=%d", pid);
        return false;
    }

    int status = 0;
    if (waitpid(pid, &status, WUNTRACED) == -1) {
        LOGE("attach_process: waitpid failed, pid=%d errno=%d (%s)",
             pid, errno, strerror(errno));
        return false;
    }

    LOGI("attach_process: attached to pid=%d status=0x%x", pid, status);
    return true;
}

bool detach_process(pid_t pid) {
    if (pid <= 0) {
        LOGE("detach_process: invalid pid=%d", pid);
        return false;
    }

    if (xptrace(PTRACE_DETACH, pid, nullptr, nullptr) == -1) {
        LOGE("detach_process: PTRACE_DETACH failed, pid=%d", pid);
        return false;
    }

    LOGI("detach_process: detached from pid=%d", pid);
    return true;
}

void ptrace_read(pid_t pid, long address, uint8_t* buffer, size_t size) {
    if (pid <= 0 || address == 0 || buffer == nullptr || size == 0) {
        LOGE("ptrace_read: invalid args pid=%d address=%lx size=%zu", pid, address, size);
        return;
    }

    memset(buffer, 0, size);

    const size_t word_size = sizeof(unsigned long);
    size_t full_words = size / word_size;
    size_t remain = size % word_size;

    for (size_t i = 0; i < full_words; ++i) {
        unsigned long word = static_cast<unsigned long>(
            xptrace(PTRACE_PEEKDATA, pid, reinterpret_cast<void*>(address + i * word_size), nullptr)
        );
        memcpy(buffer + i * word_size, &word, word_size);
    }

    if (remain > 0) {
        unsigned long word = static_cast<unsigned long>(
            xptrace(PTRACE_PEEKDATA, pid, reinterpret_cast<void*>(address + full_words * word_size), nullptr)
        );
        memcpy(buffer + full_words * word_size, &word, remain);
    }
}

void ptrace_write(pid_t pid, long address, void* data, size_t size) {
    if (pid <= 0 || address == 0 || data == nullptr || size == 0) {
        LOGE("ptrace_write: invalid args pid=%d address=%lx size=%zu", pid, address, size);
        return;
    }

    const size_t word_size = sizeof(unsigned long);
    size_t full_words = size / word_size;
    size_t remain = size % word_size;

    auto* bytes = reinterpret_cast<unsigned char*>(data);
    for (size_t i = 0; i < full_words; ++i) {
        unsigned long word = *reinterpret_cast<unsigned long*>(bytes + i * word_size);
        xptrace(PTRACE_POKEDATA,
                pid,
                reinterpret_cast<void*>(address + i * word_size),
                reinterpret_cast<void*>(word));
    }

    if (remain > 0) {
        long tail_addr = address + full_words * word_size;
        unsigned long word = static_cast<unsigned long>(
            xptrace(PTRACE_PEEKDATA, pid, reinterpret_cast<void*>(tail_addr), nullptr)
        );
        memcpy(&word, bytes + full_words * word_size, remain);
        xptrace(PTRACE_POKEDATA,
                pid,
                reinterpret_cast<void*>(tail_addr),
                reinterpret_cast<void*>(word));
    }
}
