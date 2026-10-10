# 基础进程模块（当前阶段）

这一版实现进程生命周期、callable 内核进程入口，以及 `fork`、`exec`、`kill` 的
内核逻辑和 C 系统调用分发接口。
当前启动环境仍是 RISC-V M-mode，没有用户态 trap 入口或硬件用户页表。
MLFQ 已支持内核 callable 进程的协作式切换；用户镜像仍仅由内核检查生命周期，尚不执行。

## 数据结构与内存所有权

- `proc_table`：固定 32 个槽位、内置镜像注册表、当前进程指针、单调递增 PID。
  PID 不随槽位复用；失败的创建可能留下 PID 间隙，超出有符号 64 位范围后拒绝创建。
- `proc`：PID、父进程、状态、终止标志、退出码、名称、独立内核栈、内核上下文、
  进程种类、完整用户通用寄存器现场和地址空间；内核进程另保存 callable 入口、
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

调用者必须串行访问进程表及分配器。当前只支持单 hart、无抢占的测试环境；下一阶段需要
在进程表和伙伴系统外围加入锁或中断保护。初始化后的表、分配器和元数据不能移动、复制
或在仍有存活进程时重新初始化。旧的 `mm.c` 页分配器不能同时管理同一物理内存区域。

## Callable 内核进程

`PROC_USER` 使用原来的 flat 镜像和用户寄存器现场；`PROC_KERNEL` 的执行入口是
`struct callable`，没有用户映射，也不把内核函数地址写入用户 PC。
两种进程共享 PID、状态、终止与回收规则。

原来 `types.h` 中的 callable API 已提取到 `src/include/callable.h`。`types.h` 保留
固定宽度整数别名，并通过兼容 include 继续导出 callable。整数别名使用 `<stdint.h>`
的 typedef，避免宏展开，也使 callable 的 64 位结果类型与进程模块的 `uint64_t/int64_t`
保持一致。原有构造函数、枚举、`callable_call` 和 `FN_V_V(void *)` 签名保留。

新增 `callable_valid` 可在创建进程前检查类型标记和函数是否有效，不执行函数。
`callable_invoke` 使用 `struct callable_result` 保存返回类型及对应 union 成员，
不要求进程层猜测返回值的宽度。原有 8/16/32/64 位有符号、无符号整数、指针、void
以及有无上下文参数的全部类型都支持；失败时结果保持不变。

下面是当前阶段的显式运行方式（各返回码均应检查）：

```c
static int32_t worker(void *arg) {
    unsigned int *count = arg;
    ++*count;
    return 7;
}

/* table 已初始化；即使没有镜像注册表，也可创建 callable 进程。 */
unsigned int count = 0;
struct callable entry = callable_from_fni32p(worker, &count);
struct proc *process;
if (proc_create_callable(&table, "worker", &entry, &process) == 0) {
    uint64_t pid = process->pid;
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

## 无文件系统时的 exec

调用者提供只读且长期有效的 `proc_image[]`：

```c
static const unsigned char code[] = { 0x73, 0, 0, 0 }; /* 示例 ecall 字节 */
static const struct proc_image images[] = {
    { .name = "init", .code = code, .code_size = sizeof(code), .entry_offset = 0 },
};
static struct proc_table table;
/* allocator 必须已经初始化，管理可直接访问且未保留的物理内存。 */
proc_table_init(&table, &allocator, images, 1);
struct proc *init_proc;
int result = proc_create(&table, "init", &init_proc);
```

镜像布局如下。`exec("init", argv)` 中的名称用于匹配注册表，不读取磁盘文件。
镜像是 flat code/data/BSS 描述，没有 ELF 解析、动态链接或重定位；将来执行时，程序必须
按这些虚拟地址链接，或使用位置无关代码。

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

`proc_exec` 接受可信的内核指针；`sys_exec` 从旧进程地址空间复制名称、参数指针和字符串，
不把用户虚拟地址强转为内核指针。先申请并填充完整的新地址空间，再替换旧映像。
失败时释放所有临时页，保留 PID、父进程、旧映像和寄存器。
成功时仍保留 PID、父进程、内核栈及进程状态。原、新映像短暂共存，因此需要额外空闲内存。

## fork

`proc_fork` 申请新的槽位、PID、内核栈，深拷贝所有用户页，保留相同虚拟地址和权限。
复制用户寄存器现场，父进程的 `a0` 为子 PID，子进程 `a0` 为零，子状态为 `RUNNABLE`。
不复制父进程的内核栈或内核调用链；下一阶段应将新进程的 `context.ra` 设置为公共
用户态返回入口。分配失败时回滚子进程全部资源，父进程保持不变。
本阶段没有 COW、共享页、线程和文件描述符继承。

## kill、退出与回收

`proc_kill(pid)` 标记 `killed`，睡眠目标变为 `RUNNABLE`。重复标记存活进程成功；
不存在、已退出或零 PID 返回 `PROC_ESRCH`。它不会从另一个进程的内核调用栈中强行释放资源。

- 系统调用入口及返回前检查 `killed`，通过 `proc_exit` 变为 `ZOMBIE`。
- `proc_activate` 遇到已标记目标时直接将它变为 `ZOMBIE`，不激活它。
- `proc_exit` 保存退出码，将其子进程的父指针置空，保留用户页和内核栈。
- `proc_reap` 仅回收非当前进程的 `ZOMBIE`，随后槽位变为 `UNUSED`。

未调用调度器时，`kill` 仍需显式安全点推动退出；附着 MLFQ 后，会自动唤醒并排队睡眠
目标，在选择或安全切换时将其退出。没有 `wait` 系统调用，本阶段由内核管理者显式回收。
没有特殊 PID 1 保护规则，
也没有完整的信号、用户身份或权限模型。

## 系统调用 ABI 与下一阶段接入

系统调用编号放在 `a7`，参数放在 `a0/a1`，普通返回值及负错误码放在 `a0`。

| 编号 | 名称 | 参数 | 成功结果 |
| --- | --- | --- | --- |
| 1 | `fork` | 无 | 父进程返回子 PID，子进程返回 0 |
| 2 | `exec` | 名称用户地址、`argv` 用户地址（0 表示无参数） | 替换用户 PC/SP/参数，转到新入口 |
| 3 | `kill` | PID | 0；杀死自己时退出 |

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
5. 后续加入 `wait` 与孤儿进程回收；有文件系统后，将镜像注册表替换为文件加载器。

## 验证

```sh
make test
make qemu
```

测试代码统一放在根目录 `tests/`，公共头文件统一放在 `src/include/`。
Makefile 通过共享的 `CPPFLAGS += -Isrc/include` 为主机和交叉编译配置头文件搜索路径，
代码直接使用 `#include "proc.h"` 等名称，不需要写相对目录。

主机测试包含现有链表、伙伴结构和 callable 测试，以及新增进程测试：页内容与 BSS 清零、
代码写保护、跨页读写、用户参数校验、fork 返回值与内存隔离、exec 替换与参数布局、
逐处分配失败回滚、进程表耗尽及复用、kill 睡眠目标/自己、安全回收和孤儿父指针清理。
另外验证全部 callable 返回类型、内核进程创建及结果保存、入口只执行一次、运行前/运行中
终止、显式退出、递归执行与提前回收保护、无镜像注册表及内存不足。

QEMU 仍使用原来的 `build/buddy.elf` 名称，链接进程模块和最小 freestanding 内存函数。
启动时验证进程创建、三个系统调用的 C 分发及资源回收，串口打印 `BUDDY OK` 和 `PROC OK`，
再执行 callable 内核进程并打印 `CALLABLE PROC OK`，随后验证 MLFQ 中真实进程栈切换、
轮转、睡眠、唤醒及终止，打印 `MLFQ OK`，成功退出。
尚未运行用户态 fork/exec，也未安装硬件时钟中断。
