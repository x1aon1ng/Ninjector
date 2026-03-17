# Ninjector 的 spawn 设计演进：两种方案为什么都保留下来了

## 前言

`Ninjector` 的 spawn 最后没有收敛成单一路线，而是保留了两种实现：

- 一种是基于 zygote 中间层的 `ncore` 方案
- 一种是基于 ART 槽位改写的 `--spawn-symbi` 方案

如果只看结果，这种结构会显得有点“不够干净”。但如果把整个调试过程摊开，它其实非常合理。因为这两条路解决的问题并不完全相同：

- `ncore` 更偏工程实现，目标是先做出一个稳定可用的 spawn 原型
- `symbi` 更偏研究实现，目标是尽量贴近 `TInjector_Symbi` 的原始思路

这篇文章不想简单下结论说“哪一种绝对更好”，而是想讲清楚四件事：

1. 为什么 spawn 比 attach 难很多
2. `ncore` 方案为什么成立
3. `--spawn-symbi` 为什么反复调整，最后又回退到 `TInjector_Symbi` 的模型
4. 为什么我最后没有把这两条路强行合并

## 一、spawn 真正难在哪里

attach 模式只需要回答一个问题：怎样让目标进程现在执行一次 `dlopen(target_so)`。

spawn 不一样。spawn 真正难的是“时机”。

因为目标 app 还没有起来，你必须提前在某个地方埋点。这个埋点还必须满足几个条件：

- 能在目标子进程出现时被命中
- 能区分“这是目标 app”还是“这只是另一个普通子进程”
- 在命中后可以执行真正的加载逻辑
- 最后还能被清理掉，不影响后续进程

这就意味着 spawn 的难点并不只是“再做一次远程调用”，而是：

- 埋点放在哪里
- 谁来判定目标进程
- 谁来执行 payload
- 注入完以后谁来恢复现场

如果这些边界没有想清楚，spawn 很快就会变成一个调试地狱。

## 二、第一条路：`ncore` 方案

### 1. 这条路的目标是什么

`ncore` 方案一开始的目标很明确：先不要碰太多 ART 运行时细节，而是先做一个“足够能工作”的 zygote 中间层。

它的整体思路是：

1. 把 `libncore.so` 注入 `zygote64`
2. 在 zygote 中远程调用 `ainject(package, so)`
3. 由 `ncore` 安装 `fork/vfork` hook
4. 子进程起来后，再安装 app 初始化阶段的 hook
5. 命中目标包名时，直接 `dlopen(target_so)`

从工程角度看，这条链路的优点是分层清晰。

注入器本体只负责“把 helper 送进 zygote 并给它参数”；真正的 spawn 逻辑，则由 zygote 内的 `ncore` 去负责。

### 2. 注入器侧是怎么把 `ncore` 送进去的

关键代码在 [injector.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/injector/injector.cpp#L117)。

这部分实际做了两层事：

第一层，是沿用 attach 模式，把 `libncore.so` 注入 zygote。  
第二层，是在 zygote 里远程调用 `dlsym + ainject(package, so)`，把目标包名和目标 so 路径交给 `ncore`。

核心代码大致如下：

```cpp
handle = inject_so_handle_by_pid(zygote_pid, ncore_path);
remote_ainject = call_remote_function<void*, void*, const char*>(
    zygote_pid,
    reinterpret_cast<void*>(dlsym),
    handle,
    reinterpret_cast<const char*>(remote_sym_name)
);
call_remote_call<void>(zygote_pid, reinterpret_cast<long>(remote_ainject), 2, params);
```

这里最值得注意的点是：`ncore` 的角色不是“另一个注入器”，而是“注入器投递进 zygote 的常驻控制面”。

### 3. `ncore` 里真正做了什么

`ncore` 的入口函数是：

```cpp
extern "C" void ainject(const char* package_name, const char* so_path)
```

对应位置在 [ncore.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/ncore/ncore.cpp#L156)。

它主要做三件事：

1. 保存目标包名和目标 so 路径
2. 安装 `fork/vfork` hook
3. 在子进程阶段安装更接近 app 初始化时机的 hook

关键 hook 点有两个：

- `android_os_Process_setArgV0`，对应 [ncore.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/ncore/ncore.cpp#L106)
- `selinux_android_setcontext`，对应 [ncore.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/ncore/ncore.cpp#L99)

真正决定“现在是不是目标 app”的逻辑，不是在 `ainject()` 里，而是在这些后续 hook 点里完成。

### 4. 为什么这条路很适合做工程原型

`ncore` 方案最大的优点，不是它“最先进”，而是它调试起来相对讲理。

第一，职责边界清楚。  
主程序只负责注入和下发参数，zygote 内的 `ncore` 负责等待、判断和真正的 payload 加载。

第二，日志容易打全。  
比如 [ncore.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/ncore/ncore.cpp#L71) 的 `load_payload_if_needed()` 会明确打印：是否命中目标、是否已经加载过、`dlopen` 是否成功。

第三，失败容易分层。  
spawn 失败时，可以较快判断到底是：

- `ncore` 根本没进 zygote
- `fork/vfork` hook 没装上
- 子进程 hook 没装上
- 包名没匹配上
- 还是 `dlopen(target_so)` 本身失败

这类“能拆开看”的能力，对第一版 spawn 原型来说，比“理论上更优雅”更重要。

### 5. 这条路的局限也很明显

`ncore` 虽然实用，但它并不等同于 `TInjector_Symbi` 的思路。

它本质上还是：

- 先往 zygote 里注入一个 helper so
- 让 helper 常驻并安装 hook
- 再通过 hook 拦截 app 启动过程

所以它更像一个工程上容易搭起来、也容易调试的方案，而不是一个“最贴近原项目机制”的方案。

## 三、第二条路：`--spawn-symbi` 方案

### 1. 为什么后来必须单独保留 `symbi`

你一旦把 `Ninjector` 的目标从“做出可用 spawn”升级为“尽量复现 `TInjector_Symbi` 的机制”，`ncore` 就不够了。

因为 `TInjector_Symbi` 的关键不在“zygote 里有个 helper 常驻”，而在：

- 找到 `android_os_Process_setArgV0`
- 找到它在 ART 里的槽位
- 直接把槽位改写到自定义 stub
- 在 stub 内部根据 uid 判断是否为目标 app
- 命中后由 stub 自己调用 `dlopen(target_so)`
- 最后再恢复原始槽位

这条路的研究价值，和 `ncore` 明显不同。它更接近“理解 ART 调用链是怎么被劫持的”，而不是“先把一个可工作的注入器做出来”。

### 2. `symbi` 的上下文收集阶段在做什么

当前 `--spawn-symbi` 的主入口在 [symbi_injector.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/symbi/symbi_injector.cpp#L500)。

真正复杂的部分，是前面的 `collect_symbi_context()`，对应 [symbi_injector.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/symbi/symbi_injector.cpp#L299)。

这一步主要收集五类信息：

1. 目标包名对应的 uid
2. `libandroid_runtime.so` 的路径与基址
3. `android_os_Process_setArgV0` 的实际地址
4. `libstagefright.so` 末页作为 stub 落点
5. ART 堆区中指向 `setArgV0` 的槽位地址

其中最关键的一步，是扫描可写 ART 区域，找到那个“值等于 `setArgV0` 地址”的槽位，对应 [symbi_injector.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/symbi/symbi_injector.cpp#L359)。

这一步找到的不是普通函数地址，而是“ART 运行时以后会走到的那个入口指针”。整个 `symbi` 方案的核心，就是把这个槽位改掉。

### 3. stub 是怎么工作的

真正执行注入动作的，不是主程序，而是 stub。

stub 的逻辑在 [stub.c](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/symbi/stub_src/stub.c#L11)。它做的事情非常克制：

1. 先调用原始 `setArgV0`
2. 再用 `getuid()` 判断当前进程 uid
3. 如果 uid 命中目标 app，就执行 `dlopen(so_path)`
4. 打日志后返回

也就是说，`symbi` 不是“主程序远程调用一次 `dlopen`”，而是“主程序改写 ART 槽位，让 zygote 派生出的目标 app 自己走到 stub 里，再由 stub 完成 `dlopen`”。

这一点和 `ncore` 的 helper 常驻 + hook 思路完全不同。

### 4. 主程序是怎么把槽位改到 stub 上的

改写动作在 `write_stub_and_patch_slot()` 里完成，对应 [symbi_injector.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/symbi/symbi_injector.cpp#L409)。

这里做了两件关键的事：

- 先把 stub 二进制写到选定的 shellcode 区域
- 再把 `ArtMethod` 槽位从原始 `setArgV0` 地址改成 stub 地址

相关代码大致是：

```cpp
pwrite(mem_fd, stub_copy.data(), stub_copy.size(), ctx.shellcode_base);
pwrite(mem_fd, &new_ptr, sizeof(new_ptr), ctx.art_method_slot);
```

同时，主程序还会把以下信息预先填进 stub 配置：

- 目标 uid
- 目标 so 路径
- 原始 `setArgV0` 地址
- 远程 `getuid` / `dlopen` / `__android_log_print` 地址

这意味着 stub 本身就是一个“可在目标进程上下文里独立工作的最小执行体”。

### 5. 为什么 `symbi` 需要手动 restore

`symbi` 和 `ncore` 的一个根本区别，是它直接改写了 zygote 内存里的 ART 槽位。

所以注入完成后，不能像 attach 一样单纯 `detach` 就算结束，还必须把原始槽位和原始页面内容写回去。当前实现的 restore 逻辑在 [symbi_injector.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/symbi/symbi_injector.cpp#L457)。

主程序在成功 patch 后会保持驻留，等待 `Ctrl+C`，然后执行 restore，对应 [symbi_injector.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/symbi/symbi_injector.cpp#L553)。

这是当前版本非常重要的行为边界：

- patch 阶段负责埋点
- app 启动阶段由 stub 自己判断和加载
- 退出阶段再恢复 zygote 现场

如果不 restore，就会出现你前面测试时遇到的现象：重开 app 后 hook 效果仍然存在。原因不是“payload 记住了你”，而是 zygote 里的劫持点还留着。

## 四、`symbi` 为什么会反复改写，最后又回到原项目思路

在这次项目演进里，`symbi` 其实经历过几次尝试：

- 一开始尝试做更像 Frida 的握手式方案
- 后来又尝试让主程序更主动地接管 child 注入
- 再后来发现方案越来越复杂，行为边界越来越不清楚
- 最后又回退到 `TInjector_Symbi` 的原始模型

这次回退不是“技术退步”，反而是一次边界收缩。

因为你这次做的是学习项目，不是追求把所有想法揉进一个版本。相比继续叠加新机制，回到原项目的核心思路反而更有价值：

- 研究目标更纯粹
- 与参考项目更容易逐项对照
- 博客也更容易解释清楚

所以现在的 `--spawn-symbi` 最终被收敛成了这样一条路线：

1. attach zygote
2. 找 `setArgV0` 的 ART 槽位
3. 把槽位改到 stub
4. 复制目标 so 到 app 自己可访问的位置
5. 启动目标 app
6. 由 stub 在目标 app 里自己 `dlopen`
7. 主程序驻留，直到手动 restore

这已经很接近 `TInjector_Symbi` 的原始模型了。

## 五、为什么最后没有把两条路合成一种

很多时候，看到一个项目里有两种 spawn 实现，会下意识觉得“应该收敛成一种”。但这次我最后没有这么做，因为两条路的目标不一样。

`ncore` 更像工程化原型：

- 好调试
- 好加日志
- 好分层定位问题
- 更适合先把 spawn 路线跑通

`--spawn-symbi` 更像研究型实现：

- 更贴近 `TInjector_Symbi`
- 更强调 ART 槽位改写本身
- 更能体现 zygote 启动链上的真实埋点逻辑
- 也更适合写成原理分析文章

如果强行把两条路揉成一条，很可能最后既失去 `ncore` 的工程可调试性，也失去 `symbi` 的原始机制纯度。

对一个学习项目来说，这不是收敛，而是信息损失。

## 六、从代码看，两种 spawn 其实共享了一块地基

虽然这两条路线在上层逻辑上差异很大，但它们并不是完全割裂的。

它们共享的底层地基主要有两块：

第一块，是 attach 模式提供的远程注入能力。  
无论是把 `libncore.so` 送进 zygote，还是前期为了研究 `symbi` 做各种远程准备，背后都建立在 attach 模式先站稳的前提上。

第二块，是 `main.cpp` 里保持很薄的命令行分发层。  
可以直接看 [main.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/main.cpp#L133) 和 [main.cpp](/E:/Learn/my_program/all_my_hook/kanxue/Ninjector/jni/main.cpp#L186)。

`-f` 走 `ncore` 路线，`--spawn-symbi` 走 `symbi` 路线。命令行入口并不试图掩盖两者差异，而是很明确地把它们作为两种不同模式暴露出来。

这也是我最后比较认可的一点：

> 共用能共用的底层，保留必须保留的分叉。

## 七、这两条路分别适合在什么场景下继续扩展

如果后面要继续做工程增强，我会优先沿着 `ncore` 方案往下走，比如：

- 让命中条件更稳定
- 把状态回传做得更完整
- 进一步减少对人工观察日志的依赖
- 把 zygote 中驻留逻辑整理成更清楚的生命周期

如果后面要继续做原理研究，我会优先沿着 `symbi` 方案往下走，比如：

- 更系统地分析 ART 槽位搜索的兼容性
- 研究不同 Android 版本上的偏移与映射差异
- 把 restore 过程做得更鲁棒
- 把这条路与 `TInjector_Symbi` 做更细粒度的逐项对照

也就是说，两条路后续要服务的目标，本来就不一样。

## 结语

从 attach 走到 spawn，最大的变化不是代码量变多了，而是问题的性质变了。

attach 主要解决“如何在一个已经存在的进程里执行远程 `dlopen`”。  
spawn 则必须额外解决“什么时候拦、在哪里拦、谁来判断、谁来恢复”。

`Ninjector` 最后同时保留 `ncore` 和 `--spawn-symbi`，不是因为设计没有收敛，而是因为这两条路分别回答了两类不同问题：

- `ncore` 回答的是“怎样先做出一个可工作的 spawn 工程原型”
- `symbi` 回答的是“怎样尽量忠实地复现 `TInjector_Symbi` 的机制”

对一个学习项目来说，这样的结果反而更有价值。因为你不仅拿到了一个能跑的实现，也保留了两种不同思路各自的边界、优点和代价。后面无论继续做代码，还是写博客做复盘，这两条线都值得单独展开。
