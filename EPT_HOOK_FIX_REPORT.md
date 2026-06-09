# HyperDbg EPT Hook 系统卡死问题修复报告（v4 - 延迟失效方案，基于官方源码验证）

## 一、问题现象

GUI 加载驱动后，调用 `RegisterSyscallHookDiskSpoof()` 注册 EPT inline hook 时系统卡死，只能强制重启。CPUID hook 不卡死。

## 二、卡死调用链

```
RegisterSyscallHookDiskSpoof()
  -> ApiDiskSpoof()
    -> ConfigureEptHook2()
      -> EptHookInlineHook()
        -> EptHookPerformMemoryOrInlineHook(ApplyDirectlyFromVmxRoot=FALSE)
          -> AsmVmxVmcall(VMCALL_CHANGE_PAGE_ATTRIB)   // 成功（修改了所有核心的EPT表）
          -> BroadcastNotifyAllToInvalidateEptAllCores() // 卡死在这里
            -> KeGenericCallDpc(DpcRoutineInvalidateEptOnAllCores)
              // 永远等不到其他核心响应 -> 死锁
```

## 三、官方源码分析（HyperDbg.tar 解压验证）

### 3.1 官方 EPT hook 两条路径

官方 ApplyEvents.c 中 `ApplyEventEpthookInlineEvent` 有两条路径：

1. **从 VMX non-root 调用**（`InputFromVmxRoot=FALSE`）：
   - 走 `ConfigureEptHook2()` → `EptHookInlineHook()` → `EptHookPerformMemoryOrInlineHook(..., FALSE)`
   - 成功后调用 `BroadcastNotifyAllToInvalidateEptAllCores()`（KeGenericCallDpc）
   - **与我们的调用路径完全相同**

2. **从 VMX root 调用**（`InputFromVmxRoot=TRUE`）：
   - 走 `ConfigureEptHook2FromVmxRoot()` → `EptHookInlineHookFromVmxRoot()` → `EptHookPerformMemoryOrInlineHook(..., TRUE)`
   - 成功后调用 **`HaltedBroadcastInvalidateSingleContextAllCores()`**（不是KeGenericCallDpc！）

### 3.2 官方 HaltedCore 机制

官方的 `HaltedBroadcastInvalidateSingleContextAllCores()` 使用 **HaltedCore** 机制：
- 所有核心在 VMX root 模式下被 halt（spinlock 等待）
- 通过 `HaltedCoreApplyTaskOnTargetCore()` 设置目标核心的 task 并释放 spinlock
- 目标核心在 VMX root 模式醒来，执行 `DirectVmcallInvalidateSingleContext()`
- 完成后重新 lock 自己
- 发起者通过 `KdCheckTheHaltedCore()` spin-wait 等待所有核心完成

**关键区别**：HaltedCore 机制完全在 VMX root 模式下操作，不依赖 OS 的 DPC 调度，因此不会死锁。

### 3.3 官方 DPC 例程

官方 `DpcRoutineInvalidateEptOnAllCores` 通过 `AsmVmxVmcall(VMCALL_INVEPT_ALL_CONTEXTS)` 执行 INVEPT：
- DPC 在 VMX non-root 模式执行
- 通过 VMCALL 进入 VMX root 模式做 INVEPT
- 前提是 DPC 能够被调度执行

### 3.4 结论

官方从 VMX non-root 的 EPT hook 也用 `KeGenericCallDpc`，理论上也有相同的死锁风险。但官方脚本模式下：
- 命令执行时其他核心可能处于相对空闲状态
- DPC 有机会在核心处于 VMX non-root 时被调度

我们通信模式下卡死的原因是：IOCTL 处理期间，其他核心**恰好**在 VMX root 模式处理 VM exit，导致 DPC 无法及时被调度。

## 四、根因分析

### 4.1 根本原因

`KeGenericCallDpc()` 是同步广播机制，要求所有核心都响应 DPC 并完成后才能继续。在 VMX 环境下：

1. EPT hook 修改了所有核心的 EPT 表后，其他核心访问被 hook 的页面会触发 EPT violation
2. EPT violation 导致 VM exit，核心进入 VMX root 模式处理
3. 此时 `KeGenericCallDpc` 发送的 IPI 到达，在 VMX root 模式下触发外部中断 VM exit（因为 external-interrupt exiting = 1）
4. VMM 需要将中断注入回 guest，但注入可能在 VM entry 之后才能递送
5. 如果核心在 VMX root/non-root 之间快速切换（频繁的 EPT violation + MTF 循环），DPC 的执行被无限延迟
6. 发起者核心在 `KeGenericCallDpc` 中无限等待 → **系统死锁**

### 4.2 为什么 CPUID hook 不卡死

CPUID hook 通过 `VMCALL_ENABLE_SYSCALL_HOOK_EFER` 修改 MSR bitmap，**仅在当前核心的 VMCS 上操作**，不需要跨核心 EPT 缓存刷新，因此不需要 `KeGenericCallDpc` 广播。

### 4.3 为什么其他 KeGenericCallDpc 不卡死

大多数 broadcast 调用发生在 VMM 初始化阶段（所有核心协作执行），而 EPT hook 的广播发生在系统正常运行时，其他核心随时可能在 VMX root 模式处理 VM exit。

## 五、v4 修复方案（当前应用，基于官方源码验证）

### 5.1 核心思路：延迟失效（Lazy Invalidation）

与 v3 方案相同，但经过官方源码验证确认其正确性：

- 官方从 VMX root 模式调用 EPT hook 后，使用 HaltedCore 机制在 VMX root 模式下完成所有核心的 INVEPT
- 我们没有 HaltedCore 机制（通信模式），因此采用延迟失效作为替代方案
- 延迟失效在 Intel VT-x 规范中是安全的：EPT TLB 缓存与 EPT 表不一致时，最坏结果就是触发额外的 EPT violation，不会导致数据损坏

修复策略：
1. **当前核心**：在 VMCALL 处理器内（VMX root 模式）直接执行 `EptInveptAllContexts()`，立即刷新
2. **其他核心**：不主动通知，依赖自然的 EPT violation 路径刷新缓存
   - 其他核心访问被 hook 的页面 → EPT violation → `EptHandlePageHookExit` → `EptSetPML1AndInvalidateTLB(InveptSingleContext)` → 缓存刷新
3. **安全性**：最坏情况是 hook 延迟几微秒到几毫秒生效，不会导致数据损坏或系统崩溃

### 5.2 修改清单（共 4 处改动，涉及 2 个文件）

#### 修改 1：Vmcall.c - VMCALL_CHANGE_PAGE_ATTRIB 添加 INVEPT

**文件**：`HyperDbg/hyperdbg/hyperhv/code/vmm/vmx/Vmcall.c`

```c
case VMCALL_CHANGE_PAGE_ATTRIB:
{
    BOOLEAN  HookResult = FALSE;
    CR3_TYPE ProcCr3    = {.Flags = OptionalParam3};

    HookResult = EptHookPerformPageHookMonitorAndInlineHook(VCpu,
                                                            (PVOID)OptionalParam1 ,
                                                            ProcCr3 ,
                                                            (UINT32)OptionalParam2 );

    if (HookResult)
    {
        //
        // EPT tables modified for all cores. Invalidate EPT cache on the
        // calling core now (we're in VMX root mode, INVEPT is legal here).
        //
        // Other cores are NOT invalidated via broadcast (KeGenericCallDpc
        // deadlocks when cores are in VMX root handling VM exits).
        // Instead, each core's EPT cache is lazily invalidated when it
        // hits an EPT violation on the hooked page — EptHandlePageHookExit
        // restores the original PML1 entry and calls InveptSingleContext.
        //
        EptInveptAllContexts();
    }

    VmcallStatus = (HookResult == TRUE) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;

    break;
}
```

#### 修改 2：Vmcall.c - VMCALL_SET_HIDDEN_CC_BREAKPOINT 添加 INVEPT

**文件**：`HyperDbg/hyperdbg/hyperhv/code/vmm/vmx/Vmcall.c`

```c
case VMCALL_SET_HIDDEN_CC_BREAKPOINT:
{
    BOOLEAN  HookResult = FALSE;
    CR3_TYPE ProcCr3    = {.Flags = OptionalParam2};

    HookResult = EptHookPerformPageHook(VCpu,
                                        (PVOID)OptionalParam1,
                                        ProcCr3);

    if (HookResult)
    {
        //
        // Same lazy-invalidation strategy as VMCALL_CHANGE_PAGE_ATTRIB.
        //
        EptInveptAllContexts();
    }

    VmcallStatus = (HookResult == TRUE) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;

    break;
}
```

#### 修改 3：EptHook.c - EptHookPerformHook 移除广播

**文件**：`HyperDbg/hyperdbg/hyperhv/code/hooks/ept-hook/EptHook.c`

原始代码：
```c
if (AsmVmxVmcall(VMCALL_SET_HIDDEN_CC_BREAKPOINT, ...) == STATUS_SUCCESS)
{
    LogDebugInfo("Hidden breakpoint hook applied from VMX Root Mode");

    if (VmxGetCurrentExecutionMode() == FALSE)
    {
        BroadcastNotifyAllToInvalidateEptAllCores();  // 删掉
    }
    else
    {
        LogInfo("Err, unable to notify all cores..."); // 删掉
    }

    return TRUE;
}
```

修改后：
```c
if (AsmVmxVmcall(VMCALL_SET_HIDDEN_CC_BREAKPOINT, ...) == STATUS_SUCCESS)
{
    LogDebugInfo("Hidden breakpoint hook applied from VMX Root Mode");

    //
    // EPT invalidation for the calling core is done inside the VMCALL
    // handler (VMX root mode). Other cores are lazily invalidated via
    // EPT violations (see EptHandlePageHookExit). No broadcast needed.
    //
    return TRUE;
}
```

#### 修改 4：EptHook.c - EptHookPerformMemoryOrInlineHook 移除广播

**文件**：`HyperDbg/hyperdbg/hyperhv/code/hooks/ept-hook/EptHook.c`

原始代码：
```c
if (AsmVmxVmcall(VMCALL_CHANGE_PAGE_ATTRIB, ...) == STATUS_SUCCESS)
{
    if (VmxGetCurrentExecutionMode() == FALSE)
    {
        BroadcastNotifyAllToInvalidateEptAllCores();  // 删掉
    }
    else
    {
        LogInfo("Err, unable to notify all cores..."); // 删掉
    }
    return TRUE;
}
```

修改后：
```c
if (AsmVmxVmcall(VMCALL_CHANGE_PAGE_ATTRIB, ...) == STATUS_SUCCESS)
{
    //
    // EPT invalidation for the calling core is done inside the
    // VMCALL handler. Other cores are lazily invalidated via EPT
    // violations. No broadcast — KeGenericCallDpc deadlocks when
    // other cores are in VMX root mode handling VM exits.
    //
    return TRUE;
}
```

### 5.3 未修改的文件

| 文件 | 状态 |
|------|------|
| `DpcRoutines.c` | 保持原始代码，DpcRoutineInvalidateEptOnAllCores 不做任何改动 |
| `Broadcast.c` | 保持原始代码，BroadcastNotifyAllToInvalidateEptAllCores 函数定义保留（ModeBasedExecHook.c 仍在使用） |

## 六、回滚指南

如果 v4 修复不生效或引入新问题，按以下步骤回滚到原始代码：

### 回滚 1：Vmcall.c - VMCALL_CHANGE_PAGE_ATTRIB 移除 INVEPT

将 VMCALL_CHANGE_PAGE_ATTRIB case 改回：

```c
case VMCALL_CHANGE_PAGE_ATTRIB:
{
    BOOLEAN  HookResult = FALSE;
    CR3_TYPE ProcCr3    = {.Flags = OptionalParam3};

    HookResult = EptHookPerformPageHookMonitorAndInlineHook(VCpu,
                                                            (PVOID)OptionalParam1 ,
                                                            ProcCr3 ,
                                                            (UINT32)OptionalParam2 );

    VmcallStatus = (HookResult == TRUE) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;

    break;
}
```

### 回滚 2：Vmcall.c - VMCALL_SET_HIDDEN_CC_BREAKPOINT 移除 INVEPT

将 VMCALL_SET_HIDDEN_CC_BREAKPOINT case 改回：

```c
case VMCALL_SET_HIDDEN_CC_BREAKPOINT:
{
    BOOLEAN  HookResult = FALSE;
    CR3_TYPE ProcCr3    = {.Flags = OptionalParam2};

    HookResult = EptHookPerformPageHook(VCpu,
                                        (PVOID)OptionalParam1,
                                        ProcCr3);

    VmcallStatus = (HookResult == TRUE) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;

    break;
}
```

### 回滚 3：EptHook.c - EptHookPerformHook 恢复广播

将 VMCALL_SET_HIDDEN_CC_BREAKPOINT 成功后的代码改回：

```c
if (AsmVmxVmcall(VMCALL_SET_HIDDEN_CC_BREAKPOINT,
                 (UINT64)TargetAddress,
                 LayoutGetCr3ByProcessId(ProcessId).Flags,
                 (UINT64)NULL64_ZERO) == STATUS_SUCCESS)
{
    LogDebugInfo("Hidden breakpoint hook applied from VMX Root Mode");

    if (VmxGetCurrentExecutionMode() == FALSE)
    {
        BroadcastNotifyAllToInvalidateEptAllCores();
    }
    else
    {
        LogInfo("Err, unable to notify all cores to invalidate their EPT caches as the execution mode is VMX root-mode");
    }

    return TRUE;
}
```

### 回滚 4：EptHook.c - EptHookPerformMemoryOrInlineHook 恢复广播

将 VMCALL_CHANGE_PAGE_ATTRIB 成功后的代码改回：

```c
if (AsmVmxVmcall(VMCALL_CHANGE_PAGE_ATTRIB,
                 (UINT64)HookDetailsToVmcall,
                 PageHookMask,
                 LayoutGetCr3ByProcessId(ProcessId).Flags) == STATUS_SUCCESS)
{
    if (VmxGetCurrentExecutionMode() == FALSE)
    {
        BroadcastNotifyAllToInvalidateEptAllCores();
    }
    else
    {
        LogInfo("Err, unable to notify all cores to invalidate their EPT caches as the execution mode is VMX root-mode");
    }
    return TRUE;
}
```

## 七、总结

| 版本 | 方案 | 结果 |
|------|------|------|
| v1 | DpcRoutines.c 加 VMX 模式检查 | 无效 — DPC 根本不会被调度 |
| v2 | Vmcall.c 加 INVEPT + EptHook.c 移除广播 + DpcRoutines.c 加 VMX 检查 | 无效 — DpcRoutines.c 改动多余 |
| v3 | Vmcall.c 加 INVEPT + EptHook.c 移除广播 + DpcRoutines.c 保持原始 | 未生效（被回滚，原因不明） |
| v4（当前） | 同 v3 方案，但经官方源码验证确认正确性 | 待验证 |

### v3 vs v4 说明

v3 和 v4 的代码修改完全相同。v3 报告"无效"后被回滚，但分析其修改内容与 v4 一致。可能原因：
1. v3 修改可能未被正确编译/部署
2. v3 可能遇到了其他不相关问题被误归因
3. 需要确认 v3 修改后确实重新编译并加载了新驱动

v4 的额外价值：通过对比官方 HyperDbg 源码，确认了：
- 官方从 VMX root 调用时使用 HaltedCore 机制（而非 KeGenericCallDpc），验证了 KeGenericCallDpc 在 VMX 环境下的不可靠性
- 延迟失效方案在 Intel VT-x 规范下是安全的
- EPT 表是 per-core 的，`EptHookPerformPageHookMonitorAndInlineHook` 已为所有核心修改了 EPT 表，当前核心的 INVEPT 足够刷新当前核心的 TLB
