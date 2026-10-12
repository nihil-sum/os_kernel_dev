# 基础进程模块（当前阶段）

这一版在现有 RAM inode 文件系统上实现进程生命周期、cwd、文件描述符、按路径加载的
`exec`、`fork` 继承、退出时关闭文件，以及 callable 内核进程和 C 系统调用分发接口。
代码只使用项目头文件、伙伴分配器、`kalloc`、链表和内核自己的内存函数，不依赖标准 C 库。
当前启动环境仍是 RISC-V M-mode，没有用户态 trap 入口或硬件用户页表。
MLFQ 已支持内核 callable 进程的协作式切换；用户镜像仍仅由内核检查生命周期，尚不执行。

## 数据结构与内存所有权

- `proc_table`：固定 32 个槽位、文件系统指针、持有引用的根 inode、当前进程指针、
  单调递增 PID；旧镜像注册表仅保留给兼容测试。
  PID 不随槽位复用；失败的创建可能留下 PID 间隙，超出有符号 64 位范围后拒绝创建。
- `proc`：PID、父进程、状态、终止标志、退出码、名称、独立内核栈、内核上下文、
  进程种类、完整用户通用寄存器现场、地址空间、持有引用的 cwd 和 16 个文件描述符；
  内核进程另保存 callable 入口、
  带类型标记的返回值和入口执行状态。
- `context`：保存 `ra/sp/s0..s11`。创建时初始化栈顶；MLFQ 内核执行后端在首次运行时
  将 `ra` 设置为入口 trampoline。
  `asm/swtch.s` 已完成，按此结构保存/恢复寄存器。用户进程返回入口仍待实现。
- `trapframe`：`x[0..31]` 和用户 PC；进程返回值、入口与用户栈参数都保存在这里。
  CSR 的保存、特权切换及用户态返回留给架构层。
- `proc_space`：最多 32 个软件页映射。每项保存虚拟地址、独占 order-0 物理页及 R/W/X
  权限。元数据内嵌于进程，不需要堆分配。它不是 Sv39 页表，也不会设置 `satp`。
- `proc_schedule`：MLFQ 所属调度器、内嵌就绪队列节点、优先级、剩余时间片与 CPU 配额、
  等待通道及上下文准备/挂起标记；生命周期变化自动通知附着在进程表上的调度器。

内核栈和每个映射页都来自同一个调用者提供的 `buddy_allocator`；申请时清零，
回收时释放所有页并清空进程元数据。
伙伴分配器新增 `buddy_alloc`/`buddy_free`，管理拆分、合并、元数据和空闲链表，
拒绝错误阶数、内部页和重复释放。进程模块仅申请 order-0 页。
物理地址必须能被内核直接访问；主机测试使用页对齐字节数组，QEMU 使用内核末尾之后的 RAM。

文件描述符指向 `kalloc` 分配的共享打开记录，每条记录包装已有 `struct file`，
持有一个 inode 引用及一个文件偏移。cwd 单独持有目录 inode 引用。
用户页和内核栈仍由伙伴分配器直接管理。

推荐初始化顺序是 buddy、`kalloc_init`、`make_filesystem`、
`proc_table_init_fs(table, allocator, filesystem)`，之后再按需附着 MLFQ。
新进程从文件系统根目录开始，描述符表为空。进程表本身持有根 inode 引用，
因此即使没有进程，`fs_destroy` 也返回 `FS_EBUSY`。
`proc_table_destroy` 要求所有进程已经 reap、没有 current 且调度器已分离；
它释放根引用，之后才能销毁文件系统。不能在存活表上重新调用初始化函数。

调用者必须串行访问进程表、文件系统及分配器。当前只支持单 hart、无抢占的测试环境；下一阶段需要
在进程表和伙伴系统外围加入锁或中断保护。初始化后的表、分配器和元数据不能移动、复制
或在仍有存活进程时重新初始化。旧的 `mm.c` 页分配器不能同时管理同一物理内存区域。

## Callable 内核进程

`PROC_USER` 使用从文件系统加载的 flat 镜像和用户寄存器现场；`PROC_KERNEL` 的执行入口是
`struct callable`，没有用户映射，也不把内核函数地址写入用户 PC。
两种进程共享 PID、状态、cwd、描述符、终止与回收规则。Callable 进程可以直接使用
`proc_fs.h` 的可信内核指针 API；用户系统调用入口仍只接受 `PROC_USER`。

原来 `types.h` 中的 callable API 已提取到 `src/include/callable.h`。`types.h` 保留
固定宽度整数别名，并通过兼容 include 继续导出 callable。整数别名由内核的 `ktypes.h` 使用基础 C 类型定义，避免宏展开，也使 callable 的 64 位结果类型与进程模块的 `u64_t/i64_t`
保持一致。原有构造函数、枚举、`callable_call` 和 `FN_V_V(void *)` 签名保留。

新增 `callable_valid` 可在创建进程前检查类型标记和函数是否有效，不执行函数。
`callable_invoke` 使用 `struct callable_result` 保存返回类型及对应 union 成员，
不要求进程层猜测返回值的宽度。原有 8/16/32/64 位有符号、无符号整数、指针、void
以及有无上下文参数的全部类型都支持；失败时结果保持不变。

下面是当前阶段的显式运行方式（各返回码均应检查）：

```c
static i32_t worker(void *arg) {
    unsigned int *count = arg;
    ++*count;
    return 7;
}

/* table 已初始化；即使没有镜像注册表，也可创建 callable 进程。 */
unsigned int count = 0;
struct callable entry = callable_from_fni32p(worker, &count);
struct proc *process;
if (proc_create_callable(&table, "worker", &entry, &process) == 0) {
    u64_t pid = process->pid;
    if (proc_activate(&table, process) == 0) {
        int result = proc_run_callable(&table);
        /* result == 0 表示入口执行接口成功；入口返回值在 process->result。
         * 正常情况下此时是 ZOMBIE，exit_status == 7。
         */
        (void)result;
        if (proc_deactivate(&table) == 0)
            (void)proc_reap(&table, pid);
    } else if (process->pstatus == ZOMBIE) {
        /* 激活前已被 kill 的进程也需要回收。 */
        (void)proc_reap(&table, pid);
    }
}
```

上面示例使用未附着调度器的进程表。附着 MLFQ 后，必须由 `mlfq_dispatch`/`mlfq_run`
管理当前进程，不能再手动调用 `proc_activate`/`proc_deactivate` 操作就绪队列中的进程。
完整调度用法见 [`../schd/mlfq.md`](../schd/mlfq.md)。

创建时复制 callable 描述符，借用 `arg` 指向的上下文，不复制或释放上下文。
上下文必须存活到入口返回；指针结果也只是借用，不自动释放。创建分配一个内核栈页，
但单独使用 `proc_run_callable` 时它仍是同步 C 调用，使用调用者的栈。
`mlfq_run` 后端先切到进程内核栈再调用此 API，且允许回调在自己的
栈中 `mlfq_yield`/`mlfq_sleep` 并从原调用点恢复。入口只能启动一次，执行结束后保留
结果供回收前检查；恢复挂起入口不会重新执行入口函数的开头。
调用者不能在存活期间直接修改进程的入口、类型、执行标志或其他生命周期字段。

有符号 32 位返回值作为默认退出码；void、指针和其他整数类型默认退出码为 0，
完整返回值仍在 `result` 中。回调内可显式调用 `proc_exit` 设置退出码，但随后必须
正常返回到 runner。`kill` 始终让退出码成为 -1；运行前检查到终止标志时不会调用入口。
未使用调度器时，执行中的终止请求在回调返回后生效。使用 MLFQ 时也会在下一次
yield/checkpoint/睡眠切换处终止并丢弃挂起调用链。不具备中断抢占无限循环的能力。

执行期间 `entry_active` 阻止 `proc_deactivate` 和 `proc_reap`，即使回调已经通过
`proc_exit` 标为 ZOMBIE，也不能提前释放正在被 runner 使用的进程；递归调用 runner
返回 `PROC_EBUSY`。MLFQ 在切离进程栈后有专用释放路径，支持挂起的 active 入口；
若挂起进程被退出，则安全地清除 active 标志，随后可回收。
内核回调调用生命周期函数应使用 `proc_*` API，用户系统调用
`sys_*` 入口只接受 `PROC_USER`。

`fork` 不复制 callable 的上下文指针或当前内核调用链；内核进程请求 fork 返回
`PROC_ENOSYS`。`proc_exec` 的镜像替换只接受用户进程，内核进程返回 `PROC_ENOEXEC`。
需要另一个 callable 进程时显式创建；需要运行用户镜像时创建用户进程。

## 文件系统初始化与 exec

`proc_create(table, path, &process)` 从文件系统根目录解析相对路径；
`proc_exec(table, process, path, argc, argv)` 从该进程的 cwd 解析相对路径。
绝对路径从根目录开始。路径使用已有 `fs_lookup`，支持重复斜线、`.`、`..`，
最长 1024 字节。加载时持有可执行 inode 引用，成功和失败都释放它。

启动代码可以用 `proc_image_install` 将可信 `proc_image` 描述及字节复制到普通文件：

```c
static const unsigned char code[] = { 0x73, 0, 0, 0 }; /* 示例 ecall 字节 */
static const struct proc_image image = {
    .name = "init", .code = code, .code_size = sizeof(code), .entry_offset = 0
};
static struct proc_table table;
static struct fs filesystem;
/* buddy 和 kalloc 已初始化；实际调用者必须逐项检查返回码。 */
make_filesystem(&filesystem);
struct inode *root;
inode_get(&filesystem, 1, &root);
proc_image_install(root, "init", &image);
inode_put(root);
proc_table_init_fs(&table, &allocator, &filesystem);
struct proc *init_proc;
int result = proc_create(&table, "/init", &init_proc);
```

`proc_image_install` 先完整写入未命名 inode，最后才发布目录项；失败时回收临时 inode，
不留下半个可执行文件。此后加载依赖文件内容，原 bootstrap 描述和字节无需继续存活。

可执行格式是 native RV64 小端 flat 文件。文件头 `struct proc_exec_header` 恰好 40 字节，
按顺序保存五个 `u64_t`：`magic`、`code_size`、`data_size`、`bss_size`、`entry_offset`。
`PROC_EXEC_MAGIC == 0x31434558454c534f` 包含格式版本 1。头后无填充地紧跟代码字节和
初始化数据字节，BSS 不占文件空间。加载器检查 magic、大小、入口和文件精确长度，
拒绝截断或额外字节。这个格式没有 ELF 解析、动态链接或重定位；后续真实执行时程序
必须按下面的虚拟地址链接，或使用位置无关代码。文件当前存放在 RAM 文件系统中，
还没有磁盘持久化。

`proc_table_init(table, allocator, images, count)` 保留原来的无文件系统注册表路径，
供历史兼容测试使用。它借用的描述和字节必须存活到表销毁，文件 API 返回 `PROC_ENOSYS`。
新内核代码和 shell 使用 `proc_table_init_fs`。

| 区域 | 虚拟地址 | 权限 |
| --- | --- | --- |
| 零页 | `0x0000` | 未映射 |
| 代码 | 从 `0x1000` 开始，按页向上取整 | RX |
| 数据和 BSS | 紧接代码后的页边界 | RW |
| 栈下方保护页及其余空洞 | 未映射 | 无 |
| 用户栈 | `[0x3ff000, 0x400000)`，1 页 | RW |

以上权限目前由内核复制接口检查，没有硬件隔离。所有新页先清零，BSS 和页尾也为零。
入口偏移必须落在代码内，且符合 RISC-V 压缩指令所需的 2 字节对齐。
代码、数据/BSS 和栈合计不得超过 32 页，内核栈单独占 1 页。

最多 8 个参数，每个最多 127 字节，加结尾零字符占 128 字节。
新用户栈保存参数字符串及以零指针结尾的 RV64 `argv[]`，SP 按 16 字节对齐。
入口时 `a0 = argc`，`a1 = argv`，其余通用寄存器清零。

`proc_exec` 接受可信的内核指针；`sys_exec` 从旧进程地址空间复制路径、参数指针和字符串，
不把用户虚拟地址强转为内核指针。先申请并填充完整的新地址空间，再替换旧映像。
失败时释放所有临时页，保留 PID、父进程、旧映像和寄存器。
成功时仍保留 PID、父进程、内核栈、进程状态、cwd 和全部描述符及其偏移。
名称更新为路径的 basename，截断至 `PROC_NAME_MAX - 1` 字节。
原、新映像短暂共存，因此需要额外空闲内存。

## 进程文件接口

`src/include/proc_fs.h` 导出可信内核指针 API，均显式接收 `table` 和 `process`：

| API | 行为 |
| --- | --- |
| `proc_open(..., path, flags)` | 打开普通文件，返回最小空闲 fd |
| `proc_close(..., fd)` | 关闭该描述符，释放一份打开记录引用 |
| `proc_dup(..., fd)` | 返回共享同一打开记录和偏移的新 fd |
| `proc_read(..., fd, dst, count)` | 从当前偏移读入内核缓冲区，返回字节数或负错误码 |
| `proc_write(..., fd, src, count)` | 从内核缓冲区写入，成功后推进偏移 |
| `proc_chdir(..., path)` | 获取新目录引用后替换 cwd；失败保持旧 cwd |
| `proc_mkdir(..., path)` | 创建目录 |
| `proc_unlink(..., path)` | 删除文件名或空目录；保护根、`.` 和 `..` |

fd 范围是 0–15；没有预装 stdin/stdout/stderr 或控制台设备，因此第一次 open 可以返回 0。
独立 open 同一 inode 会得到独立偏移；dup 和 fork 共享打开记录，任一描述符的 I/O
都会推进它们共同的偏移。关闭一个别名不影响其余描述符。

open 标志为 `PROC_O_READ=1`、`PROC_O_WRITE=2`、`PROC_O_CREATE=4`、
`PROC_O_EXCL=8`、`PROC_O_TRUNC=16`。必须指定 READ 或 WRITE；EXCL 需要 CREATE，
TRUNC 需要 WRITE。CREATE 可以打开已有文件，CREATE|EXCL 拒绝已有名字。
耗尽描述符时返回 `PROC_EMFILE`，不会先创建或截断文件。无效 fd 返回 `PROC_EBADF`，
不符合访问模式的 I/O 返回 `PROC_EPERM`。其他文件系统错误映射到 `proc_error`，
这些错误值是项目定义，不依赖宿主 `errno`。

unlink 只去掉目录项；打开记录仍持有 inode，已有 fd 可继续 I/O，最后关闭时才回收。
cwd 也独立持有目录引用，即使目录被 unlink 仍存活到引用释放；其父指针已清除，
相对 `..` 不能恢复已删除的路径，且不能在分离目录中创建新条目。

## fork

`proc_fork` 申请新的槽位、PID、内核栈，深拷贝所有用户页，保留相同虚拟地址和权限。
复制用户寄存器现场，父进程的 `a0` 为子 PID，子进程 `a0` 为零，子状态为 `RUNNABLE`。
不复制父进程的内核栈或内核调用链；下一阶段应将新进程的 `context.ra` 设置为公共
用户态返回入口。分配失败时回滚子进程全部资源，父进程保持不变。
fork 为 cwd 增加独立引用，并为每个继承 fd 增加打开记录引用；父进程之后 chdir
不会改变子进程 cwd，关闭父 fd 也不会关闭子 fd。引用或页面申请失败都回滚子进程资源。
本阶段没有 COW、共享用户页或线程。

## kill、退出与回收

`proc_kill(pid)` 标记 `killed`，睡眠目标变为 `RUNNABLE`。重复标记存活进程成功；
不存在、已退出或零 PID 返回 `PROC_ESRCH`。它不会从另一个进程的内核调用栈中强行释放资源。

- 系统调用入口及返回前检查 `killed`，通过 `proc_exit` 变为 `ZOMBIE`。
- `proc_activate` 遇到已标记目标时直接将它变为 `ZOMBIE`，不激活它。
- `proc_exit` 保存退出码，将其子进程的父指针置空，立即关闭全部 fd 并释放 cwd；
  用户页和内核栈继续保留至 reap。
- `proc_reap` 仅回收非当前进程的 `ZOMBIE`，随后槽位变为 `UNUSED`。

未调用调度器时，`kill` 仍需显式安全点推动退出；附着 MLFQ 后，会自动唤醒并排队睡眠
目标，在选择或安全切换时将其退出。没有 `wait` 系统调用，本阶段由内核管理者显式回收。
没有特殊 PID 1 保护规则，
也没有完整的信号、用户身份或权限模型。

## 系统调用 ABI 与下一阶段接入

系统调用编号放在 `a7`，参数放在 `a0/a1/a2`，普通返回值及负错误码放在 `a0`。

| 编号 | 名称 | 参数 | 成功结果 |
| --- | --- | --- | --- |
| 1 | `fork` | 无 | 父进程返回子 PID，子进程返回 0 |
| 2 | `exec` | 路径用户地址、`argv` 用户地址（0 表示无参数） | 替换用户 PC/SP/参数，转到新入口 |
| 3 | `kill` | PID | 0；杀死自己时退出 |
| 4 | `open` | 路径用户地址、flags | fd |
| 5 | `close` | fd | 0 |
| 6 | `read` | fd、用户缓冲区地址、长度 | 字节数；EOF 为 0 |
| 7 | `write` | fd、用户缓冲区地址、长度 | 字节数 |
| 8 | `dup` | fd | 新 fd |
| 9 | `chdir` | 路径用户地址 | 0 |
| 10 | `mkdir` | 路径用户地址 | 0 |
| 11 | `unlink` | 路径用户地址 | 0 |
| 12 | `exit` | 退出码 | `SYSCALL_EXIT` |

路径通过 `proc_copyinstr` 复制，长度包含终止符的缓冲区上限是 `FS_PATH_MAX + 1`。
用户地址始终经软件映射翻译，不强转为内核指针。`proc_user_range` 在 I/O 前验证完整
用户范围及权限；错误返回 `PROC_EFAULT`，文件内容和打开偏移不改变。read 用小内核
缓冲区分段复制，write 先用 `kalloc` 暂存完整输入，再调用一次 `proc_write`，保留
inode 写入在内存不足时的整体回滚语义。文件系统 sys_exec 的路径和参数也暂存于
内核堆，避免 4 KiB 内核栈溢出。所有这些检查仍依赖调用者串行化。

`syscall_dispatch` 必须在架构层已经识别用户 `ecall` 后调用。它先将旧 PC 加 4，
然后才执行 fork，确保父子都从 ecall 后继续。exec 成功时保留新入口和参数寄存器，
不会用普通返回值覆盖 `argc`。

分发结果是 `SYSCALL_RETURN`、`SYSCALL_EXEC` 或 `SYSCALL_EXIT`。前两者表示可从保存的
现场恢复用户执行；最后一种必须进入退出路径。`proc_activate`/`proc_deactivate`
只记录当前进程及状态，不选择进程、不切换 CPU，也不构成调度实现。

MLFQ 和内核进程上下文切换已完成；后续需要：

1. 完成 Sv39 页表、内核映射及软件映射到硬件 PTE 的转换，确保 exec 后切换页表、刷新 TLB
   和指令缓存，权限由硬件执行。
2. 完成 trap 汇编、CSR 保存、ecall 识别、用户态返回入口；将新进程 `context.ra` 接到该入口。
3. 将时钟中断接到 `mlfq_tick`，在 trap 完整保存现场后按调度请求切换；加入锁及中断保护。
4. 在所有返回用户态及准备运行的路径检查 `killed`；退出进程先切换离开自己的内核栈，
   再清除 `current`，最后允许回收。不能仅清除指针就释放仍在使用的内核栈。
5. 加入 `wait` 与孤儿进程回收、控制台设备 fd；按需要加入 ELF 格式、块设备、缓存和
   持久化文件系统。当前文件加载器和 RAM inode 层已接通。

## 验证

```sh
make stage-test
```

测试代码统一放在根目录 `tests/`，公共头文件统一放在 `src/include/`。
Makefile 通过共享的 `CPPFLAGS += -Isrc/include` 为主机和交叉编译配置头文件搜索路径，
代码直接使用 `#include "proc.h"` 等名称，不需要写相对目录。

主机测试包含现有链表、伙伴结构和 callable 测试，以及新增进程测试：页内容与 BSS 清零、
代码写保护、跨页读写、用户参数校验、fork 返回值与内存隔离、exec 替换与参数布局、
逐处分配失败回滚、进程表耗尽及复用、kill 睡眠目标/自己、安全回收和孤儿父指针清理。
另外验证全部 callable 返回类型、内核进程创建及结果保存、入口只执行一次、运行前/运行中
终止、显式退出、递归执行与提前回收保护、无镜像注册表及内存不足。

QEMU 仍使用原来的 `build/buddy.elf` 名称，链接进程模块和项目自己的 freestanding 内存函数。
`tests/proc_fs_test.c` 仅包含项目头文件，在 QEMU 中验证文件镜像/BSS/参数、损坏镜像回滚、
fork/dup 共享偏移、独立 open、cwd 引用、unlink 后继续 I/O、fd 耗尽无副作用、退出释放、
系统调用用户缓冲区检查、逐次页面分配失败和完整内存恢复，成功打印 `PROC FS OK`。
原有 `BUDDY OK`、`KALLOC OK`、`INODE OK`、`PROC OK`、`CALLABLE PROC OK` 和
`MLFQ OK` 检查继续执行。`make stage-test` 通过六组历史主机测试、QEMU 内核测试和
384 项 UART shell 检查（含独立 shell cwd 和文本编辑器）；新内核测试和内核源码不依赖标准 C 库。
尚未运行用户态 fork/exec，也未安装硬件时钟中断。
