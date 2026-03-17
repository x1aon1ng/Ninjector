# Ninjector 博客配图与流程图提纲

这份文档不是正文，而是给两篇博客配图时可以直接参考的提纲。  
如果你准备发到看雪、博客园或者 GitHub Pages，这几张图基本就够用了。

## 一、attach 文章建议配图

### 图 1：attach 注入总流程

标题建议：

`Ninjector attach 模式总流程`

流程建议：

1. 用户执行 `Ninjector -P <pid> <so>`
2. 注入器 `ptrace attach` 目标进程
3. 计算远程 `malloc/dlopen/dlerror/free` 地址
4. 远程调用 `malloc`
5. 用 `ptrace_write` 写入 so 路径
6. 远程调用 `dlopen`
7. 如果失败，再远程调用 `dlerror`
8. 远程调用 `free`
9. `ptrace detach`

图上建议标出两种数据流：

- 控制流：注入器如何驱动目标进程
- 数据流：so 路径和错误字符串如何在两个进程之间流动

### 图 2：远程地址换算示意图

标题建议：

`同一函数在本地进程和目标进程中的地址换算`

建议内容：

- 左边画注入器自己的模块映射
- 右边画目标进程的模块映射
- 中间标公式：

```text
remote_addr = local_func - local_module_base + remote_module_base
```

这张图主要用于解释为什么 `dlopen` 可以直接拿本地符号推导出远程地址。

### 图 3：attach 失败定位路径

标题建议：

`attach 注入失败时的定位路径`

建议分三层：

1. attach 失败  
   - `ptrace` 权限/进程状态问题
2. 远程调用失败  
   - 地址换算/寄存器/调用约定问题
3. `dlopen` 失败  
   - so 路径、权限、依赖、SELinux 问题

这张图很适合放在“第一次失败但定位到 SELinux”的那一节后面。

## 二、spawn 文章建议配图

### 图 4：两种 spawn 方案总览对比

标题建议：

`Ninjector 中两种 spawn 方案的结构对比`

建议左右对照：

左侧 `ncore`：

1. 注入 `libncore.so` 到 `zygote64`
2. 调用 `ainject(package, so)`
3. `ncore` 常驻 zygote
4. hook `fork/vfork`
5. 子进程阶段 hook `setArgV0`
6. 命中后 `dlopen(target_so)`

右侧 `symbi`：

1. attach `zygote64`
2. 找 `setArgV0` 的 `ArtMethod` 槽位
3. 把槽位改写到 stub
4. app 启动时进入 stub
5. stub 命中 uid 后 `dlopen(target_so)`
6. 手动 restore

这张图几乎可以作为整篇 spawn 文章的总图。

### 图 5：`ncore` 方案的数据与控制边界

标题建议：

`ncore 方案中的控制面与执行面`

建议分三层：

1. 注入器主程序
2. zygote 中的 `ncore`
3. 目标 app 子进程

重点表达：

- 主程序只负责准备
- `ncore` 负责等待和判定
- 子进程负责真正加载 payload

这张图适合解释“为什么 `ncore` 是一个工程上更稳的方案”。

### 图 6：`symbi` 方案的内存改写路径

标题建议：

`symbi 方案：从 ArtMethod 槽位到 stub 执行`

建议把图画成三段：

1. `libandroid_runtime.so` 中的原始 `setArgV0`
2. ART 堆区里的 `ArtMethod` 槽位
3. `libstagefright.so` 末页上的 stub

箭头关系：

- 原始状态：槽位指向 `setArgV0`
- patch 后：槽位指向 stub
- stub 内部再调用原始 `setArgV0`

这是解释 `symbi` 原理时最关键的一张图。

### 图 7：spawn 方案演进路线图

标题建议：

`Ninjector spawn 方案的演进过程`

建议画成时间线：

1. attach 跑通
2. 引入 `ncore`
3. `ncore` 跑通 spawn
4. 尝试 `nsymbi`
5. 尝试 Frida 风格握手
6. 行为复杂化、调试变难
7. 回退为 `TInjector_Symbi` 原始模型

这张图很适合放在第二篇文章中间，用来解释“为什么最终保留两种方案”。

## 三、截图建议

除了流程图，我建议每篇再补 2 到 3 张真实截图。

### attach 文章建议截图

1. `Ninjector -P <pid> <so>` 的命令行截图
2. 第一次失败时的 `dlerror` 日志截图
3. `setenforce 0` 后成功的对比截图

### spawn 文章建议截图

1. `Ninjector -f -p <package> <so>` 或 `--spawn-symbi` 的命令行截图
2. `ncore` 成功命中的 logcat 截图
3. `symbi` 方案 patch 成功后的 logcat 截图
4. `Ctrl+C` restore 之后的日志截图

## 四、作图建议

如果你想让图更像技术博客而不是课堂笔记，我建议统一一种风格：

- 背景白色
- 模块框用浅灰或浅蓝
- 目标进程/zygote/app 用不同颜色区分
- 箭头统一方向，从左到右
- 关键系统对象单独加标签：
  - `zygote64`
  - `ArtMethod slot`
  - `libstagefright tail page`
  - `target_so`

术语尽量统一，不要一张图写“目标进程”，另一张图又写“目标 app 子进程”但没有说明关系。

## 五、建议的插图位置

### `BLOG_ATTACH_PRINCIPLE_CN.md`

建议插图顺序：

1. 图 1：放在“核心链路”之前
2. 图 2：放在“远程地址换算”那一节
3. 图 3：放在“第一次真实测试”那一节后面

### `BLOG_SPAWN_TWO_SCHEMES_CN.md`

建议插图顺序：

1. 图 4：放在前言后，先给全局视图
2. 图 5：放在 `ncore` 小节中
3. 图 6：放在 `symbi` 小节中
4. 图 7：放在“为什么最后保留两条路线”之前

如果你后面要继续，我可以下一步直接帮你把这几张图的 Mermaid 版本也写出来。这样你可以直接转成 PNG 或贴进博客。
