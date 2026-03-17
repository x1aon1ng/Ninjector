# 从零实现一个最小 Android Native 注入器：Ninjector 的 attach 模式是怎么工作的

## 前言

如果一开始就去做 Android 的 spawn 注入，通常很快就会陷入一个尴尬局面：你知道它没成功，但很难判断究竟失败在什么位置。到底是 zygote 选错了、时机没踩对、包名没有命中、远程调用写错了，还是目标 so 自己就有问题？

我在做 `Ninjector` 时，刻意没有从 spawn 开始，而是先把问题收得非常小，只做一条最短的链路：`attach + ptrace + 远程 dlopen`。它不优雅，也不炫，但足够诚实。只要这条链路能稳定跑通，后面所有复杂方案才有落脚点。

这篇文章想讲清楚三件事：

1. 为什么我先做 attach，而不是直接做 spawn
2. `Ninjector` 的 attach 模式到底在做什么
3. 代码里哪些设计点，决定了这条链路能不能被解释、能不能被调试

## 一、为什么先做 attach

attach 模式的目标非常直接：让一个已经存在的目标进程执行一次 `dlopen(target_so)`。

这条链只需要解决几个问题：

- 如何找到目标进程
- 如何停住目标进程
- 如何在目标进程里调用函数
- 如何把 so 路径写进目标进程
- 如何把 `dlopen` 的结果和错误信息拿回来

而 spawn 模式远不止这些。它还要多处理：

- 该注入哪个 zygote
- 在哪个阶段拦截子进程
- 怎么判断这个子进程就是目标 app
- 注入完成后怎么清理埋点

所以我的第一阶段只回答一个问题：

> 能不能稳定地让目标进程执行一次远程 `dlopen`？

只要这个问题回答清楚，后面的复杂性才是“建立在已知事实上的复杂性”，而不是“所有问题一起混着来”。

## 二、项目为什么这样拆

为了不让第一版退化成一个几百行的 `main.cpp`，我把 attach 模式拆成了几个最小模块：

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
```

这个结构不是为了“看起来更像工程项目”，而是为了把几个完全不同的问题切开。

`process` 模块负责进程、模块、地址换算。  
`ptrace` 模块负责 attach、读写内存、远程调用。  
`injector` 模块负责把 so 注进去。  
`main.cpp` 只保留命令行入口。

这样拆的好处，在调试时非常明显。你能快速判断问题卡在：

- 目标 PID 根本没找到
- 模块基址算错了
- `ptrace` 没 attach 上
- 远程调用没执行成功
- 还是 `dlopen` 自己失败了

这类“可切分”对注入器项目非常重要。因为注入失败几乎是常态，真正有价值的不是“成功那一次”，而是失败时你能不能把原因分层看清楚。

## 三、attach 模式的核心链路

从概念上说，attach 注入并不复杂。注入器希望目标进程在自己的上下文里执行下面这几行：

```c
void *p = malloc(strlen(path) + 1);
strcpy(p, path);
dlopen(p, RTLD_NOW | RTLD_GLOBAL);
```

难点不在这三行代码本身，而在于：

- `malloc` 要在目标进程里执行
- `strcpy` 的数据要写进目标进程内存
- `dlopen` 也要在目标进程里执行
- 而且这一切都不能破坏目标进程原本的寄存器现场

所以 `Ninjector` 的 attach 主链可以收敛成下面几步：

1. `PTRACE_ATTACH` 停住目标进程
2. 在远程进程中调用 `malloc`
3. 用 `ptrace_write` 把 so 路径写进去
4. 在远程进程中调用 `dlopen`
5. 如果失败，再远程调用 `dlerror`
6. 释放远程缓冲区
7. `PTRACE_DETACH` 恢复目标进程

对应的主逻辑就在 [injector.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/injector/injector.cpp#L31)。

## 四、命令行入口只做参数分发

`main.cpp` 在这个项目里刻意保持得很薄。attach 模式对应的入口非常直接：

- 接收 `-P <pid> <so_path>`
- 打印日志
- 调用 `inject_so_by_pid()`

可以直接看 [main.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/main.cpp#L204)。

这种写法的意义不是“简洁”，而是边界清楚。`main.cpp` 不承担任何注入细节，它只决定“要不要做”和“把参数交给谁做”。这样后面新增 spawn 模式时，不会把 attach 的逻辑揉乱。

## 五、process 模块解决的是“远程地址怎么推出来”

做远程调用时，一个最核心的问题是：我在本进程拿到的 `dlopen` 地址，为什么可以在目标进程里继续用？

答案不是“直接拿地址照搬”，而是“先找函数所在模块，再做基址换算”。

这部分的关键代码在 [process.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/process/process.cpp#L142)。核心公式是：

```text
remote_addr = local_func - local_module_base + remote_module_base
```

为了得到这个结果，`process` 模块拆成了几步：

- `get_pid()` 从 `/proc/*/cmdline` 找目标进程，对应 [process.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/process/process.cpp#L9)
- `get_module_base()` 解析 `/proc/<pid>/maps` 找模块基址，对应 [process.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/process/process.cpp#L59)
- `get_module_name()` 先判断本地函数属于哪个模块，对应 [process.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/process/process.cpp#L101)
- `get_remote_addr()` 最后做基址换算，对应 [process.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/process/process.cpp#L142)

这一层抽出来之后，`ptrace` 模块就不需要关心“地址是怎么来的”，它只关心“拿到远程地址后怎么调”。

## 六、ptrace 模块解决的是“怎么把函数真的调起来”

attach 注入里最底层、也最容易出错的一层，是 `ptrace`。`Ninjector` 把它单独放进 [ptrace_arm64.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/ptrace/ptrace_arm64.cpp)。

### 1. attach 和 detach

`attach_process()` 与 `detach_process()` 分别封装了 `PTRACE_ATTACH` 和 `PTRACE_DETACH`，并在 attach 后显式 `waitpid()`，确保目标进程真的停住，对应：

- [ptrace_arm64.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/ptrace/ptrace_arm64.cpp#L32)
- [ptrace_arm64.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/ptrace/ptrace_arm64.cpp#L54)

这一步看上去简单，但它决定了后续读写内存、改寄存器是否有稳定前提。

### 2. 按字长读写目标进程内存

`ptrace_read()` 和 `ptrace_write()` 没有假设“所有长度都正好按机器字对齐”，而是按 `unsigned long` 分块处理，尾部不足一个字长时再单独补齐，对应：

- [ptrace_arm64.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/ptrace/ptrace_arm64.cpp#L69)
- [ptrace_arm64.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/ptrace/ptrace_arm64.cpp#L96)

这一点在写 so 路径字符串时尤其重要。因为路径长度几乎不可能刚好天然对齐，如果没有尾部读改写逻辑，最后几个字节很容易把远程内存写坏。

### 3. 统一的错误出口

`xptrace()` 把所有 `ptrace()` 调用统一包了一层，对应 [ptrace_arm64.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/ptrace/ptrace_arm64.cpp#L11)。

它的价值不是“少写点代码”，而是让所有低层失败都带上相同格式的日志。做注入器时，失败路径通常比成功路径更重要，这种统一出口会让调试成本明显下降。

## 七、injector 模块把 attach 链真正串起来了

真正把“远程 malloc + 写字符串 + 远程 dlopen”串成完整链路的，是 [injector.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/injector/injector.cpp)。

### 1. 先在目标进程中申请一块字符串内存

`remote_alloc_string()` 先远程调用 `malloc(strlen(str) + 1)`，再用 `ptrace_write()` 把路径写进去，对应 [injector.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/injector/injector.cpp#L9)。

这样写有两个好处：

- 远程内存的生命周期清楚
- 后续 `dlopen` 只需要拿一个远程地址，不必再关心字符串传输细节

### 2. 失败时主动回收错误信息

主注入函数 `inject_so_handle_by_pid()` 的核心入口在 [injector.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/injector/injector.cpp#L31)。它做的事情其实很克制：

1. attach 目标进程
2. 准备远程路径字符串
3. 远程调用 `dlopen`
4. 如果 `dlopen` 返回空，再远程调用 `dlerror`
5. 用 `ptrace_read()` 把错误字符串读回本地
6. 清理远程内存并 detach

这里最关键的一点，是“失败时不只是返回 false，而是把 `dlerror()` 拿回来”。这直接决定了调试体验。

比如你前面测试时出现过这类典型报错：

```text
dlopen failed: couldn't map "/data/local/tmp/Ninjector/liball_in_one.so" segment 2: Permission denied
```

这种信息一拿到，问题就不再是“注入器大概哪里坏了”，而是可以明确收敛到 so 路径权限、文件上下文或者 SELinux 上。对学习项目来说，这种可解释性比单纯成功一次更有价值。

### 3. 为什么 attach 成功后还要重新 detach

很多初学者写注入器时，会把目标进程一直挂在 `ptrace` 状态里，觉得这样后续更方便。但 `Ninjector` 的 attach 路线没有这么做，而是在动作完成后明确 `detach`，对应 [injector.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/injector/injector.cpp#L90)。

这背后的设计很朴素：attach 模式只想完成“一次性远程加载”，不是接管整个目标进程生命周期。动作做完就退出，行为边界最清楚，也最适合作为后续所有注入实验的基线。

## 八、第一次真正踩到的坑：不是代码错，而是 SELinux

在 Android 上做 `dlopen` 远程注入，最容易误判的地方之一，就是把权限问题看成代码问题。

我在测试 `Ninjector` attach 时，遇到过这样一类现象：

- `ptrace attach` 成功
- 远程调用链也走到了 `dlopen`
- 但 `dlopen` 明确报 `Permission denied`
- `setenforce 0` 之后又能立即成功

这说明注入链本身已经通了，失败不是因为 `ptrace`、寄存器或远程地址换算写错，而是目标 so 的映射权限被 SELinux 拦下来了。

这一点其实也说明，前面把 `dlerror()` 拉回来的设计是必要的。没有这一步，你只会得到一句笼统的“inject failed”；有了它，才能把问题收敛成系统策略问题，而不是继续盲猜代码。

## 九、attach 这条链为什么值得保留

即便后面已经做了 `spawn`，我还是认为 attach 是整个 `Ninjector` 项目里最重要的基线。

原因有三点：

第一，它足够小。  
你可以非常清楚地解释每一层在干什么，不会一上来就被 zygote、ART、hook 时机混在一起。

第二，它足够可验证。  
一条远程 `dlopen` 链如果通了，后面无论是往 zygote 里塞 helper，还是改写 ART 槽位，本质上都建立在同一个事实之上：你已经掌握了远程执行与远程内存写入。

第三，它足够适合作为调试基线。  
spawn 一旦出问题，你很容易不确定是“spawn 逻辑错了”，还是“最底层的远程调用链根本就不稳”。而 attach 的存在，恰好可以把这两个问题拆开。

## 十、回到代码：这个最小注入器到底做成了什么

如果把整条链压缩成一句话，`Ninjector` attach 模式做的事情其实是：

> 用 `ptrace` 临时接管目标进程，借它自己的 `malloc` 和 `dlopen`，把一个外部 so 加载进来，再把控制权还回去。

从实现上看，它没有引入多余框架，也没有为了“做成项目”而强行堆很多抽象。整个设计就是围绕三件事展开：

- 地址能不能算对
- 远程调用能不能站稳
- 出错时能不能把原因拿回来

这也是我后来继续做 spawn 的前提。因为只有 attach 这条最短链路站稳，后面的所有复杂设计才不是空中楼阁。

## 结语

如果你也想自己从零做一个 Android 注入器，我的建议仍然是：不要一开始就追求 spawn，也不要一上来就追求“和某个成熟项目一样”。

先做 attach，把 `ptrace`、远程地址换算、远程 `dlopen` 这三件事彻底跑通。等你能稳定解释一次成功、也能稳定解释一次失败，再去做 zygote、ART 槽位和更早期的启动时机控制，整个学习路径会顺很多。

对 `Ninjector` 来说，attach 不是过渡版本，而是整套注入实验里最重要的一块地基。
