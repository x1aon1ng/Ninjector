#ifndef ATTACH_INJECTOR_PTRACE_PTRACE_ARM64_H
#define ATTACH_INJECTOR_PTRACE_PTRACE_ARM64_H

#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <linux/uio.h>
#include <elf.h>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "../process/process.h"

#if !defined(__aarch64__)
#error "This file only supports aarch64"
#endif

#define REGS_ARG_NUM 8
#define pt_regs user_regs_struct
#define uregs regs
#define ARM_sp sp
#define ARM_lr regs[30]
#define ARM_pc pc
#define ARM_cpsr pstate

bool attach_process(pid_t pid);
bool detach_process(pid_t pid);
long xptrace(int request, ...);
void ptrace_read(pid_t pid, long address, uint8_t* buffer, size_t size);
void ptrace_write(pid_t pid, long address, void* data, size_t size);

template<typename Ret>
Ret call_remote_call(pid_t pid, long address, int argc, long* args) {
    pt_regs regs{};
    pt_regs backup_regs{};

    iovec regs_iov{
        .iov_base = &regs,
        .iov_len = sizeof(pt_regs)
    };
    iovec backup_iov{
        .iov_base = &backup_regs,
        .iov_len = sizeof(pt_regs)
    };

    xptrace(PTRACE_GETREGSET, pid, reinterpret_cast<void*>(NT_PRSTATUS), &regs_iov);
    backup_regs = regs;

    for (int i = 0; i < argc && i < REGS_ARG_NUM; ++i) {
        regs.uregs[i] = args[i];
    }

    if (argc > REGS_ARG_NUM) {
        size_t stack_size = (argc - REGS_ARG_NUM) * sizeof(long);
        regs.ARM_sp -= stack_size;
        ptrace_write(pid, regs.ARM_sp, args + REGS_ARG_NUM, stack_size);
    }

    regs.ARM_lr = 0;
    regs.ARM_pc = address;

#define CPSR_T_MASK (1u << 5)
    if (regs.ARM_pc & 1) {
        regs.ARM_pc &= (~1u);
        regs.ARM_cpsr |= CPSR_T_MASK;
    } else {
        regs.ARM_cpsr &= ~CPSR_T_MASK;
    }

    xptrace(PTRACE_SETREGSET, pid, reinterpret_cast<void*>(NT_PRSTATUS), &regs_iov);
    xptrace(PTRACE_CONT, pid, nullptr, nullptr);

    int status = 0;
    waitpid(pid, &status, WUNTRACED);
    while ((status & 0xFF) != 0x7f) {
        xptrace(PTRACE_CONT, pid, nullptr, nullptr);
        waitpid(pid, &status, WUNTRACED);
    }

    xptrace(PTRACE_GETREGSET, pid, reinterpret_cast<void*>(NT_PRSTATUS), &regs_iov);
    xptrace(PTRACE_SETREGSET, pid, reinterpret_cast<void*>(NT_PRSTATUS), &backup_iov);

    if constexpr (std::is_void_v<Ret>) {
        return;
    } else {
        return reinterpret_cast<Ret>(regs.uregs[0]);
    }
}

template<typename Ret, typename... Args>
Ret call_remote_function(pid_t pid, void* local_func, Args... args) {
    if (local_func == nullptr) {
        if constexpr (std::is_void_v<Ret>) {
            return;
        } else {
            return static_cast<Ret>(0);
        }
    }

    long remote_addr = get_remote_addr(pid, local_func);
    if (remote_addr == 0) {
        if constexpr (std::is_void_v<Ret>) {
            return;
        } else {
            return static_cast<Ret>(0);
        }
    }

    long params[sizeof...(Args) == 0 ? 1 : sizeof...(Args)] = {};
    int index = 0;
    ((params[index++] = (long) args), ...);

    return call_remote_call<Ret>(pid, remote_addr, sizeof...(Args), params);
}

#endif // ATTACH_INJECTOR_PTRACE_PTRACE_ARM64_H
