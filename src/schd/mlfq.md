# MLFQ 调度模块

模块由两个部分组成：`mlfq.c` 实现独立于硬件的策略和队列，`riscv.c` 使用
`asm/swtch.s` 驱动内核 callable 进程在各自的内核栈运行。
现阶段支持单 hart、协作式切换；用户态执行与硬件时钟中断尚未实现。

## 策略

优先级从 0（最高）到 3（最低）。每级使用现有 `list_meta` 实现 FIFO 就绪队列，
节点内嵌于 `proc.schedule`，无需分配额外内存。同一进程最多在一个队列中出现一次。
正在运行、睡眠、ZOMBIE 和 UNUSED 进程不会出现在就绪队列。

默认配置如下，单位都是调用 `mlfq_tick` 所代表的一个时钟 tick。

| 优先级 | 单次时间片 quantum | 本级累计 CPU 配额 allotment |
| --- | --- | --- |
| 0 | 1 | 2 |
| 1 | 2 | 4 |
| 2 | 4 | 8 |
| 3 | 8 | 16 |

- 新建和 fork 子进程进入最高级。
- 优先选择最高非空队列；同级轮转。
- 每个 CPU tick 扣除当前进程的时间片及本级累计配额。
- 时间片用尽后回到同级队尾并重置时间片，累计配额不重置。
- 本级配额用尽后降一级，并获得新级别的时间片及配额；最低级保持原级轮转。
- 主动 yield、被高优先级任务抢先以及睡眠/唤醒均保留未用完的时间片和配额。
  在配额刚好用尽时请求睡眠，也先降级，避免通过频繁阻塞规避 CPU 计费。
- 新的高优先级任务变为可运行后，`mlfq_should_reschedule` 请求当前较低级进程切换。
- 默认每 64 tick 全局 boost：就绪、运行及睡眠进程重置到最高级，重新获得预算。
  合并就绪队列时保持各队列原有 FIFO 顺序，当前进程请求切换并排在已等待进程之后。
  在同一 tick 发生预算耗尽和 boost 时，boost 的新预算生效。

`mlfq_init` 接受自定义 `mlfq_config`；时间片必须大于零，配额不得小于时间片。
`boost_interval = 0` 可禁用周期 boost。总 tick 计数饱和于 `U64_MAX`；
boost 使用单独的周期计数，因此不依赖总 tick 计数回绕。

## 生命周期接入

`proc_table.scheduler` 最多附着一个调度器。初始化时，调度器接管已有 RUNNABLE/SLEEPING
进程；要求没有当前进程、运行中的 callback 或未完成创建的 USED 槽位。
初始化参数检查失败时不修改进程表、调度器或队列。

创建、fork、kill、exit 和回收通过进程模块中的 hook 自动维护队列。`exec` 保留原调度
预算，不能通过更换镜像重置优先级。进程槽位回收前节点已摘除，槽位复用不会留下悬空链表。

附着后，调用者应通过调度接口管理 current，不再直接调用 `proc_deactivate`；
`proc_activate` 也不能直接激活仍在队列中的进程。未附着调度器的旧手动进程 API 继续有效。

睡眠使用非空的通道指针标识等待事件。`mlfq_wakeup` 唤醒该通道上的全部等待者，返回数量。
通道按地址比较，不访问其内容，调用者保证其身份在等待期间稳定。
单 hart 下必须将检查等待条件、入睡、更新条件和唤醒串行执行，避免丢失唤醒。

`kill` 会唤醒并排队睡眠目标。调度时跳过已 killed 的进程，将其标为 ZOMBIE；当前进程的
终止请求会在下一次安全切换点生效。已挂起的内核调用链在退出时被丢弃，不会恢复，也不会
自动析构 callback 借用的上下文或其自行持有的资源。

回收仍由 `proc_reap` 显式完成，不会自动清除退出码或 callable 结果。
调度器析构拒绝正在运行的驱动、当前进程、尚未返回的 callback 和睡眠进程。
内核栈、进程表、调度器和借用的 callback 上下文必须在挂起期间保持有效。

## 内核执行示例

在已初始化的伙伴分配器及进程表上运行：

```c
static struct mlfq scheduler;

static i32_t worker(void *arg) {
    unsigned int *work_done = arg;
    for (unsigned int i = 0; i < 8; i++) {
        ++*work_done;
        int status = mlfq_tick(&scheduler); /* 暂时代替硬件时钟 tick。 */
        if (status < 0) return status;
        status = mlfq_checkpoint(&scheduler);
        if (status != 0) return status;
    }
    return 0;
}

/* table 已初始化且没有 current。生产调用应检查每个返回码。 */
unsigned int work_done = 0;
struct callable entry = callable_from_fni32p(worker, &work_done);
struct proc *process;
if (mlfq_init(&scheduler, &table, NULL) == 0) {
    if (proc_create_callable(&table, "worker", &entry, &process) == 0) {
        u64_t pid = process->pid;
        if (mlfq_run(&scheduler) == 0 && process->pstatus == ZOMBIE)
            (void)proc_reap(&table, pid);
    }
    (void)mlfq_destroy(&scheduler);
}
```

`mlfq_run` 选择进程，在第一次运行时设置 trampoline，然后通过 `swtch` 切到进程内核栈。
trampoline 调用已有 `proc_run_callable`，因此入口及结果的类型处理完全复用 callable API。
`mlfq_yield`、`mlfq_sleep`、`mlfq_checkpoint` 保存当前内核调用链并切回调度器栈，
下一次被选中时从原调用点继续；callback 的开头只执行一次。

`mlfq_checkpoint` 仅在需要轮转、降级、高优先级抢先、boost 或终止时切换。
`mlfq_yield` 无条件主动轮转；`mlfq_sleep` 挂起到指定通道。这三个接口只能在当前
callable 入口内调用，嵌套调用 `mlfq_run` 被拒绝。

没有就绪进程时，`mlfq_run` 返回 0；睡眠进程的内核调用链仍然保留。外部事件调用
`mlfq_wakeup` 后，可再次调用 `mlfq_run`，从旧睡眠调用点恢复。
本阶段 callback 不调用 yield/checkpoint/sleep 时会一直占用 CPU，无法抢占死循环。

## 策略层接口和后续 trap 接入

主机测试可以直接使用这些接口驱动保存的进程状态，无需汇编切换：

1. `mlfq_dispatch(s, &p)`：返回 1 表示已选中并激活进程，0 表示空闲，负值表示错误。
2. `mlfq_tick(s)`：计费并返回是否需要切换；不直接操作 CPU 上下文。
3. 架构层完整保存寄存器并切离进程栈后，调用
   `mlfq_complete(s, MLFQ_PREEMPT/MLFQ_YIELD/MLFQ_BLOCK/MLFQ_EXIT, channel)`。
4. 再次 dispatch；退出进程可在 current 已清除后由内核管理者 reap。

策略层可选择用户进程并维护其优先级、时间预算及生命周期；当前 RISC-V 执行后端只能
执行内核 callable。遇到用户进程时 `mlfq_run` 返回 `PROC_ENOSYS`，将该进程重新入队，
保留其资源，供后续用户执行后端接管。

`struct context` 的汇编布局为 `ra@0, sp@8, s0@16 ... s11@104`，共 112 字节。
编译时静态断言验证布局。此 switch 保存的是协作式 C ABI 现场；未来硬件 trap 层需要
额外保存所有被中断代码的调用者保存寄存器和 CSR，再使用调度策略。

将来需设置硬件时钟、安装 trap 入口，并将每次中断接到 `mlfq_tick`。收到切换请求后，
应在安全的 trap 返回/内核边界切换。多 hart 或启用中断后，需要给伙伴系统、进程表和
就绪队列加锁，并将睡眠与条件保护锁配合使用。当前实现尚未写入 CLINT 或安装中断处理程序。

## 测试

```sh
make test
make qemu
```

`tests/mlfq_test.c` 检查优先级顺序、同级轮转、所有级别降级与最低级轮转、累计 CPU
配额、yield 和睡眠不能重置预算、高优先级抢先、周期/手动 boost、睡眠目标 boost、
fork 自动入队、exec 保留预算、kill/回收/槽位复用、非法输入、调度器附着及队列不变量。

QEMU 运行三个 callable CPU 作业，验证真实内核栈地址、局部变量在切换后仍有效、
`1,2,3,1,2,3` 的初始轮转以及低级队列的双 tick 连续执行。
另外检查睡眠/唤醒、空闲返回后再次恢复、终止睡眠调用链及用户后端不可用时的安全退回。
成功时在现有检查之后打印 `MLFQ OK` 并退出。
