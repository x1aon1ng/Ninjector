# Ninjector Spawn 最终记录

## 1. 目标

`Ninjector` 的 spawn 最终保留了两条路线：

- 默认 `-f`：基于 `ncore` 的 zygote 中间层方案
- `--spawn-symbi`：基于 `TInjector_Symbi` 思路的 ART 槽位改写方案

保留两条路线的原因不是“没收敛”，而是它们服务的目标不同：

- `ncore` 更偏工程原型，强调稳定、清晰、好调试
- `symbi` 更偏机制学习，强调尽量忠实复现参考项目

## 2. ncore 路线总结

### 2.1 设计目标

在不破坏最小 attach 注入器结构的前提下，先做出一条可工作的 spawn 链路。

基本思路是：

1. 把 `libncore.so` 注入 `zygote64`
2. 远程调用 `ainject(package, so)`
3. 由 `ncore` 在 zygote 内安装 `fork/vfork` hook
4. 子进程启动后继续安装初始化阶段 hook
5. 命中目标包名后执行 `dlopen(target_so)`

### 2.2 关键演进

- 引入 `jni/ncore/ncore.cpp`，把 spawn 逻辑从 CLI 主程序中拆出去
- 新增 `-f -p <package> <so>` 命令入口
- 通过远程 `dlsym("ainject")` 把包名和 so 路径交给 zygote 内的 `ncore`
- 在 `ncore` 中接入 Dobby，hook `fork/vfork`
- 在子进程中继续 hook：
  - `android_os_Process_setArgV0`
  - `selinux_android_setcontext`
- 增强日志与成功回传
- 将成功回传最终收敛为结果文件 `/data/local/tmp/Ninjector/spawn_result.json`
- 在主程序中加入自动 `force-stop + am start`

### 2.3 最终结论

`ncore` 路线已经证明下面这些点是成立的：

- zygote 侧中间层注入可用
- `ainject(package, so)` 参数传递可用
- 子进程 hook 链可用
- 包名匹配可用
- payload so 加载可用
- 自动启动目标 app 可用
- 成功回传可用

因此 `ncore` 保留为当前项目中默认、最稳的 spawn 方案。

## 3. symbi 路线演进总结

### 3.1 初始目标

用户希望 `Ninjector` 中再增加一条单独的 spawn 模式，思路尽量贴近 `TInjector_Symbi`，并且作为学习项目保留完整的演进脉络。

最早的 `--spawn-symbi` 并不是最终形态，而是先做了一条 helper-so 路线：

- 注入 `libnsymbi.so` 到 `zygote64`
- 在 zygote 中安装 `fork/vfork` 与 `setArgV0` hook
- 命中目标后 `dlopen(target_so)`

这条路一开始能工作，但本质上仍然是“zygote 常驻 helper + hook”，并不等于真正的 `TInjector_Symbi`。

### 3.2 中间阶段暴露出的关键问题

在旧 `nsymbi` 阶段，主要暴露了几类问题：

- 日志不完整，成功与失败难以直接区分
- TCP / UNIX socket 回传不稳定，主程序容易误判失败
- one-shot 清理只在子进程生效，不能真正恢复 zygote 父进程状态
- 即使一次注入成功，后续手动重开 app 仍可能继续触发

为了解决这些问题，中间做过一系列工程修正：

- 补齐 `LOGI/LOGE`
- 将回传从 TCP 改到 UNIX socket，再改到结果文件
- 尝试 one-shot 清理和父进程主动清理

这些工作有价值，但最终也说明了一件事：旧 `nsymbi` 方案在结构上已经偏离了最初想学习的东西。

### 3.3 架构切换

后续明确决定：

- 废弃旧 `nsymbi` helper-so 路线
- `--spawn-symbi` 改成真正的 Symbi 路线

新的实现开始围绕下面这条主链展开：

1. attach `zygote64`
2. 定位 `android_os_Process_setArgV0`
3. 扫描 ART 相关可写区域，找到指向 `setArgV0` 的槽位
4. 选择一页可落 stub 的可执行区域
5. 将 stub 写入目标进程
6. 把 ART 槽位改写到 stub
7. 启动目标 app
8. 由 stub 在目标进程内自己判断并 `dlopen(target_so)`
9. 最后恢复原始槽位与原始代码页

## 4. symbi 路线最终收敛

### 4.1 为什么没有继续走 Frida 风格握手

中间一度尝试把 `symbi` 做成更像 Frida 的握手式 payload：

- stub 命中后与 host 侧握手
- host 再接管 child 注入

但这条路虽然研究味更强，实际却引入了更多复杂度：

- 启动链更长
- 同步点更多
- 调试边界更模糊
- 还出现了启动页卡死

重新对照 `TInjector_Symbi` 后确认，参考项目并没有走这条路，而是更直接的：

- patch zygote 中 ART 槽位
- stub 内部直接 `dlopen`
- 主程序驻留，手动 restore

所以最后决定完全回退到 `TInjector_Symbi` 的原始思路。

### 4.2 最终版的关键调试结论

在最终版 `symbi` 路线上，真正决定稳定性的几个问题分别是：

#### 1. zygote 槽位恢复是否真的生效

出现过“重开 App 后 Hook 仍然生效”的问题。最后确认不是旧进程残留，而是：

- `zygote64` 里的 ART 槽位没有被可靠恢复

最终通过下面几项修正解决：

- 恢复前后打印槽位值做校验
- 恢复动作加入重试
- 处理“旧的 patched slot 残留”情况

#### 2. shellcode 原始页如何可靠恢复

仅从当前进程内存备份 shellcode 页并不稳。最后改为：

- 直接从 `libstagefright.so` 文件中按映射偏移读取原始页

这样恢复时不会再依赖运行中已经被改写过的内存内容。

#### 3. stub 数据为什么一度找不到

曾出现 marker 找不到的问题，根因是：

- `helper.lds` 没有把 `.data*` 一起并入 `.payload`

修正后，`stubApi` 与 marker 才真正进入嵌入二进制。

#### 4. 执行上下文差异会影响 zygote 附加

同一台设备上观察到：

- 交互式 `adb shell -> su` 可以 `ptrace attach zygote64`
- `adb shell su -c` 方式会失败

这说明：

- 运行环境本身会影响 `zygote64` 暂停与附加
- 某些问题并不是代码错误，而是执行上下文差异

### 4.3 最终行为

当前 `--spawn-symbi` 的行为已经与 `TInjector_Symbi` 基本同构：

- 只选择 `zygote64` / `zygote`
- patch `setArgV0` 的 ART 槽位
- stub 内部直接按 uid 命中并 `dlopen(target_so)`
- 主程序驻留，直到手动 `Ctrl+C`
- `Ctrl+C` 后恢复原始槽位与 shellcode 区

在最终验证中，已经确认：

- app 正常启动
- hook 生效
- restore 完成后，再次手动重开 app 不会继续自动注入

## 5. 当前项目状态

现在 `Ninjector` 的 spawn 可以这样理解：

### 默认路线：`-f`

- 工程优先
- 更稳
- 可观测性更强
- 更适合作为日常调试和扩展基础

### 学习路线：`--spawn-symbi`

- 机制优先
- 更贴近 `TInjector_Symbi`
- 重点在 ART 槽位改写与 stub 执行
- 更适合写原理分析和实现复盘

## 6. 工程结论

这次 spawn 演进最后留下了几个明确结论：

1. 先做 attach 是对的  
   没有稳定的 attach 地基，就很难判断 spawn 问题到底出在远程调用、zygote 时机还是 payload 本身。

2. `ncore` 值得保留  
   它让 spawn 有了稳定、可调试、边界清楚的工程版本。

3. `symbi` 必须回到参考项目思路  
   如果目标是学习 `TInjector_Symbi`，就不应该把实现继续改造成另一套完全不同的机制。

4. 可观测性和恢复逻辑与注入本身同样重要  
   很多看似“偶发不生效”或“重开后仍生效”的问题，最后都不是注入动作本身，而是回传、恢复或执行上下文的问题。

## 7. 保留建议

后续文档层面建议保留下面几份文件：

- 本文：spawn 最终记录
- `BLOG_SPAWN_TWO_SCHEMES_CN.md`：适合对外发布的博客正文
- `BLOG_FIGURE_OUTLINES_CN.md`：配图提纲

这样既保留工程演进依据，也不会再让目录里堆满过时的阶段性记录。
