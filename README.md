# Ninjector

`Ninjector` 是一个基于 Android NDK 的本地注入工具项目，当前仓库主要包含注入器本体、进程处理逻辑、`ptrace` 相关实现，以及一个用于配合注入的本地共享库 `libncore.so`。

项目目前面向 `arm64-v8a`，使用 `ndk-build` 构建，产物包括可执行文件 `Ninjector` 和共享库 `libncore.so`。

## 功能概览

当前代码中包含以下几类能力：

- 按目标进程 PID 进行注入
- 按包名触发目标应用启动并执行注入
- 基于 zygote 的 spawn 注入流程
- 基于 `symbi` 的注入相关实现
- 构建独立本地 so 模块 `libncore.so`

当前支持的主要用法包括：

```bash
Ninjector -P <pid> <so_path>
Ninjector -f -p <package> <so_path>
Ninjector --spawn-symbi -p <package> <so_path>
Ninjector -h
```

## 目录结构

```text
Ninjector/
├─ jni/
│  ├─ common/              # 日志等通用头文件
│  ├─ injector/            # 注入实现
│  ├─ ncore/               # 本地共享库源码
│  ├─ process/             # 进程查找与处理逻辑
│  ├─ ptrace/              # arm64 ptrace 相关实现
│  ├─ symbi/               # symbi 注入相关实现与 stub
│  ├─ Android.mk
│  ├─ Application.mk
│  └─ main.cpp             # 命令行入口
└─ .gitignore
```

## 构建环境

当前工程配置如下：

- ABI: `arm64-v8a`
- Android Platform: `android-21`
- STL: `c++_static`
- Build System: `ndk-build`

建议环境：

- Windows / Linux / macOS 任一可运行 Android NDK 的开发环境
- 已正确安装并配置 Android NDK
- 命令行可直接使用 `ndk-build`

## 编译方法

在项目根目录执行：

```bash
ndk-build -C jni
```

或者进入 `jni` 目录后执行：

```bash
ndk-build
```

默认会根据 `jni/Application.mk` 和 `jni/Android.mk` 进行构建。

## 输出产物

构建后主要产物包括：

- `libs/arm64-v8a/Ninjector`
- `libs/arm64-v8a/libncore.so`

其中：

- `Ninjector` 是命令行注入器可执行文件
- `libncore.so` 是项目内构建的本地共享库

## 使用方法

### 1. 按 PID 注入

```bash
Ninjector -P <pid> <so_path>
```

示例：

```bash
Ninjector -P 12345 /data/local/tmp/libtarget.so
```

### 2. 按包名启动目标应用并注入

```bash
Ninjector -f -p <package> <so_path>
```

示例：

```bash
Ninjector -f -p com.demo.target /data/local/tmp/libtarget.so
```

### 3. 使用 spawn-symbi 模式

```bash
Ninjector --spawn-symbi -p <package> <so_path>
```

示例：

```bash
Ninjector --spawn-symbi -p com.demo.target /data/local/tmp/libtarget.so
```

## 运行前提

使用前请确认：

- 目标设备为 `arm64` 架构
- 设备环境允许相关注入操作
- 目标进程、zygote、文件路径等条件满足运行要求
- 待注入的 so 已推送到设备可访问路径
- 具备足够权限执行 `ptrace` / 注入相关操作

## 注意事项

- 当前工程明显依赖 Android Native 注入场景，不适用于普通 Java 层应用开发
- `spawn` 相关逻辑依赖目标设备环境，实际行为可能随 Android 版本和安全策略变化
- `jni/Android.mk` 中引用了外部静态库 `libdobby.a`，请确保相关依赖路径存在且可用
- 仓库当前只提交源码与必要文件，构建产物目录已在 `.gitignore` 中忽略

## 免责声明

本项目仅用于 Android Native 技术研究、逆向分析学习与安全测试。请仅在合法授权的设备、进程和场景中使用，因不当使用造成的任何后果由使用者自行承担。
