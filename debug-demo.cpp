/**
 * @file debug-demo.cpp
 * @brief HyperDbg 调试 Demo - VMI 本地模式调试记事本
 *
 * 全部使用 C API，不调用 hyperdbg_u_run_command
 * 所有数据通过结构体获取，方便 GUI 直接使用
 *
 * 核心 API：
 *   hyperdbg_u_connect_local_debugger()     - 连接本地调试器
 *   hyperdbg_u_start_process()              - 启动被调试进程 (OEP暂停)
 *   hyperdbg_u_continue_debuggee()          - 继续执行
 *   hyperdbg_u_pause_debuggee()             - 暂停执行
 *   hyperdbg_u_stepping_regular_step_in()   - 单步进入
 *   hyperdbg_u_stepping_step_over()         - 单步越过
 *   hyperdbg_u_read_all_registers()         - 读取所有寄存器 -> GUEST_REGS + GUEST_EXTRA_REGISTERS
 *   hyperdbg_u_read_memory()                - 读取内存 -> BYTE[] (堆栈/代码等)
 *   hyperdbg_u_write_memory()               - 写入内存
 *   hyperdbg_u_set_breakpoint()             - 设置断点
 *   hyperdbg_u_read_target_register()       - 读取单个寄存器
 *   hyperdbg_u_write_target_register()      - 写入单个寄存器
 *
 * 堆栈遍历：
 *   HyperDbg 没有导出 callstack API (KdSendCallStackPacketToDebuggee 是内部函数)
 *   所以用 hyperdbg_u_read_memory 读 RSP 处内存，手动遍历栈帧
 *   x64 调用约定: RBP chain walking 或 RSP+offset 扫描返回地址
 */

#include <Windows.h>
#include <cstdio>
#include <cstring>

#include "SDK/HyperDbgSdk.h"
#include "SDK/imports/user/HyperDbgLibImports.h"

//
// 单步执行次数
//
#define STEP_COUNT 10

//
// 每次读取堆栈的字节数
//
#define STACK_READ_SIZE 256

//
// 调用栈最大深度
//
#define MAX_CALLSTACK_DEPTH 32

static volatile BOOL g_Running = TRUE;

// ============================================================
// 数据结构 - GUI 可以直接使用这些结构体
// ============================================================

/**
 * @brief 寄存器快照 - 包含所有寄存器值
 */
struct RegisterSnapshot
{
    GUEST_REGS          GpRegs;     // 通用寄存器
    GUEST_EXTRA_REGISTERS ExtraRegs; // 特殊寄存器 (RIP, RFLAGS, 段寄存器)
    BOOL                Valid;      // 数据是否有效
};

/**
 * @brief 堆栈条目 - 栈上的一个 8 字节值
 */
struct StackEntry
{
    UINT64  Address;    // 栈地址 (RSP + offset)
    UINT64  Value;      // 栈上的值
    BOOL    Valid;      // 数据是否有效
};

/**
 * @brief 堆栈快照 - 完整的栈内容
 */
struct StackSnapshot
{
    StackEntry  Entries[STACK_READ_SIZE / 8];
    UINT32      Count;      // 有效条目数
    UINT64      RspBase;    // 栈基址 (RSP)
    BOOL        Valid;
};

/**
 * @brief 调用栈帧 - 一个栈帧
 */
struct CallStackFrame
{
    UINT64  ReturnAddress;   // 返回地址
    UINT64  StackPointer;    // 栈指针
    UINT64  FramePointer;    // 帧指针 (RBP)
    BOOL    Valid;
};

/**
 * @brief 调用栈快照
 */
struct CallStackSnapshot
{
    CallStackFrame  Frames[MAX_CALLSTACK_DEPTH];
    UINT32          FrameCount;
    BOOL            Valid;
};

/**
 * @brief 单步结果 - 每步执行后的完整状态
 */
struct StepResult
{
    int                 StepNumber;
    RegisterSnapshot    Registers;
    StackSnapshot       Stack;
    CallStackSnapshot   CallStack;
};

// ============================================================
// API 封装 - 返回结构体数据，GUI 直接用
// ============================================================

/**
 * @brief 读取所有寄存器 -> RegisterSnapshot
 */
RegisterSnapshot
GetRegisterSnapshot()
{
    RegisterSnapshot snap = {0};
    snap.Valid = hyperdbg_u_read_all_registers(&snap.GpRegs, &snap.ExtraRegs);
    return snap;
}

/**
 * @brief 读取堆栈内存 -> StackSnapshot
 *
 * 从 RSP 开始读取 STACK_READ_SIZE 字节
 */
StackSnapshot
GetStackSnapshot()
{
    StackSnapshot snap = {0};

    RegisterSnapshot regs = GetRegisterSnapshot();
    if (!regs.Valid)
        return snap;

    snap.RspBase = regs.GpRegs.rsp;

    BYTE buf[STACK_READ_SIZE] = {0};
    UINT32 returnLen = 0;
    DEBUGGER_READ_MEMORY_ADDRESS_MODE addrMode = DEBUGGER_READ_ADDRESS_MODE_64_BIT;

    snap.Valid = hyperdbg_u_read_memory(
        snap.RspBase,
        DEBUGGER_READ_VIRTUAL_ADDRESS,
        READ_FROM_KERNEL,
        0,  // current process
        STACK_READ_SIZE,
        TRUE,
        &addrMode,
        buf,
        &returnLen);

    if (!snap.Valid)
        return snap;

    snap.Count = returnLen / 8;
    for (UINT32 i = 0; i < snap.Count && i < STACK_READ_SIZE / 8; i++)
    {
        snap.Entries[i].Address = snap.RspBase + i * 8;
        snap.Entries[i].Value   = *(UINT64 *)(buf + i * 8);
        snap.Entries[i].Valid   = TRUE;
    }

    return snap;
}

/**
 * @brief 读取指定地址的内存
 *
 * GUI 可用此函数读取任意地址的内存
 */
BOOL
ReadMemory(UINT64 address, UINT32 pid, BYTE * buffer, UINT32 size, UINT32 * bytesRead)
{
    DEBUGGER_READ_MEMORY_ADDRESS_MODE addrMode = DEBUGGER_READ_ADDRESS_MODE_64_BIT;
    return hyperdbg_u_read_memory(
        address,
        DEBUGGER_READ_VIRTUAL_ADDRESS,
        READ_FROM_KERNEL,
        pid,
        size,
        TRUE,
        &addrMode,
        buffer,
        bytesRead);
}

/**
 * @brief 写入内存
 */
BOOL
WriteMemory(UINT64 address, UINT32 pid, BYTE * data, UINT32 size)
{
    return hyperdbg_u_write_memory(
        (PVOID)address,
        EDIT_VIRTUAL_MEMORY,
        pid,
        data,
        size);
}

/**
 * @brief 读取单个寄存器
 */
BOOL
ReadRegister(REGS_ENUM regId, UINT64 * value)
{
    return hyperdbg_u_read_target_register(regId, value);
}

/**
 * @brief 写入单个寄存器
 */
BOOL
WriteRegister(REGS_ENUM regId, UINT64 value)
{
    return hyperdbg_u_write_target_register(regId, value);
}

/**
 * @brief 遍历调用栈 (RBP chain walking)
 *
 * x64 帧指针链遍历:
 *   当前 RBP -> 指向上一层的 RBP
 *   [RBP+0x08] = 返回地址
 *   [RBP+0x00] = 上一层的 RBP
 *
 * 注意: 如果编译器优化掉了 RBP (frame pointer omission)，此方法不适用
 *       需要用调试信息 (PDB) 辅助遍历
 */
CallStackSnapshot
GetCallStack()
{
    CallStackSnapshot snap = {0};

    RegisterSnapshot regs = GetRegisterSnapshot();
    if (!regs.Valid)
        return snap;

    //
    // Frame 0: 当前 RIP + RSP
    //
    snap.Frames[0].ReturnAddress = regs.ExtraRegs.RIP;
    snap.Frames[0].StackPointer  = regs.GpRegs.rsp;
    snap.Frames[0].FramePointer  = regs.GpRegs.rbp;
    snap.Frames[0].Valid         = TRUE;
    snap.FrameCount              = 1;

    //
    // RBP chain walking
    //
    UINT64 currentRbp = regs.GpRegs.rbp;

    for (UINT32 i = 1; i < MAX_CALLSTACK_DEPTH; i++)
    {
        if (currentRbp == 0)
            break;

        BYTE frameData[16] = {0}; // 读取 [RBP] 和 [RBP+8]
        UINT32 bytesRead = 0;

        if (!ReadMemory(currentRbp, 0, frameData, 16, &bytesRead) || bytesRead < 16)
            break;

        UINT64 parentRbp   = *(UINT64 *)(frameData + 0);  // [RBP+0x00] = parent RBP
        UINT64 returnAddr  = *(UINT64 *)(frameData + 8);   // [RBP+0x08] = return address

        //
        // 基本合法性检查
        //
        if (returnAddr == 0)
            break;

        // RBP 应该是栈地址 (高位 0xFFFF 或 0x0000)
        if (parentRbp != 0 && (parentRbp & 0xFFFF000000000000ULL) != 0xFFFF000000000000ULL &&
            (parentRbp & 0xFFFF000000000000ULL) != 0x0000000000000000ULL)
            break;

        // RBP 应该递增 (栈向下增长)
        if (parentRbp != 0 && parentRbp <= currentRbp)
            break;

        snap.Frames[i].ReturnAddress = returnAddr;
        snap.Frames[i].StackPointer  = currentRbp + 16; // 上一层的 RSP
        snap.Frames[i].FramePointer  = parentRbp;
        snap.Frames[i].Valid         = TRUE;
        snap.FrameCount++;

        currentRbp = parentRbp;
    }

    snap.Valid = TRUE;
    return snap;
}

/**
 * @brief 执行单步并获取完整状态
 */
StepResult
StepAndGetState(int stepNumber)
{
    StepResult result = {0};
    result.StepNumber = stepNumber;

    if (!hyperdbg_u_stepping_regular_step_in())
        return result;

    Sleep(500);

    result.Registers = GetRegisterSnapshot();
    result.Stack     = GetStackSnapshot();
    result.CallStack = GetCallStack();

    return result;
}

// ============================================================
// 打印函数 - 控制台输出 (GUI 不需要这些，直接用结构体)
// ============================================================

void
PrintRegisterSnapshot(const RegisterSnapshot * snap)
{
    if (!snap->Valid)
    {
        printf("  [registers unavailable]\n");
        return;
    }

    const GUEST_REGS &r = snap->GpRegs;
    const GUEST_EXTRA_REGISTERS &e = snap->ExtraRegs;

    printf("  RAX=%016llx  RBX=%016llx  RCX=%016llx\n", r.rax, r.rbx, r.rcx);
    printf("  RDX=%016llx  RSI=%016llx  RDI=%016llx\n", r.rdx, r.rsi, r.rdi);
    printf("  RBP=%016llx  RSP=%016llx\n", r.rbp, r.rsp);
    printf("  R8 =%016llx  R9 =%016llx  R10=%016llx\n", r.r8, r.r9, r.r10);
    printf("  R11=%016llx  R12=%016llx  R13=%016llx\n", r.r11, r.r12, r.r13);
    printf("  R14=%016llx  R15=%016llx\n", r.r14, r.r15);
    printf("  RIP=%016llx  RFLAGS=%016llx\n", e.RIP, e.RFLAGS);
    printf("  CS=%04x  DS=%04x  ES=%04x  FS=%04x  GS=%04x  SS=%04x\n",
           e.CS, e.DS, e.ES, e.FS, e.GS, e.SS);
}

void
PrintStackSnapshot(const StackSnapshot * snap)
{
    if (!snap->Valid)
    {
        printf("  [stack unavailable]\n");
        return;
    }

    printf("  Stack (RSP=%016llx, %u entries):\n", snap->RspBase, snap->Count);
    for (UINT32 i = 0; i < snap->Count; i++)
    {
        printf("    [RSP+0x%03x] = %016llx\n",
               (UINT32)(snap->Entries[i].Address - snap->RspBase),
               snap->Entries[i].Value);
    }
}

void
PrintCallStackSnapshot(const CallStackSnapshot * snap)
{
    if (!snap->Valid)
    {
        printf("  [callstack unavailable]\n");
        return;
    }

    printf("  Callstack (%u frames):\n", snap->FrameCount);
    for (UINT32 i = 0; i < snap->FrameCount; i++)
    {
        const CallStackFrame &f = snap->Frames[i];
        if (!f.Valid)
            continue;

        if (i == 0)
            printf("    #%-2u RIP=%016llx  RSP=%016llx  RBP=%016llx\n",
                   i, f.ReturnAddress, f.StackPointer, f.FramePointer);
        else
            printf("    #%-2u RetAddr=%016llx  RSP=%016llx  RBP=%016llx\n",
                   i, f.ReturnAddress, f.StackPointer, f.FramePointer);
    }
}

void
PrintStepResult(const StepResult * result)
{
    printf("\n--- Step #%d ---\n", result->StepNumber);
    PrintRegisterSnapshot(&result->Registers);
    PrintStackSnapshot(&result->Stack);
    PrintCallStackSnapshot(&result->CallStack);
}

// ============================================================
// Console handler / Load / Unload
// ============================================================

int
hyperdbg_message_callback(const char * Text)
{
    printf("%s", Text);
    return 0;
}

BOOL WINAPI
ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT)
    {
        printf("\n[*] Shutting down...\n");
        g_Running = FALSE;
        return TRUE;
    }
    return FALSE;
}

int
LoadHyperDbg()
{
    char CpuId[13] = {0};
    hyperdbg_u_read_vendor_string(CpuId);
    printf("[*] CPU vendor: %s\n", CpuId);

    if (strcmp(CpuId, "GenuineIntel") != 0)
    {
        printf("[-] Requires Intel VT-x\n");
        return 1;
    }

    if (!hyperdbg_u_detect_vmx_support())
    {
        printf("[-] VMX not supported\n");
        return 1;
    }
    printf("[+] VMX supported\n");

    hyperdbg_u_set_text_message_callback(hyperdbg_message_callback);

    printf("[*] Installing driver...\n");
    if (hyperdbg_u_install_kd_driver() == 1)
    {
        printf("[-] Install driver failed\n");
        return 1;
    }

    printf("[*] Loading VMM...\n");
    if (hyperdbg_u_load_vmm() == 1)
    {
        printf("[-] Load VMM failed\n");
        hyperdbg_u_uninstall_kd_driver();
        return 1;
    }

    printf("[+] HyperDbg loaded\n");
    return 0;
}

void
UnloadHyperDbg()
{
    printf("[*] Unloading...\n");
    hyperdbg_u_run_command((CHAR *)"events all clear");
    hyperdbg_u_unload_vmm();
    hyperdbg_u_unload_kd();
    hyperdbg_u_stop_kd_driver();
    hyperdbg_u_uninstall_kd_driver();
    printf("[+] Unloaded\n");
}

// ============================================================
// 主调试流程
// ============================================================

void
DebugNotepad()
{
    printf("\n========================================\n");
    printf("[*] Starting notepad.exe debug session\n");
    printf("========================================\n\n");

    //
    // Step 1: 连接本地调试器
    //
    printf("[*] Connecting to local debugger...\n");
    hyperdbg_u_connect_local_debugger();
    Sleep(1000);

    //
    // Step 2: 启动 notepad.exe (OEP 暂停)
    //
    printf("[*] Starting notepad.exe (will pause at OEP)...\n");
    hyperdbg_u_start_process(L"C:\\Windows\\system32\\notepad.exe");
    Sleep(3000);

    printf("\n========================================\n");
    printf("[+] Notepad paused at OEP!\n");
    printf("========================================\n\n");

    //
    // Step 3: 获取 OEP 处状态 (全部用 API，不用 run_command)
    //
    RegisterSnapshot oepRegs = GetRegisterSnapshot();
    StackSnapshot    oepStack = GetStackSnapshot();
    CallStackSnapshot oepCallstack = GetCallStack();

    printf("--- OEP State ---\n");
    PrintRegisterSnapshot(&oepRegs);
    PrintStackSnapshot(&oepStack);
    PrintCallStackSnapshot(&oepCallstack);

    //
    // Step 4: 单步执行，每步获取完整状态
    //
    for (int i = 1; i <= STEP_COUNT && g_Running; i++)
    {
        StepResult result = StepAndGetState(i);
        PrintStepResult(&result);
    }

    //
    // Step 5: 继续执行
    //
    printf("\n[*] Continuing execution...\n");
    hyperdbg_u_continue_debuggee();
    printf("[+] Debug session complete. Notepad is running.\n");
}

/**
 * @brief 交互模式 - 全部用 API，不用 run_command
 */
void
InteractiveMode()
{
    printf("\n[*] Interactive mode (API only, no run_command)\n");
    printf("Commands: regs | stack | callstack | step | stepover | go | bp <addr> | read <addr> | quit\n\n");

    char input[1024];

    while (g_Running)
    {
        printf("hyperdbg> ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin))
            break;

        input[strcspn(input, "\n")] = '\0';
        if (strlen(input) == 0)
            continue;

        if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0)
            break;

        if (strcmp(input, "regs") == 0)
        {
            RegisterSnapshot snap = GetRegisterSnapshot();
            PrintRegisterSnapshot(&snap);
            continue;
        }

        if (strcmp(input, "stack") == 0)
        {
            StackSnapshot snap = GetStackSnapshot();
            PrintStackSnapshot(&snap);
            continue;
        }

        if (strcmp(input, "callstack") == 0 || strcmp(input, "k") == 0)
        {
            CallStackSnapshot snap = GetCallStack();
            PrintCallStackSnapshot(&snap);
            continue;
        }

        if (strcmp(input, "step") == 0 || strcmp(input, "t") == 0)
        {
            StepResult result = StepAndGetState(0);
            PrintStepResult(&result);
            continue;
        }

        if (strcmp(input, "stepover") == 0 || strcmp(input, "p") == 0)
        {
            hyperdbg_u_stepping_step_over();
            Sleep(500);
            RegisterSnapshot snap = GetRegisterSnapshot();
            PrintRegisterSnapshot(&snap);
            continue;
        }

        if (strcmp(input, "go") == 0 || strcmp(input, "g") == 0)
        {
            hyperdbg_u_continue_debuggee();
            continue;
        }

        if (strncmp(input, "bp ", 3) == 0)
        {
            UINT64 addr = 0;
            if (sscanf(input + 3, "%llx", &addr) == 1)
            {
                if (hyperdbg_u_set_breakpoint(addr, 0, 0, 0))
                    printf("[+] Breakpoint set at 0x%llx\n", addr);
                else
                    printf("[-] Set breakpoint failed\n");
            }
            else
            {
                printf("Usage: bp <hex_address>\n");
            }
            continue;
        }

        if (strncmp(input, "read ", 5) == 0)
        {
            UINT64 addr = 0;
            if (sscanf(input + 5, "%llx", &addr) == 1)
            {
                BYTE buf[64] = {0};
                UINT32 bytesRead = 0;
                if (ReadMemory(addr, 0, buf, 64, &bytesRead))
                {
                    printf("[+] Read %u bytes from 0x%llx:\n", bytesRead, addr);
                    for (UINT32 i = 0; i < bytesRead; i += 16)
                    {
                        printf("  %016llx: ", addr + i);
                        for (UINT32 j = 0; j < 16 && (i + j) < bytesRead; j++)
                            printf("%02x ", buf[i + j]);
                        printf("\n");
                    }
                }
                else
                    printf("[-] Read failed\n");
            }
            else
            {
                printf("Usage: read <hex_address>\n");
            }
            continue;
        }

        printf("Unknown command: %s\n", input);
    }
}

void
ShowUsage(const char * exe)
{
    printf("HyperDbg Debug Demo - VMI Local Mode (API Only)\n");
    printf("\nUsage: %s [mode]\n", exe);
    printf("\nModes:\n");
    printf("  auto        - Auto debug notepad: start, OEP, step N times\n");
    printf("  interactive - Interactive mode: manual API commands\n");
    printf("\nAuto mode steps: %d\n", STEP_COUNT);
    printf("\nInteractive commands:\n");
    printf("  regs              - Get registers (API: read_all_registers)\n");
    printf("  stack             - Get stack (API: read_memory from RSP)\n");
    printf("  callstack / k     - Get callstack (API: RBP chain walking)\n");
    printf("  step / t          - Step into (API: stepping_regular_step_in)\n");
    printf("  stepover / p      - Step over (API: stepping_step_over)\n");
    printf("  go / g            - Continue (API: continue_debuggee)\n");
    printf("  bp <addr>         - Set breakpoint (API: set_breakpoint)\n");
    printf("  read <addr>       - Read memory (API: read_memory)\n");
    printf("  quit              - Exit\n");
    printf("\n");
    printf("Data Structures for GUI:\n");
    printf("  RegisterSnapshot  - { GpRegs, ExtraRegs, Valid }\n");
    printf("  StackSnapshot     - { Entries[], Count, RspBase, Valid }\n");
    printf("  CallStackSnapshot - { Frames[], FrameCount, Valid }\n");
    printf("  StepResult        - { StepNumber, Registers, Stack, CallStack }\n");
    printf("\n");
    printf("Key APIs:\n");
    printf("  GetRegisterSnapshot()  -> RegisterSnapshot\n");
    printf("  GetStackSnapshot()     -> StackSnapshot\n");
    printf("  GetCallStack()         -> CallStackSnapshot\n");
    printf("  StepAndGetState(n)     -> StepResult\n");
    printf("  ReadMemory(addr,pid,buf,sz,&len) -> BOOL\n");
    printf("  WriteMemory(addr,pid,data,sz)    -> BOOL\n");
    printf("  ReadRegister(regId,&value)        -> BOOL\n");
    printf("  WriteRegister(regId,value)        -> BOOL\n");
}

int
main(int argc, char * argv[])
{
    const char * mode = "auto";

    if (argc >= 2)
    {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
        {
            ShowUsage(argv[0]);
            return 0;
        }
        mode = argv[1];
    }

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    printf("=== HyperDbg Debug Demo (API Only) ===\n");
    printf("[*] Mode: %s\n\n", mode);

    if (LoadHyperDbg() != 0)
    {
        printf("[-] Load failed\n");
        return 1;
    }

    if (strcmp(mode, "auto") == 0)
    {
        DebugNotepad();
    }
    else if (strcmp(mode, "interactive") == 0 || strcmp(mode, "i") == 0)
    {
        printf("[*] Connecting to local debugger...\n");
        hyperdbg_u_connect_local_debugger();
        Sleep(1000);
        InteractiveMode();
    }
    else
    {
        printf("[-] Unknown mode: %s\n", mode);
        ShowUsage(argv[0]);
    }

    UnloadHyperDbg();
    return 0;
}
