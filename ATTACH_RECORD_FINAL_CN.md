# Ninjector Attach 最终记录

## 1. 目标

attach 阶段的目标很明确：

- 只做 ARM64
- 需要 root
- 只支持按 PID 注入
- 只验证 `ptrace + 远程 dlopen`

这一阶段刻意把边界收得很小，不追求功能完整，而是先把最短、最基础的注入链路彻底验证清楚。

## 2. 为什么先做 Attach

和 spawn 相比，attach 更容易实现，也更容易调试。

attach 路径只需要解决这些问题：

1. 找到目标进程
2. 使用 `ptrace` 附加目标进程
3. 解析远程函数地址
4. 把 so 路径写入目标进程内存
5. 远程调用 `dlopen`
6. 读取成功结果或 `dlerror`

而 spawn 还需要额外面对：

- zygote 进程选择
- `fork/vfork` hook
- 子进程启动时机
- 目标包名匹配
- 中间层模块设计
- 生命周期清理

因此 attach 阶段真正想回答的问题只有一句话：

> 能否稳定地向一个 ARM64 目标进程注入指定 so，并通过 `ptrace + dlopen` 让它成功加载？

## 3. 工程结构

为了避免第一版重新长成“所有逻辑都塞进 `main.cpp`”的结构，工程被拆成几个最小模块：

```text
Ninjector/
  jni/
    common/
      log.h
    process/
      process.h
      process.cpp
    ptrace/
      ptrace_arm64.h
      ptrace_arm64.cpp
    injector/
      injector.h
      injector.cpp
    main.cpp
    Android.mk
    Application.mk
```

各模块职责如下：

- `common/log.h`
  统一日志输出
- `process`
  负责 PID 查询、maps 解析、模块基址查询、远程地址换算
- `ptrace`
  负责 attach/detach、远程内存读写、远程函数调用
- `injector`
  负责 attach 模式下的 so 注入流程
- `main.cpp`
  负责命令行入口

## 4. 核心 Attach 链路

最小 attach 注入链如下：

1. 执行 `Ninjector -P <pid> <so_path>`
2. `ptrace attach`
3. 解析 `malloc/free/dlopen/dlerror` 的远程地址
4. 远程调用 `malloc(strlen(path)+1)`
5. 把 so 路径写入目标进程内存
6. 远程调用 `dlopen(path, RTLD_NOW | RTLD_GLOBAL)`
7. 如果失败，远程调用 `dlerror`
8. detach

从概念上说，注入器是在目标进程中让它执行类似下面的逻辑：

```c
void* p = malloc(strlen(path) + 1);
strcpy(p, path);
dlopen(p, RTLD_NOW | RTLD_GLOBAL);
```

## 5. 关键实现点

### 5.1 `process` 模块

实现了：

- 从 `/proc` 按进程名查找 PID
- 解析 `/proc/<pid>/maps`
- 获取模块基址
- 根据函数地址反查所属模块
- 计算远程函数地址：

```text
remote_addr = local_func - local_module_base + remote_module_base
```

### 5.2 `ptrace` 模块

实现了：

- `PTRACE_ATTACH` / `PTRACE_DETACH`
- 用 `PTRACE_PEEKDATA` / `PTRACE_POKEDATA` 做远程内存读写
- ARM64 调用约定下的远程函数调用
- 寄存器备份与恢复

```
 # 一、为什么远程写内存要按字长分块，而不是直接 memcpy

  ## 1. 先说结论

  因为你不是在给“自己进程的内存”写数据，而是在给“另一个进程的内存”写数据。
  普通的 memcpy 只能操作你自己当前进程里能直接访问的地址，不能直接跨进程写。

  所以注入器不能这么写：

  memcpy((void *)remote_addr, data, size);

  看起来像是在写目标进程地址，其实不是。
  这行代码只会试图在“你自己进程里”访问一个叫 remote_addr 的地址。这个地址通常：

  - 在你进程里根本没映射
  - 或者映射含义完全不同

  结果要么崩，要么写错地方。

  所以跨进程写内存，必须借助内核提供的机制，比如：

  - ptrace
  - process_vm_writev
  - /proc/<pid>/mem

  在 Ninjector 这种最小注入器里，用的是 ptrace。

  ———

  ## 2. ptrace 为什么不能像 memcpy 一样整块写

  因为 ptrace 的经典读写接口 PTRACE_PEEKDATA / PTRACE_POKEDATA 本身就是按“机器字”工作的。

  你可以粗略理解成：

  > 一次 ptrace 写，不是任意长度字节流写入，而是写一个机器字大小的数据。

  在 64 位 ARM 上，一个机器字通常是 8 字节，也就是 sizeof(unsigned long)。

  所以你没法天然一次写 13 个字节、27 个字节这种长度。
  必须拆成：

  - 前面若干个完整字长
  - 最后不足一个字长的尾巴

  这就是为什么 ptrace_write() 要分块。

  ———

  ## 3. 什么叫“按字长分块”

  比如你要写一个字符串：

  /data/local/tmp/libtest.so\0

  假设总长度是 27 字节。
  在 64 位系统上按 8 字节分，就会变成：

  - 第 1 块：8 字节
  - 第 2 块：8 字节
  - 第 3 块：8 字节
  - 剩余尾巴：3 字节

  前 24 字节可以直接按整块写。
  最后 3 字节不能直接“只写 3 字节”，因为 PTRACE_POKEDATA 通常还是按一个完整字写。

  所以尾巴要特殊处理。

  ———

  ## 4. 为什么尾巴要特殊处理

  这是最关键的一点。

  假设目标地址最后那 8 个字节原本内容是：

  AA BB CC DD EE FF 11 22

  你只想改前 3 个字节，变成：

  78 79 7A

  如果你粗暴地直接写一个完整字，但剩下 5 个字节没处理好，就可能把原来内存里不该动的数据覆盖掉。

  所以正确做法通常是：

  1. 先把目标地址这一整个字读出来
  2. 只替换前 remain 个字节
  3. 再把整个字写回去

  也就是“读-改-写”。

  这就是为什么 ptrace_write() 代码里尾部通常会先 PTRACE_PEEKDATA，再 memcpy 部分字节，最后 PTRACE_POKEDATA。

  ———

  ## 5. 为什么不能偷懒地一次次只写一个字节

  理论上你可以想：“那我每次就写 1 字节不行吗？”

  问题是：

  - ptrace 本身不是为高频单字节写设计的
  - 系统调用开销很高
  - 这样写会很慢
  - 有的平台/实现也不一定支持你这样精细地安全写

  所以工程上更常见的做法就是：

  > 尽量按机器字整块写，最后尾巴做一次读改写。

  这样是兼顾正确性和效率的折中方案。

  ———

  ## 6. 这和 Ninjector 里的代码有什么对应

  你看 ptrace_arm64.cpp:96 里的 ptrace_write()，它本质上就在做三件事：

  1. 算出完整字长块数
  2. 循环写前面完整块
  3. 如果有剩余尾巴，就先读目标字，再局部覆盖，再写回

  这不是一种“实现风格”，而是跨进程写内存时非常典型、非常基础的做法。
```



### 5.3 `injector` 模块

实现了：

- 远程字符串分配辅助函数
- attach 模式下注入 so
- 远程读取 `dlerror`

## 6. 编译阶段暴露出的问题

第一次完整执行 NDK 编译时，先后暴露出几个典型的小问题：

- 头文件路径错误
- 日志宏命名不一致
- 模板参数转换问题
- `goto` 跨过变量初始化

这些问题修完后，`ndk-build` 成功产出了最小可执行文件。

## 7. 第一次设备测试结论

第一次测试使用的是类似下面的命令：

```bash
/data/local/tmp/Ninjector/Ninjector -P <pid> /data/local/tmp/Ninjector/liball_in_one.so
```

第一次的关键日志是：

```text
Ninjector: main: pid=15804 so=/data/local/tmp/Ninjector/liball_in_one.so
Ninjector: attach_process: attached to pid=15804 status=0x137f
Ninjector: inject_so_by_pid: attached to pid=15804
Ninjector: inject_so_by_pid: remote dlopen failed
Ninjector: inject_so_by_pid: dlerror=dlopen failed: couldn't map "/data/local/tmp/Ninjector/liball_in_one.so" segment 2: Permission denied
Ninjector: detach_process: detached from pid=15804
Ninjector: main: inject failed
```

这段日志说明了两件事：

- attach 成功
- 远程调用链是通的

真正失败的是目标进程映射 so 文件时被拒绝。

也就是说，根因不在注入器主链本身，而在 Android 的执行映射安全策略。

## 8. 根因验证

为了验证环境假设，临时关闭了 SELinux：

```bash
setenforce 0
```

之后再次执行同样的 attach 注入，注入成功。

这一步确认了真正的限制来自：

- SELinux
- 或目标 so 所在路径的可执行映射限制

而不是：

- `ptrace`
- 远程地址解析
- 远程 `dlopen` 调用过程

## 9. Attach 阶段里程碑

到 attach 阶段结束时，已经确认了这些事实：

1. 一个最小 ARM64 attach 注入器可以工作
2. `ptrace + 远程 dlopen` 这条链是成立的
3. 失败时可以通过 `dlerror` 快速定位问题
4. 下一阶段的主要限制已经不再是注入器架构，而是 Android 运行时策略

## 10. 当前状态

已验证：

- 能通过编译
- attach 注入可用
- attach 注入后目标 hook 可生效

当前已知限制：

- 还没有包名模式
- 没有 hide
- 没有 retry
- 没有内存加载 so
- 仍然依赖 so 路径和 SELinux 状态

## 11. 工程结论

attach 阶段最大的价值，不只是“成功注入了一次”，而是为后续所有 spawn 尝试提供了一个可靠基线：

- 如果后面 spawn 失败，可以先回到 attach 验证底层远程调用链是否仍然成立
- 如果 `dlopen` 失败，可以直接从错误字符串判断是路径、权限还是策略问题
- 如果底层链路不稳，就不应该过早把问题归因到 zygote、hook 或时机控制上

因此对整个 `Ninjector` 项目来说，attach 不是过渡代码，而是后续所有演进的地基。

## 12. 保留建议

文档层面建议保留下面几份：

- 本文：attach 最终记录
- `BLOG_ATTACH_PRINCIPLE_CN.md`：适合对外发布的博客正文
- `BLOG_FIGURE_OUTLINES_CN.md`：配图提纲

这样 attach 部分既有对外文章，也有内部过程记录。
