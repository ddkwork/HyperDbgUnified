---
name: "hyperdbg-api-refactor"
description: "精简 HyperDbg：移除脚本引擎，将命令转为 IOCTL API 函数，驱动层提供 SSDT/CPUID 等直通接口。适用于需要纯 API 模式简化调试器/ Hook 工具的场景。"
---

# HyperDbg API Refactor — 精简重构计划

## 目标

将 HyperDbg 从「调试器 CLI + 脚本引擎」架构，重构为 **「驱动 API 层 + 用户态 IOCTL 直通」** 架构。

```
重构前：
用户态 CLI → 命令解析器 → libhyperdbg → IOCTL → 事件系统 → 脚本引擎 → 动作执行

重构后：
用户态 GUI → IOCTL API 直通 → 驱动 API 处理函数 → 直接操作 VT 硬件
```

## 核心改动

### 1. 移除脚本引擎

| 文件 | 操作 |
|------|------|
| `hyperkd/code/debugger/script-engine/ScriptEngine.c` | 删除 |
| `hyperkd/header/debugger/script-engine/ScriptEngine.h` | 删除 |
| `script-engine/` (整个目录) | 删除 |
| `script-eval/` (整个目录) | 删除 |
| `hyperkd/CMakeLists.txt` | 移除 script-engine/script-eval 引用 |

### 2. 简化事件系统

| 文件 | 操作 |
|------|------|
| `hyperhv/code/interface/Dispatch.c` | 移除 `VmmCallbackTriggerEvents` 调用链，改为直接调用 handler |
| `hyperhv/code/interface/Callback.c` | 简化或删除回调转发 |
| `hyperkd/code/debugger/core/Debugger.c` | 移除 `DebuggerTriggerEvents` 中的脚本相关逻辑 |

### 3. 新增 IOCTL API 层

在 `hyperkd` 中新增 API 分发中心：

```
hyperkd/code/debugger/api/
  ├── ApiDispatch.c          ← IOCTL API 主分发器
  ├── ApiCpuid.c             ← CPUID spoof API
  ├── ApiSsdt.c              ← SSDT hook API
  ├── ApiEpt.c               ← EPT hook API
  ├── ApiMsr.c               ← MSR hook API
  ├── ApiIo.c                ← IO port hook API
  ├── ApiSyscall.c           ← SYSCALL hook API
  ├── ApiMemory.c            ← 内存读写 API
  └── ApiProcess.c           ← 进程/线程 API
```

每个 API 函数签名：

```c
NTSTATUS
ApiHandler(PVOID InputBuffer, UINT32 InputSize, PVOID OutputBuffer, UINT32 OutputSize);
```

### 4. 新增 IOCTL 定义

```c
#define IOCTL_HYPERDBG_API_BASE       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HYPERDBG_CPUID_SPOOF    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HYPERDBG_SSDT_HOOK      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HYPERDBG_EPT_HOOK       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HYPERDBG_MSR_HOOK       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x904, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HYPERDBG_IO_HOOK        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x905, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HYPERDBG_SYSCALL_HOOK   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x906, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HYPERDBG_MEM_READ       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x910, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HYPERDBG_MEM_WRITE      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x911, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HYPERDBG_TERMINATE      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x9FF, METHOD_BUFFERED, FILE_ANY_ACCESS)
```

### 5. libhyperdbg 命令转 API

`libhyperdbg/code/debugger/commands/` 下的每个命令转为 API 函数：

| 原命令 | 新 API | 说明 |
|--------|--------|------|
| `extension-commands/cpuid.cpp` | `ApiCpuid.c` | IOCTL 传 leaf + 伪造值，驱动直接查表 |
| `extension-commands/epthook.cpp` | `ApiEpt.c` | IOCTL 传地址 + 回调类型 |
| `extension-commands/msrread.cpp` | `ApiMsr.c` | IOCTL 传 msr index |
| `extension-commands/syscall-sysret.cpp` | `ApiSyscall.c` | IOCTL 传 syscall num + handler |
| `extension-commands/ioin.cpp / ioout.cpp` | `ApiIo.c` | IOCTL 传 port + value |
| `debugging-commands/r.cpp` | `ApiRegs.c` | IOCTL 读/写 guest 寄存器 |
| `debugging-commands/memory相关` | `ApiMemory.c` | IOCTL 读/写 guest 物理/虚拟内存 |
| `meta-commands/process.cpp` | `ApiProcess.c` | IOCTL 枚举/切换进程 |

### 6. 删除冗余文件

| 路径 | 原因 |
|------|------|
| `libhyperdbg/` (整个) | 用户态库不再需要，功能移至驱动 API |
| `script-engine/` | 移除脚本引擎 |
| `script-eval/` | 移除脚本求值器 |
| `symbol-parser/` | 移除符号解析（GUI 自己做） |
| `hyperdbg-cli/` | 移除 CLI |
| `hyperdbg-test/` | 移除测试代码（可选） |

### 7. CPUID Hook 轻量化

在 `hyperhv/code/vmm/vmx/Hv.c` 的 `HvHandleCpuid` 中，加查表逻辑：

```c
// 查 CPUID spoof 表（优先于事件系统）
CPUID_SPOOF_RESULT * spoof = CpuidSpoofLookup(Regs->rax, Regs->rcx);
if (spoof) {
    Regs->rax = spoof->eax;
    Regs->rbx = spoof->ebx;
    Regs->rcx = spoof->ecx;
    Regs->rdx = spoof->edx;
    HvSuppressRipIncrement(VCpu);
    return;
}
```

### 8. SSDT Hook 实现

在 `hyperkd` 新增 `ApiSsdt.c`，提供：

```c
NTSTATUS
ApiSsdtHook(UINT32 ServiceIndex, PVOID HookFunction, PVOID *OriginalFunction);

NTSTATUS
ApiSsdtUnhook(UINT32 ServiceIndex);

NTSTATUS
ApiSsdtQuery(UINT32 ServiceIndex, PVOID *CurrentFunction);
```

## 文件变更清单

### 删除
- `HyperDbg/hyperdbg/script-engine/` (整个目录)
- `HyperDbg/hyperdbg/script-eval/` (整个目录)
- `HyperDbg/hyperdbg/symbol-parser/` (整个目录)
- `HyperDbg/hyperdbg/libhyperdbg/` (整个目录)
- `HyperDbg/hyperdbg/hyperdbg-cli/` (整个目录)
- `HyperDbg/hyperdbg/hyperdbg-test/` (整个目录)
- `HyperDbg/hyperdbg/hyperkd/code/debugger/script-engine/` (整个目录)
- `HyperDbg/hyperdbg/hyperkd/header/debugger/script-engine/` (整个目录)

### 新增
- `HyperDbg/hyperdbg/hyperkd/code/debugger/api/ApiDispatch.c`
- `HyperDbg/hyperdbg/hyperkd/code/debugger/api/ApiCpuid.c`
- `HyperDbg/hyperdbg/hyperkd/code/debugger/api/ApiSsdt.c`
- `HyperDbg/hyperdbg/hyperkd/code/debugger/api/ApiEpt.c`
- `HyperDbg/hyperdbg/hyperkd/code/debugger/api/ApiMsr.c`
- `HyperDbg/hyperdbg/hyperkd/code/debugger/api/ApiIo.c`
- `HyperDbg/hyperdbg/hyperkd/code/debugger/api/ApiSyscall.c`
- `HyperDbg/hyperdbg/hyperkd/code/debugger/api/ApiMemory.c`
- `HyperDbg/hyperdbg/hyperkd/code/debugger/api/ApiProcess.c`
- `HyperDbg/hyperdbg/hyperkd/header/debugger/api/ApiDispatch.h`

### 修改
- `HyperDbg/hyperdbg/hyperhv/code/vmm/vmx/Hv.c` — 加 CPUID spoof 查表
- `HyperDbg/hyperdbg/hyperhv/code/interface/Dispatch.c` — 简化 CPUID dispatch
- `HyperDbg/hyperdbg/hyperkd/code/debugger/core/Debugger.c` — 移除脚本引擎引用
- `HyperDbg/hyperdbg/hyperkd/header/debugger/core/Debugger.h` — 移除脚本引擎声明
- `HyperDbg/hyperdbg/include/SDK/headers/Constants.h` — 新增 IOCTL 定义
- `CMakeLists.txt` — 移除已删除目录的构建目标

## 执行步骤

1. **创建 API 目录结构** — 创建 `api/` 目录和头文件
2. **实现 CPUID Spoof API** — 最简单，直接查表
3. **实现 ApiDispatch 主分发器** — IOCTL 入口
4. **实现 SSDT Hook API** — 核心功能
5. **实现其他 API** — EPT/MSR/IO/SYSCALL/Memory
6. **修改 IOCTL 入口** — 将现有 IOCTL handler 改为走 ApiDispatch
7. **删除冗余文件** — 脚本引擎、libhyperdbg 等
8. **更新 CMakeLists.txt** — 移除已删除的构建目标
9. **验证编译**
