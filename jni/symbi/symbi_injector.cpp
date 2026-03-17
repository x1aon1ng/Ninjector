#include "symbi_injector.h"
#include "symbi_stub.h"
#include "../common/log.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <elf.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#define SYMBI_TARGET_SYMBOL "_Z27android_os_Process_setArgV0P7_JNIEnvP8_jobjectP8_jstring"

namespace {

volatile sig_atomic_t g_keep_running = 1;

struct MemoryMap {
    uintptr_t start = 0;
    uintptr_t end = 0;
    char perms[5] = {0};
    size_t offset = 0;
    std::string pathname;
};

struct SymbiContext {
    pid_t zygote_pid = -1;
    uid_t target_uid = static_cast<uid_t>(-1);
    uintptr_t shellcode_base = 0;
    uintptr_t set_argv0_address = 0;
    uintptr_t art_method_slot = 0;
    uintptr_t original_ptr = 0;
    uintptr_t remote_getuid = 0;
    uintptr_t remote_dlopen = 0;
    uintptr_t remote_log_print = 0;
    std::string libandroid_runtime_path;
    std::string shellcode_map_path;
    uintptr_t shellcode_map_start = 0;
    size_t shellcode_map_offset = 0;
    std::string remote_so_path;
    std::vector<uint8_t> original_shellcode_area;
};

void signal_handler(int) {
    g_keep_running = 0;
}

std::vector<MemoryMap> get_process_maps(pid_t pid) {
    std::vector<MemoryMap> maps;
    char path[64] = {0};
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        MemoryMap map{};
        char perms[5] = {0};
        char dev[16] = {0};
        char path_buf[512] = {0};
        unsigned long inode = 0;
        if (sscanf(line.c_str(),
                   "%lx-%lx %4s %lx %15s %lu %511s",
                   &map.start,
                   &map.end,
                   perms,
                   &map.offset,
                   dev,
                   &inode,
                   path_buf) >= 6) {
            memcpy(map.perms, perms, sizeof(map.perms));
            map.pathname = path_buf;
            maps.push_back(map);
        }
    }
    return maps;
}

uintptr_t get_symbol_offset_from_elf(const std::string& elf_path, const char* symbol_name) {
    int fd = open(elf_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return 0;
    }

    void* map_base = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map_base == MAP_FAILED) {
        return 0;
    }

    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(map_base);
    auto* shdr = reinterpret_cast<Elf64_Shdr*>(reinterpret_cast<uintptr_t>(map_base) + ehdr->e_shoff);

    uintptr_t symbol_offset = 0;
    for (int i = 0; i < ehdr->e_shnum; ++i) {
        if (shdr[i].sh_type != SHT_DYNSYM) {
            continue;
        }

        auto* syms = reinterpret_cast<Elf64_Sym*>(reinterpret_cast<uintptr_t>(map_base) + shdr[i].sh_offset);
        int count = static_cast<int>(shdr[i].sh_size / sizeof(Elf64_Sym));
        auto* strtab = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(map_base) + shdr[shdr[i].sh_link].sh_offset);
        for (int j = 0; j < count; ++j) {
            if (strcmp(strtab + syms[j].st_name, symbol_name) == 0) {
                symbol_offset = syms[j].st_value;
                break;
            }
        }
        if (symbol_offset != 0) {
            break;
        }
    }

    uintptr_t load_bias = 0;
    auto* phdr = reinterpret_cast<Elf64_Phdr*>(reinterpret_cast<uintptr_t>(map_base) + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; ++i) {
        if (phdr[i].p_type == PT_LOAD) {
            load_bias = phdr[i].p_vaddr;
            break;
        }
    }

    munmap(map_base, static_cast<size_t>(st.st_size));
    if (symbol_offset < load_bias) {
        return 0;
    }
    return symbol_offset - load_bias;
}

uintptr_t get_module_base(pid_t pid, const std::string& lib_name) {
    char path[64] = {0};
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    std::ifstream maps(path);
    std::string line;

    while (std::getline(maps, line)) {
        if (line.find(lib_name) == std::string::npos) {
            continue;
        }

        uintptr_t start = 0;
        uintptr_t offset = 0;
        char perms[5] = {0};
        if (sscanf(line.c_str(), "%lx-%*x %4s %lx", &start, perms, &offset) == 3) {
            if (offset == 0 && perms[3] != 's') {
                return start;
            }
        }
    }

    return 0;
}

uintptr_t get_remote_symbol(pid_t pid, const std::string& lib_name, const char* symbol) {
    uintptr_t base = get_module_base(pid, lib_name);
    if (base == 0) {
        return 0;
    }

    auto maps = get_process_maps(pid);
    std::string local_path;
    for (const auto& map : maps) {
        if (map.pathname.find(lib_name) != std::string::npos) {
            local_path = map.pathname;
            break;
        }
    }
    if (local_path.empty()) {
        return 0;
    }

    uintptr_t offset = get_symbol_offset_from_elf(local_path, symbol);
    if (offset == 0) {
        return 0;
    }
    return base + offset;
}

uid_t get_uid_from_package(const char* package_name) {
    std::ifstream pkg_file("/data/system/packages.list");
    if (!pkg_file.is_open()) {
        return static_cast<uid_t>(-1);
    }

    std::string line;
    while (std::getline(pkg_file, line)) {
        if (line.find(package_name) == std::string::npos) {
            continue;
        }

        std::stringstream ss(line);
        std::string pkg;
        uid_t uid = static_cast<uid_t>(-1);
        if (ss >> pkg >> uid) {
            if (pkg == package_name) {
                return uid;
            }
        }
    }

    return static_cast<uid_t>(-1);
}

bool stop_process(pid_t pid) {
    if (kill(pid, SIGSTOP) != 0) {
        LOGE("symbi: SIGSTOP failed pid=%d errno=%d", pid, errno);
        return false;
    }

    LOGI("symbi: stopped pid=%d via SIGSTOP", pid);
    return true;
}

void resume_process(pid_t pid) {
    if (kill(pid, SIGCONT) != 0) {
        LOGE("symbi: SIGCONT failed pid=%d errno=%d", pid, errno);
        return;
    }
    LOGI("symbi: resumed pid=%d via SIGCONT", pid);
}

int open_remote_mem(pid_t pid) {
    char path[64] = {0};
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        LOGE("symbi: failed to open %s errno=%d", path, errno);
    }
    return fd;
}

bool load_original_shellcode_page(const SymbiContext& ctx, std::vector<uint8_t>* buffer) {
    if (ctx.shellcode_map_path.empty() || ctx.shellcode_base == 0) {
        return false;
    }

    buffer->assign(stub_binary_size, 0);
    size_t file_offset = ctx.shellcode_map_offset +
                         static_cast<size_t>(ctx.shellcode_base - ctx.shellcode_map_start);
    int fd = open(ctx.shellcode_map_path.c_str(), O_RDONLY);
    if (fd < 0) {
        LOGE("symbi: failed to open shellcode backing file %s", ctx.shellcode_map_path.c_str());
        return false;
    }

    ssize_t read_size = pread(fd, buffer->data(), buffer->size(), static_cast<off_t>(file_offset));
    close(fd);
    if (read_size != static_cast<ssize_t>(buffer->size())) {
        LOGE("symbi: failed to read original shellcode page from file=%s offset=0x%zx",
             ctx.shellcode_map_path.c_str(), file_offset);
        return false;
    }

    return true;
}

bool prepare_target_so_for_app(uid_t target_uid,
                               const char* package_name,
                               const char* so_path,
                               std::string* remote_so_path) {
    std::string target_dir = std::string("/data/data/") + package_name + "/cache";
    *remote_so_path = target_dir + "/lib" + std::to_string(target_uid) + ".so";

    std::string cp_cmd = std::string("cp ") + so_path + " " + *remote_so_path;
    std::string chown_cmd = std::string("chown ") + std::to_string(target_uid) + ":" +
                            std::to_string(target_uid) + " " + *remote_so_path;
    std::string chmod_cmd = std::string("chmod 755 ") + *remote_so_path;

    int cp_ret = system(cp_cmd.c_str());
    int chown_ret = system(chown_cmd.c_str());
    int chmod_ret = system(chmod_cmd.c_str());
    LOGI("symbi: cp ret=%d chown ret=%d chmod ret=%d path=%s",
         cp_ret, chown_ret, chmod_ret, remote_so_path->c_str());
    return cp_ret == 0 && chown_ret == 0 && chmod_ret == 0;
}

bool collect_symbi_context(pid_t zygote_pid,
                           const char* package_name,
                           const char* so_path,
                           SymbiContext* ctx) {
    ctx->zygote_pid = zygote_pid;
    ctx->target_uid = get_uid_from_package(package_name);
    if (ctx->target_uid == static_cast<uid_t>(-1)) {
        LOGE("symbi: failed to resolve package uid package=%s", package_name);
        return false;
    }

    auto maps = get_process_maps(zygote_pid);
    std::vector<MemoryMap> heap_candidates;
    for (const auto& map : maps) {
        if (ctx->libandroid_runtime_path.empty() &&
            map.pathname.find("libandroid_runtime.so") != std::string::npos) {
            ctx->libandroid_runtime_path = map.pathname;
        }
        if (ctx->shellcode_base == 0 &&
            map.pathname.find("libstagefright.so") != std::string::npos &&
            map.perms[2] == 'x') {
            ctx->shellcode_base = map.end - static_cast<uintptr_t>(getpagesize());
            ctx->shellcode_map_path = map.pathname;
            ctx->shellcode_map_start = map.start;
            ctx->shellcode_map_offset = map.offset;
        }
        if ((map.pathname.find("boot.art") != std::string::npos ||
             map.pathname.find("boot-framework.art") != std::string::npos ||
             map.pathname.find("dalvik-LinearAlloc") != std::string::npos) &&
            map.perms[0] == 'r' && map.perms[1] == 'w') {
            heap_candidates.push_back(map);
        }
    }

    if (ctx->libandroid_runtime_path.empty() || ctx->shellcode_base == 0) {
        LOGE("symbi: failed to locate libandroid_runtime/libstagefright");
        return false;
    }

    uintptr_t libandroid_runtime_base = get_module_base(zygote_pid, ctx->libandroid_runtime_path);
    uintptr_t symbol_offset = get_symbol_offset_from_elf(ctx->libandroid_runtime_path, SYMBI_TARGET_SYMBOL);
    if (libandroid_runtime_base == 0 || symbol_offset == 0) {
        LOGE("symbi: failed to resolve setArgV0");
        return false;
    }
    ctx->set_argv0_address = libandroid_runtime_base + symbol_offset;

    ctx->remote_getuid = get_remote_symbol(zygote_pid, "libc.so", "getuid");
    ctx->remote_dlopen = get_remote_symbol(zygote_pid, "libdl.so", "dlopen");
    ctx->remote_log_print = get_remote_symbol(zygote_pid, "liblog.so", "__android_log_print");
    if (ctx->remote_getuid == 0 || ctx->remote_dlopen == 0 || ctx->remote_log_print == 0) {
        LOGE("symbi: failed to resolve remote helper symbols");
        return false;
    }

    int mem_fd = open_remote_mem(zygote_pid);
    if (mem_fd < 0) {
        return false;
    }

    for (const auto& heap : heap_candidates) {
        size_t region_size = static_cast<size_t>(heap.end - heap.start);
        if (region_size < sizeof(uintptr_t)) {
            continue;
        }

        std::vector<uint8_t> buffer(region_size);
        ssize_t read_size = pread(mem_fd, buffer.data(), region_size, static_cast<off_t>(heap.start));
        if (read_size != static_cast<ssize_t>(region_size)) {
            continue;
        }

        auto* found = reinterpret_cast<uint8_t*>(
            memmem(buffer.data(), region_size, &ctx->set_argv0_address, sizeof(ctx->set_argv0_address)));
        if (found != nullptr) {
            ctx->art_method_slot = heap.start + static_cast<uintptr_t>(found - buffer.data());
            break;
        }
    }

    if (ctx->art_method_slot == 0) {
        close(mem_fd);
        LOGE("symbi: failed to find art_method_slot for setArgV0=0x%lx", ctx->set_argv0_address);
        return false;
    }

    if (pread(mem_fd, &ctx->original_ptr, sizeof(ctx->original_ptr), static_cast<off_t>(ctx->art_method_slot)) !=
        static_cast<ssize_t>(sizeof(ctx->original_ptr))) {
        close(mem_fd);
        LOGE("symbi: failed to read original art_method slot");
        return false;
    }

    if (!load_original_shellcode_page(*ctx, &ctx->original_shellcode_area)) {
        close(mem_fd);
        return false;
    }

    close(mem_fd);
    LOGI("symbi: prepared zygote=%d uid=%d setArgV0=0x%lx slot=0x%lx original=0x%lx shellcode=0x%lx so=%s",
         zygote_pid,
         static_cast<int>(ctx->target_uid),
         ctx->set_argv0_address,
         ctx->art_method_slot,
         ctx->original_ptr,
         ctx->shellcode_base,
         so_path);
    return true;
}

bool write_stub_and_patch_slot(int mem_fd, const SymbiContext& ctx) {
    char remote_pattern[] = "/ningningning123123";
    uintptr_t marker = reinterpret_cast<uintptr_t>(
        memmem(stub_binary, stub_binary_size, remote_pattern, sizeof(remote_pattern)));
    if (marker == 0) {
        LOGE("symbi: failed to locate stub marker");
        return false;
    }

    std::vector<uint8_t> stub_copy(stub_binary, stub_binary + stub_binary_size);
    uintptr_t offset = marker - reinterpret_cast<uintptr_t>(stub_binary);
    auto* stub_cfg = reinterpret_cast<TStub*>(stub_copy.data() + offset);

    stub_cfg->uid = static_cast<int>(ctx.target_uid);
    memset(stub_cfg->so_path, 0, sizeof(stub_cfg->so_path));
    strncpy(stub_cfg->so_path, ctx.remote_so_path.c_str(), sizeof(stub_cfg->so_path) - 1);
    stub_cfg->original_set_argv0 =
        reinterpret_cast<int (*)(JNIEnv*, jobject, jstring)>(ctx.set_argv0_address);
    stub_cfg->slot_addr = ctx.art_method_slot;
    stub_cfg->getuid = reinterpret_cast<uid_t (*)()>(ctx.remote_getuid);
    stub_cfg->dlopen = reinterpret_cast<void* (*)(const char*, int)>(ctx.remote_dlopen);
    stub_cfg->log_print =
        reinterpret_cast<int (*)(int, const char*, const char*, ...)>(ctx.remote_log_print);

    ssize_t written_code = pwrite(mem_fd,
                                  stub_copy.data(),
                                  stub_copy.size(),
                                  static_cast<off_t>(ctx.shellcode_base));
    if (written_code != static_cast<ssize_t>(stub_copy.size())) {
        LOGE("symbi: failed to write stub to shellcode_base=0x%lx", ctx.shellcode_base);
        return false;
    }

    uintptr_t new_ptr = ctx.shellcode_base;
    ssize_t written_ptr = pwrite(mem_fd,
                                 &new_ptr,
                                 sizeof(new_ptr),
                                 static_cast<off_t>(ctx.art_method_slot));
    if (written_ptr != static_cast<ssize_t>(sizeof(new_ptr))) {
        LOGE("symbi: failed to patch art_method_slot=0x%lx", ctx.art_method_slot);
        return false;
    }

    LOGI("symbi: wrote stub to 0x%lx and patched slot=0x%lx -> 0x%lx",
         ctx.shellcode_base, ctx.art_method_slot, new_ptr);
    return true;
}

bool restore_original_slot(const SymbiContext& ctx) {
    if (!stop_process(ctx.zygote_pid)) {
        return false;
    }

    bool ok = false;
    int mem_fd = open_remote_mem(ctx.zygote_pid);
    if (mem_fd >= 0) {
        ssize_t slot_written = pwrite(mem_fd,
                                      &ctx.original_ptr,
                                      sizeof(ctx.original_ptr),
                                      static_cast<off_t>(ctx.art_method_slot));
        ssize_t code_written = pwrite(mem_fd,
                                      ctx.original_shellcode_area.data(),
                                      ctx.original_shellcode_area.size(),
                                      static_cast<off_t>(ctx.shellcode_base));
        ok = slot_written == static_cast<ssize_t>(sizeof(ctx.original_ptr)) &&
             code_written == static_cast<ssize_t>(ctx.original_shellcode_area.size());
        close(mem_fd);
    }

    resume_process(ctx.zygote_pid);
    if (ok) {
        LOGI("symbi: restore complete slot=0x%lx shellcode=0x%lx",
             ctx.art_method_slot, ctx.shellcode_base);
    } else {
        LOGE("symbi: restore failed");
    }
    return ok;
}

bool start_target_app_symbi(const char* package_name) {
    std::string force_stop_cmd = std::string("am force-stop ") + package_name;
    std::string start_cmd = std::string("am start $(cmd package resolve-activity --brief '") +
                            package_name + "'| tail -n 1)";
    int force_stop_ret = system(force_stop_cmd.c_str());
    int start_ret = system(start_cmd.c_str());
    LOGI("symbi: start app force-stop ret=%d start ret=%d", force_stop_ret, start_ret);
    return start_ret == 0;
}

} // namespace

bool inject_spawn_symbi_by_pids(const std::vector<pid_t>& pids,
                                const char* package_name,
                                const char* so_path) {
    if (pids.empty() ||
        package_name == nullptr || package_name[0] == '\0' ||
        so_path == nullptr || so_path[0] == '\0') {
        LOGE("symbi: invalid spawn-symbi args");
        return false;
    }

    pid_t zygote_pid = -1;
    for (pid_t pid : pids) {
        if (pid > 0) {
            zygote_pid = pid;
            break;
        }
    }
    if (zygote_pid <= 0) {
        LOGE("symbi: no valid zygote pid");
        return false;
    }

    SymbiContext ctx{};
    if (!collect_symbi_context(zygote_pid, package_name, so_path, &ctx)) {
        return false;
    }

    if (!prepare_target_so_for_app(ctx.target_uid, package_name, so_path, &ctx.remote_so_path)) {
        LOGE("symbi: failed to prepare target so");
        return false;
    }

    if (!stop_process(zygote_pid)) {
        return false;
    }

    bool success = false;
    int mem_fd = open_remote_mem(zygote_pid);
    if (mem_fd >= 0) {
        success = write_stub_and_patch_slot(mem_fd, ctx);
        close(mem_fd);
    }
    resume_process(zygote_pid);

    if (!success) {
        return false;
    }

    if (!start_target_app_symbi(package_name)) {
        restore_original_slot(ctx);
        return false;
    }

    LOGI("symbi: patched zygote64, waiting for Ctrl+C to restore");
    g_keep_running = 1;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    while (g_keep_running) {
        sleep(1);
    }

    return restore_original_slot(ctx);
}

bool inject_spawn_symbi_by_package(pid_t zygote_pid,
                                   const char* package_name,
                                   const char* so_path) {
    return inject_spawn_symbi_by_pids(std::vector<pid_t>{zygote_pid}, package_name, so_path);
}

