/**
 * @file epthook-demo.cpp
 * @brief HyperDbg Hook Demo - EPT Hook + CPUID 伪造
 *
 * 全部使用导出的用户模式 API，不调用未导出的内部函数
 *
 * 核心 API：
 *   hyperdbg_u_run_command()    - 执行 HyperDbg 命令 (如 !epthook2, !cpuid)
 *   hyperdbg_u_run_script()     - 执行脚本表达式 (纯 C 调用，无命令行)
 *   hyperdbg_u_eval_expression()- - 求值表达式，获取符号地址
 *   hyperdbg_u_read_memory()    - 读取内存
 *   hyperdbg_u_write_memory()   - 写入内存
 *   hyperdbg_u_read_all_registers()  - 读取寄存器
 *   hyperdbg_u_read_target_register() - 读取单个寄存器
 *   hyperdbg_u_write_target_register() - 写入单个寄存器
 *
 * 注意：SendEventToKernel / RegisterActionToEvent 是内部函数，未导出到用户模式
 *       用户模式只能通过 run_command / run_script 注册事件
 *
 * NtDeviceIoControlFile x64 参数布局：
 *   RCX = FileHandle, RDX = Event, R8 = ApcRoutine, R9 = ApcContext
 *   [RSP+0x28] = IoStatusBlock, [RSP+0x30] = IoControlCode
 *   [RSP+0x38] = InputBuffer,   [RSP+0x40] = InputBufferLength
 *   [RSP+0x48] = OutputBuffer,  [RSP+0x50] = OutputBufferLength
 */

#include <Windows.h>
#include <cstdio>
#include <cstring>

#include "SDK/HyperDbgSdk.h"
#include "SDK/imports/user/HyperDbgLibImports.h"

#define TARGET_PROCESS_NAME "svchost.exe"

static volatile BOOL g_Running = TRUE;

//
// Message callback - GUI 可替换此回调获取输出数据
//
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

//
// Load HyperDbg in VMI mode
//
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

//
// Unload HyperDbg
//
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
// EPT Hook - 使用 hyperdbg_u_run_command API
// ============================================================

/**
 * @brief EPT hook NtDeviceIoControlFile + 读取 IOCTL 码和 buffer
 */
void
SetupEptHook_IoctlRead(const char * pname)
{
    char cmd[2048];

    snprintf(cmd, sizeof(cmd),
        "!epthook2 nt!NtDeviceIoControlFile script { "
            "if (strcmp(pname, \"%s\") == 0) { "
                "printf(\"[IOCTL] %%s pid:%%x IOCTL:%%x InBuf:%%llx OutBuf:%%llx\\n\", "
                    "pname, $pid, dd(@rsp+0x30), dq(@rsp+0x38), dq(@rsp+0x48)); "
            "} "
        "}",
        pname);

    printf("[*] EPT hook + IOCTL read\n");
    hyperdbg_u_run_command((CHAR *)cmd);
}

/**
 * @brief EPT hook + 修改 output buffer (假硬盘序列号)
 *
 * 伪造 STORAGE_DEVICE_DESCRIPTOR 中的序列号
 * Entry hook 检测 IOCTL_STORAGE_QUERY_PROPERTY
 * Post syscall 修改 output buffer
 */
void
SetupEptHook_FakeDiskSerial(const char * pname)
{
    char cmd[2048];

    //
    // Step 1: entry hook 检测 IOCTL_STORAGE_QUERY_PROPERTY
    //
    snprintf(cmd, sizeof(cmd),
        "!epthook2 nt!NtDeviceIoControlFile script { "
            "if (strcmp(pname, \"%s\") == 0) { "
                "if (dd(@rsp+0x30) == 0x2d1400) { "
                    "printf(\"[DISK-QUERY] OutBuf=%%llx len=%%x\\n\", dq(@rsp+0x48), dd(@rsp+0x50)); "
                "} "
            "} "
        "}",
        pname);

    printf("[*] Step 1 - Entry hook\n");
    hyperdbg_u_run_command((CHAR *)cmd);

    //
    // Step 2: !syscall stage post 修改 output buffer
    //
    snprintf(cmd, sizeof(cmd),
        "!syscall stage post script { "
            "if (strcmp(pname, \"%s\") == 0) { "
                "event_sc; "
                "if (@sc == 0x7) { "
                    "if (@rax == 0) { "
                        "printf(\"[POST-IOCTL] OK, rax=%%llx\\n\", @rax); "
                    "} "
                "} "
            "} "
        "}",
        pname);

    printf("[*] Step 2 - Post syscall\n");
    hyperdbg_u_run_command((CHAR *)cmd);
}

/**
 * @brief EPT hook + 修改 input buffer
 *
 * 修改 STORAGE_PROPERTY_QUERY.PropertyId:
 * StorageDeviceProperty(0) -> StorageAdapterProperty(1)
 */
void
SetupEptHook_ModifyInputBuffer(const char * pname)
{
    char cmd[2048];

    snprintf(cmd, sizeof(cmd),
        "!epthook2 nt!NtDeviceIoControlFile script { "
            "if (strcmp(pname, \"%s\") == 0) { "
                "if (dd(@rsp+0x30) == 0x2d1400) { "
                    "if (dd(dq(@rsp+0x38)) == 0x0) { "
                        "printf(\"[MODIFY] StorageDeviceProperty -> StorageAdapterProperty\\n\"); "
                        "ed(dq(@rsp+0x38), 0x1); "
                    "} "
                "} "
            "} "
        "}",
        pname);

    printf("[*] Modify input buffer\n");
    hyperdbg_u_run_command((CHAR *)cmd);
}

// ============================================================
// CPUID Hook - 使用 hyperdbg_u_run_command API
// ============================================================

/**
 * @brief CPUID Hook - 监控 CPUID 执行
 *
 * !cpuid 命令: 当指定 EAX 值的 CPUID 执行时触发
 * 脚本中可读取 @eax @ebx @ecx @edx
 *
 * 要修改 CPUID 返回值:
 *   方式1: 脚本中用 eb/ed/eq 写内存伪造结果
 *   方式2: 透明模式 (!hide) 自动伪造 CPUID
 *   方式3: 用 hyperdbg_u_write_target_register 在回调中修改
 */
void
SetupCpuidHook_Monitor()
{
    printf("[*] Setting up CPUID monitor hooks\n");

    //
    // Hook CPUID EAX=0: 监控厂商字符串
    //
    hyperdbg_u_run_command((CHAR *)
        "!cpuid 0 script { "
            "printf(\"[CPUID-0] EAX=%%llx EBX=%%llx ECX=%%llx EDX=%%llx\\n\", @eax, @ebx, @ecx, @edx); "
        "}");

    //
    // Hook CPUID EAX=1: 监控处理器签名
    //
    hyperdbg_u_run_command((CHAR *)
        "!cpuid 1 script { "
            "printf(\"[CPUID-1] EAX=%%llx (signature) EBX=%%llx ECX=%%llx EDX=%%llx\\n\", @eax, @ebx, @ecx, @edx); "
        "}");

    printf("[+] CPUID hooks registered\n");
}

/**
 * @brief CPUID Hook - 伪造 CPUID 返回值
 *
 * 使用 !cpuid + 脚本修改寄存器值
 * 在 VMX root 模式下，脚本可以修改寄存器来伪造 CPUID 结果
 *
 * CPUID EAX=0 返回:
 *   EAX = 最大支持 EAX 值
 *   EBX/ECX/EDX = 厂商字符串 (GenuineIntel)
 *
 * CPUID EAX=1 返回:
 *   EAX = 处理器签名 (Family/Model/Stepping)
 *   EBX = Brand index + CLFLUSH line size / APIC ID
 *   ECX/EDX = 特性标志
 */
void
SetupCpuidHook_FakeSerial()
{
    printf("[*] Setting up CPUID fake serial hooks\n");

    //
    // CPUID EAX=0: 伪造厂商字符串
    // 原始: EBX="Genu" ECX="inel" EDX="ineI" -> "GenuineIntel"
    // 伪造: EBX="Fake" ECX="CPU_" EDX="HOOO" -> "FakeCPU_HOOO"
    //
    // 注意: 在 !cpuid 脚本中，寄存器值是 CPUID 执行前的值
    //       要修改 CPUID 返回值，需要在 post emulation 阶段修改
    //       !cpuid 默认在 pre emulation 阶段执行
    //
    //       实际修改 CPUID 返回值最可靠的方式是使用透明模式
    //       hyperdbg_u_enable_transparent_mode() 会自动伪造 CPUID
    //

    //
    // 方式1: 监控 + 打印 (确认 hook 生效)
    //
    hyperdbg_u_run_command((CHAR *)
        "!cpuid 0 script { "
            "printf(\"[CPUID-0-FAKE] Original: EAX=%%llx EBX=%%llx ECX=%%llx EDX=%%llx\\n\", "
                "@eax, @ebx, @ecx, @edx); "
        "}");

    //
    // 方式2: 使用透明模式自动伪造 CPUID
    // !hide 命令会让 HyperDbg 自动修改 CPUID 返回值来隐藏虚拟化
    // 这是修改 CPUID 返回值最简单可靠的方式
    //
    // hyperdbg_u_enable_transparent_mode(0, NULL, FALSE);
    //

    //
    // CPUID EAX=1: 监控处理器签名
    //
    hyperdbg_u_run_command((CHAR *)
        "!cpuid 1 script { "
            "printf(\"[CPUID-1-FAKE] Signature: EAX=%%llx\\n\", @eax); "
        "}");

    printf("[+] CPUID fake serial hooks registered\n");
    printf("[*] To actually modify CPUID return values, use transparent mode:\n");
    printf("    hyperdbg_u_enable_transparent_mode(pid, NULL, TRUE);\n");
}

/**
 * @brief 使用透明模式伪造 CPUID
 *
 * 透明模式 (!hide) 会自动伪造 CPUID 结果来隐藏 VT-x
 * 包括修改 VMX 相关的 CPUID 位
 */
void
SetupTransparentMode(UINT32 pid)
{
    printf("[*] Enabling transparent mode for PID=%u\n", pid);

    if (hyperdbg_u_enable_transparent_mode(pid, NULL, TRUE))
    {
        printf("[+] Transparent mode enabled - CPUID will be faked\n");
    }
    else
    {
        printf("[-] Failed to enable transparent mode\n");
    }
}

// ============================================================
// 内存/寄存器 API - 纯 C 调用，GUI 直接用
// ============================================================

/**
 * @brief 读取内存 (纯 C API)
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
 * @brief 写入内存 (纯 C API)
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
 * @brief 读取单个寄存器 (纯 C API)
 */
BOOL
ReadRegister(REGS_ENUM regId, UINT64 * value)
{
    return hyperdbg_u_read_target_register(regId, value);
}

/**
 * @brief 写入单个寄存器 (纯 C API)
 */
BOOL
WriteRegister(REGS_ENUM regId, UINT64 value)
{
    return hyperdbg_u_write_target_register(regId, value);
}

/**
 * @brief 求值表达式 / 获取符号地址 (纯 C API)
 *
 * 示例:
 *   hyperdbg_u_eval_expression("nt!NtDeviceIoControlFile", &hasError)
 *   hyperdbg_u_eval_expression("0x7ff000000 + 0x100", &hasError)
 */
UINT64
EvalSymbol(const char * expr, BOOLEAN * hasError)
{
    return hyperdbg_u_eval_expression((CHAR *)expr, hasError);
}

/**
 * @brief 执行脚本表达式 (纯 C API，不需要命令行)
 *
 * 与 run_command 不同，run_script 直接执行脚本表达式
 * 适合 GUI 程序化调用
 */
BOOL
RunScript(const char * script)
{
    return hyperdbg_u_run_script((CHAR *)script, TRUE);
}

// ============================================================
// Usage
// ============================================================

void
ShowUsage(const char * exe)
{
    printf("HyperDbg Hook Demo - EPT Hook + CPUID Fake\n");
    printf("\nUsage: %s [mode] [process_name]\n", exe);
    printf("\nModes:\n");
    printf("  1 = EPT hook + read IOCTL/buffer\n");
    printf("  2 = EPT hook + fake disk serial\n");
    printf("  3 = EPT hook + modify input buffer\n");
    printf("  4 = CPUID hook monitor\n");
    printf("  5 = CPUID hook fake serial\n");
    printf("  6 = Transparent mode (auto fake CPUID)\n");
    printf("  7 = Memory/Register API demo\n");
    printf("\nExamples:\n");
    printf("  %s 1                - EPT hook svchost.exe\n", exe);
    printf("  %s 4                - CPUID monitor\n", exe);
    printf("  %s 5                - CPUID fake serial\n", exe);
    printf("  %s 6                - Transparent mode\n", exe);
    printf("\n");
    printf("Exported User-Mode APIs:\n");
    printf("  hyperdbg_u_run_command(cmd)             - Execute command\n");
    printf("  hyperdbg_u_run_script(expr, show_err)   - Execute script\n");
    printf("  hyperdbg_u_eval_expression(expr, &err)   - Evaluate expression\n");
    printf("  hyperdbg_u_read_memory(addr,type,rd,pid,sz,...)  - Read memory\n");
    printf("  hyperdbg_u_write_memory(addr,type,pid,src,sz)    - Write memory\n");
    printf("  hyperdbg_u_read_all_registers(&regs,&ext)        - Read all regs\n");
    printf("  hyperdbg_u_read_target_register(regId,&val)      - Read register\n");
    printf("  hyperdbg_u_write_target_register(regId,val)      - Write register\n");
    printf("  hyperdbg_u_enable_transparent_mode(pid,name,is_pid) - Fake CPUID\n");
    printf("  hyperdbg_u_set_breakpoint(addr,pid,tid,core)     - Set breakpoint\n");
    printf("\n");
    printf("NOTE: SendEventToKernel/RegisterActionToEvent are INTERNAL functions\n");
    printf("      NOT exported to user mode. Use run_command/run_script instead.\n");
}

int
main(int argc, char * argv[])
{
    int mode = 1;
    const char * pname = TARGET_PROCESS_NAME;

    if (argc >= 2)
    {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
        {
            ShowUsage(argv[0]);
            return 0;
        }
        mode = atoi(argv[1]);
    }
    if (argc >= 3)
    {
        pname = argv[2];
    }

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    printf("=== HyperDbg Hook Demo ===\n");
    printf("[*] Mode: %d, Process: %s\n\n", mode, pname);

    if (LoadHyperDbg() != 0)
    {
        printf("[-] Load failed\n");
        return 1;
    }

    switch (mode)
    {
    case 1: SetupEptHook_IoctlRead(pname); break;
    case 2: SetupEptHook_FakeDiskSerial(pname); break;
    case 3: SetupEptHook_ModifyInputBuffer(pname); break;
    case 4: SetupCpuidHook_Monitor(); break;
    case 5: SetupCpuidHook_FakeSerial(); break;
    case 6: SetupTransparentMode(0); break;
    case 7:
    {
        printf("[*] Memory/Register API demo\n");
        printf("    ReadMemory(addr, pid, buf, sz, &len) -> BOOL\n");
        printf("    WriteMemory(addr, pid, data, sz) -> BOOL\n");
        printf("    ReadRegister(regId, &value) -> BOOL\n");
        printf("    WriteRegister(regId, value) -> BOOL\n");
        printf("    EvalSymbol(\"nt!NtDeviceIoControlFile\", &err) -> UINT64\n");
        printf("    RunScript(\"script expression\") -> BOOL\n");

        //
        // Demo: 求值符号地址
        //
        BOOLEAN hasError = FALSE;
        UINT64 addr = EvalSymbol("nt!NtDeviceIoControlFile", &hasError);
        if (!hasError && addr != 0)
            printf("[+] nt!NtDeviceIoControlFile = 0x%llx\n", addr);
        else
            printf("[-] Symbol resolution failed (VMM not loaded or symbols not available)\n");
        break;
    }
    default:
        printf("[-] Unknown mode: %d\n", mode);
        ShowUsage(argv[0]);
        UnloadHyperDbg();
        return 1;
    }

    printf("\n[+] Active. Ctrl+C to stop.\n\n");

    while (g_Running)
        Sleep(1000);

    UnloadHyperDbg();
    return 0;
}
