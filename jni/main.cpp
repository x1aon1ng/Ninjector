#include "common/log.h"
#include "injector/injector.h"
#include "process/process.h"
#include "symbi/symbi_injector.h"

#include <cstdlib>
#include <cstring>
#include <future>
#include <thread>
#include <string>
#include <fstream>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

#define NINJECTOR_RESULT_FILE "/data/local/tmp/Ninjector/spawn_result.json"

static void wait_for_spawn_callback(std::promise<int>& promise_obj) {
    unlink(NINJECTOR_RESULT_FILE);
    int callback_pid = -1;
    std::string payload;

    for (int i = 0; i < 80; ++i) {
        struct stat st{};
        if (stat(NINJECTOR_RESULT_FILE, &st) == 0 && st.st_size > 0) {
            std::ifstream file(NINJECTOR_RESULT_FILE);
            if (file.good()) {
                std::getline(file, payload, '\0');
            }
            break;
        }
        usleep(100000);
    }

    if (payload.empty()) {
        promise_obj.set_value(-1);
        return;
    }

    LOGI("main: spawn callback payload=%s", payload.c_str());
    const char* pid_key = "\"pid\":";
    const char* pid_pos = strstr(payload.c_str(), pid_key);
    if (pid_pos != nullptr) {
        callback_pid = atoi(pid_pos + strlen(pid_key));
    }
    unlink(NINJECTOR_RESULT_FILE);
    promise_obj.set_value(callback_pid);
}

static bool start_target_app(const char* package_name) {
    if (package_name == nullptr || package_name[0] == '\0') {
        LOGE("start_target_app: invalid package name");
        return false;
    }

    std::string force_stop_cmd = std::string("am force-stop ") + package_name;
    std::string start_cmd = std::string("am start $(cmd package resolve-activity --brief '") +
                            package_name + "'| tail -n 1)";

    LOGI("start_target_app: force-stop package=%s", package_name);
    int force_stop_ret = system(force_stop_cmd.c_str());
    LOGI("start_target_app: force-stop ret=%d", force_stop_ret);

    LOGI("start_target_app: start package=%s", package_name);
    int start_ret = system(start_cmd.c_str());
    LOGI("start_target_app: start ret=%d", start_ret);

    return start_ret == 0;
}

static std::vector<pid_t> collect_spawn_source_pids() {
    const char* names[] = {
#if defined(__aarch64__)
        "zygote64"
#else
        "zygote"
#endif
    };
    std::vector<pid_t> pids;
    for (const char* name : names) {
        pid_t pid = get_pid(name);
        if (pid > 0) {
            pids.push_back(pid);
            LOGI("main: spawn-symbi candidate source name=%s pid=%d", name, pid);
        }
    }
    return pids;
}

static void show_help(const char* name) {
    printf("Usage:\n");
    printf("  %s -P <pid> <so_path>\n", name);
    printf("  %s -f -p <package> <so_path>\n", name);
    printf("  %s --spawn-symbi -p <package> <so_path>\n", name);
    printf("  %s -h\n", name);

    LOGI("Usage:");
    LOGI("  %s -P <pid> <so_path>", name);
    LOGI("  %s -f -p <package> <so_path>", name);
    LOGI("  %s --spawn-symbi -p <package> <so_path>", name);
    LOGI("  %s -h", name);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        show_help(argv[0]);
        return 0;
    }

    pid_t pid = -1;
    const char* so_path = nullptr;
    const char* package_name = nullptr;
    bool spawn_mode = false;
    bool spawn_symbi_mode = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-P") == 0 && i + 2 < argc) {
            pid = static_cast<pid_t>(atoi(argv[++i]));
            so_path = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 2 < argc) {
            package_name = argv[++i];
            so_path = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0) {
            spawn_mode = true;
        } else if (strcmp(argv[i], "--spawn-symbi") == 0) {
            spawn_symbi_mode = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            show_help(argv[0]);
            return 0;
        }
    }

    if (spawn_mode && spawn_symbi_mode) {
        LOGE("main: choose only one spawn mode");
        show_help(argv[0]);
        return -1;
    }

    if (spawn_mode) {
        if (package_name == nullptr || so_path == nullptr || so_path[0] == '\0') {
            LOGE("main: invalid spawn arguments");
            show_help(argv[0]);
            return -1;
        }

        pid = get_pid("zygote64");
        if (pid <= 0) {
            LOGE("main: zygote64 not found");
            return -1;
        }

        char cwd[PATH_MAX] = {0};
        if (getcwd(cwd, sizeof(cwd)) == nullptr) {
            LOGE("main: getcwd failed");
            return -1;
        }

        std::string ncore_path = std::string(cwd) + "/libncore.so";
        LOGI("main: spawn mode zygote=%d package=%s so=%s ncore=%s",
             pid, package_name, so_path, ncore_path.c_str());

        if (!prepare_spawn_in_zygote(pid, ncore_path.c_str(), package_name, so_path)) {
            LOGE("main: spawn prepare failed");
            return -1;
        }

        LOGI("main: spawn prepare success");

        std::promise<int> promise_obj;
        std::future<int> future = promise_obj.get_future();
        std::thread callback_thread(wait_for_spawn_callback, std::ref(promise_obj));

        if (!start_target_app(package_name)) {
            callback_thread.join();
            LOGE("main: auto start target app failed");
            return -1;
        }

        callback_thread.join();
        int callback_pid = future.get();
        if (callback_pid > 0) {
            LOGI("main: spawn inject success child_pid=%d", callback_pid);
            if (!clear_spawn_in_zygote(pid, ncore_path.c_str())) {
                LOGE("main: spawn clear failed");
            } else {
                LOGI("main: spawn clear success");
            }
        } else {
            LOGE("main: spawn callback timeout or failed");
            return -1;
        }

        LOGI("main: auto start target app success");
        return 0;
    }

    if (spawn_symbi_mode) {
        if (package_name == nullptr || so_path == nullptr || so_path[0] == '\0') {
            LOGE("main: invalid spawn-symbi arguments");
            show_help(argv[0]);
            return -1;
        }

        std::vector<pid_t> spawn_source_pids = collect_spawn_source_pids();
        if (spawn_source_pids.empty()) {
            LOGE("main: no spawn source processes found");
            return -1;
        }

        LOGI("main: spawn-symbi mode sources=%zu package=%s so=%s",
              spawn_source_pids.size(), package_name, so_path);
        return inject_spawn_symbi_by_pids(spawn_source_pids, package_name, so_path) ? 0 : -1;
    }

    if (pid <= 0 || so_path == nullptr || so_path[0] == '\0') {
        LOGE("main: invalid arguments");
        show_help(argv[0]);
        return -1;
    }

    LOGI("main: pid=%d so=%s", pid, so_path);
    if (!inject_so_by_pid(pid, so_path)) {
        LOGE("main: inject failed");
        return -1;
    }

    LOGI("main: inject success");
    return 0;
}


