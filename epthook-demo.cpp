/**
 * @file epthook-demo.cpp
 * @brief HyperDbg 一体化硬件伪造工具
 *
 * 通过 libhyperdbg API 实现：
 *   - 硬盘序列号无痕伪造（EPT hook NtDeviceIoControlFile + sysret post）
 *   - CPUID Leaf=1 四寄存器篡改
 *   - 网卡 MAC 地址伪造（EPT hook NtDeviceIoControlFile IOCTL_NDIS_QUERY_GLOBAL_STATS）
 *   - Windows 全主流随机数 API 劫持
 *   - Hook 前输出原始值，Hook 后输出伪造值（真实对比）
 */

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "SDK/HyperDbgSdk.h"
#include "SDK/imports/user/HyperDbgLibImports.h"

// ====================== 自定义配置区（自行修改） ======================
#define TARGET_PROCESS_NAME "target.exe"
#define FAKE_HDD_SN         "SPOOF-HDD-7721-AC69"

// CPUID Leaf=1 伪造四寄存器返回值
#define FAKE_CPUID_EAX 0x00000001
#define FAKE_CPUID_EBX 0x3927DF10
#define FAKE_CPUID_ECX 0x72BC5891
#define FAKE_CPUID_EDX 0x28FD614A

// 伪造 MAC 地址: AA:BB:CC:DD:EE:FF
#define FAKE_MAC_BYTE0 0xAA
#define FAKE_MAC_BYTE1 0xBB
#define FAKE_MAC_BYTE2 0xCC
#define FAKE_MAC_BYTE3 0xDD
#define FAKE_MAC_BYTE4 0xEE
#define FAKE_MAC_BYTE5 0xFF

// 自定义固定随机字节数组
static const BYTE g_FixedRndData[] = {
    0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
    0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x01
};
// =================================================================

static volatile BOOL g_ShouldExit = FALSE;

BOOL WINAPI
ConsoleCtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT)
    {
        g_ShouldExit = TRUE;
        printf("\n[*] Ctrl+C received, exiting...\n");
        fflush(stdout);
        return TRUE;
    }
    return FALSE;
}

int
hyperdbg_message_callback(const char * Text)
{
    printf("%s", Text);
    fflush(stdout);
    return 0;
}

static INT
RunCommand(const char * cmd)
{
    INT ret = hyperdbg_u_run_command((CHAR *)cmd);
    fflush(stdout);
    return ret;
}

// ============================================================
// Load / Unload HyperDbg
// ============================================================

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

    hyperdbg_u_set_text_message_callback((PVOID)hyperdbg_message_callback);

    printf("[*] Installing driver...\n");
    fflush(stdout);
    if (hyperdbg_u_install_kd_driver() == 1)
    {
        printf("[-] Install driver failed\n");
        return 1;
    }

    printf("[*] Loading VMM...\n");
    fflush(stdout);
    if (hyperdbg_u_load_vmm() == 1)
    {
        printf("[-] Load VMM failed\n");
        hyperdbg_u_uninstall_kd_driver();
        return 1;
    }

    printf("[+] HyperDbg loaded\n");
    fflush(stdout);
    return 0;
}

void
UnloadHyperDbg()
{
    printf("[*] Unloading...\n");
    fflush(stdout);
    hyperdbg_u_unload_vmm();
    hyperdbg_u_unload_kd();
    hyperdbg_u_stop_kd_driver();
    hyperdbg_u_uninstall_kd_driver();
    printf("[+] Unloaded\n");
    fflush(stdout);
}

// ============================================================
// 模块1：硬盘序列号 + 网卡MAC 无痕伪造
// EPT hook NtDeviceIoControlFile (pre) + sysret (post)
//
// 真实前后对比逻辑：
//   pre hook: 仅标记目标IOCTL，记录buffer指针
//   sysret post hook: 系统调用已返回，buffer中有原始数据
//     1) 先读取并打印原始值
//     2) 再覆盖伪造值
//     3) 打印伪造值
// ============================================================

void
SetupDiskAndMacHook()
{
    printf("[*] Setting up disk serial + MAC address spoof hook\n");

    //
    // Pre-hook: 标记目标IOCTL，记录buffer信息
    //   磁盘: IOCTL_STORAGE_QUERY_PROPERTY 0x2D1400 / SMART 0x7C088
    //   网卡: IOCTL_NDIS_QUERY_GLOBAL_STATS 0x17002
    //         OID 0x01010101 (OID_802_3_PERMANENT_ADDRESS)
    //         OID 0x01010102 (OID_802_3_CURRENT_ADDRESS)
    //
    RunCommand(
        "!epthook nt!NtDeviceIoControlFile stage pre script { "
            "u64 ioctl_code = qword(@rsp + 0x28); "

            "if (ioctl_code == 0x2D1400 || ioctl_code == 0x7C088) { "
                ".g_disk_output_ptr = ptr(qword(@rsp + 0x40)); "
                ".g_disk_output_size = dword(@rsp + 0x48); "
                ".g_disk_hook_flag = 1; "
            "} "

            "if (ioctl_code == 0x17002) { "
                "void* inbuf = ptr(qword(@rsp + 0x30)); "
                "u32 inbuf_len = dword(@rsp + 0x38); "
                "if (inbuf_len >= 4) { "
                    "u32 oid = dword(inbuf); "
                    "if (oid == 0x01010101 || oid == 0x01010102) { "
                        ".g_mac_output_ptr = ptr(qword(@rsp + 0x40)); "
                        ".g_mac_output_size = dword(@rsp + 0x48); "
                        ".g_mac_hook_flag = 1; "
                    "} "
                "} "
            "} "
        "}");

    //
    // Post-hook (sysret): 系统调用已返回，buffer中有原始数据
    // 先打印原始值，再覆盖，再打印伪造值
    //
    RunCommand(
        "!sysret stage post script { "
            // ---- 磁盘序列号 ----
            "if (.g_disk_hook_flag == 1 && .g_disk_output_ptr != 0 && .g_disk_output_size > 0) { "
                "if (strstr($procname, \"" TARGET_PROCESS_NAME "\") != 0) { "
                    // 1) 打印原始SN（系统调用已填充buffer）
                    "printf(\"[BEFORE-DISK] %s Original SN: \", $procname); "
                    "prints(.g_disk_output_ptr, .g_disk_output_size); "
                    "printf(\"\\n\"); "
                    // 2) 覆盖伪造SN
                    "u32 copy_len = min(sizeof(.fake_hdd_sn), .g_disk_output_size); "
                    "memcpy(.g_disk_output_ptr, .fake_hdd_sn, copy_len); "
                    // 3) 打印伪造SN
                    "printf(\"[AFTER-DISK]  %s Spoofed SN: \", $procname); "
                    "prints(.g_disk_output_ptr, copy_len); "
                    "printf(\"\\n\"); "
                "} "
                ".g_disk_hook_flag = 0; "
                ".g_disk_output_ptr = 0; "
                ".g_disk_output_size = 0; "
            "} "

            // ---- 网卡MAC ----
            "if (.g_mac_hook_flag == 1 && .g_mac_output_ptr != 0 && .g_mac_output_size >= 6) { "
                "if (strstr($procname, \"" TARGET_PROCESS_NAME "\") != 0) { "
                    // 1) 打印原始MAC
                    "printf(\"[BEFORE-MAC]  %s Original MAC: %%02x:%%02x:%%02x:%%02x:%%02x:%%02x\\n\", $procname, "
                        "byte(.g_mac_output_ptr), byte(.g_mac_output_ptr+1), byte(.g_mac_output_ptr+2), "
                        "byte(.g_mac_output_ptr+3), byte(.g_mac_output_ptr+4), byte(.g_mac_output_ptr+5)); "
                    // 2) 覆盖伪造MAC
                    "byte(.g_mac_output_ptr)   = 0xAA; "
                    "byte(.g_mac_output_ptr+1) = 0xBB; "
                    "byte(.g_mac_output_ptr+2) = 0xCC; "
                    "byte(.g_mac_output_ptr+3) = 0xDD; "
                    "byte(.g_mac_output_ptr+4) = 0xEE; "
                    "byte(.g_mac_output_ptr+5) = 0xFF; "
                    // 3) 打印伪造MAC
                    "printf(\"[AFTER-MAC]   %s Spoofed MAC: %%02x:%%02x:%%02x:%%02x:%%02x:%%02x\\n\", $procname, "
                        "byte(.g_mac_output_ptr), byte(.g_mac_output_ptr+1), byte(.g_mac_output_ptr+2), "
                        "byte(.g_mac_output_ptr+3), byte(.g_mac_output_ptr+4), byte(.g_mac_output_ptr+5)); "
                "} "
                ".g_mac_hook_flag = 0; "
                ".g_mac_output_ptr = 0; "
                ".g_mac_output_size = 0; "
            "} "
        "}");

    printf("[+] Disk serial + MAC address hook registered\n");
}

// ============================================================
// 模块2：CPUID 伪造
// !cpuid 事件触发时寄存器已有CPU计算结果
// 先打印原始值，再覆盖，再打印伪造值
// ============================================================

void
SetupCpuidHook()
{
    printf("[*] Setting up CPUID fake hook\n");

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "!cpuid script { "
            "if (strstr($procname, \"" TARGET_PROCESS_NAME "\") == 0) return; "
            "if ($cpuid_leaf == 1) { "
                // 1) 打印原始CPUID
                "printf(\"[BEFORE-CPUID] %%s Leaf=1 Original: EAX=0x%%llx RBX=0x%%llx RCX=0x%%llx RDX=0x%%llx\\n\", $procname, @rax, @rbx, @rcx, @rdx); "
                // 2) 覆盖伪造值
                "@rax = 0x%08X; "
                "@rbx = 0x%08X; "
                "@rcx = 0x%08X; "
                "@rdx = 0x%08X; "
                // 3) 打印伪造值
                "printf(\"[AFTER-CPUID]  %%s Leaf=1 Spoofed: EAX=0x%%08x RBX=0x%%08x RCX=0x%%08x RDX=0x%%08x\\n\", $procname, @rax, @rbx, @rcx, @rdx); "
            "} "
        "}",
        FAKE_CPUID_EAX, FAKE_CPUID_EBX, FAKE_CPUID_ECX, FAKE_CPUID_EDX);

    RunCommand(cmd);
    printf("[+] CPUID hook registered\n");
}

// ============================================================
// 模块3：Windows 全主流随机数 API 劫持
// stage post: 函数已执行，buffer中有原始随机数据
// 先打印原始数据，再覆盖，再打印伪造数据
// ============================================================

void
SetupRandomHooks()
{
    printf("[*] Setting up random API hooks\n");

    // 1. CryptGenRandom - stage post时buffer已有原始随机数据
    RunCommand(
        "!epthook advapi32!CryptGenRandom stage post script { "
            "if (strstr($procname, \"" TARGET_PROCESS_NAME "\") == 0) return; "
            "u32 req_len = dword(@rdx); "
            "void* buf_ptr = ptr(@r8); "
            // 1) 打印原始随机数据
            "printf(\"[BEFORE-CryptGenRandom] %%s len=%%d data=\", $procname, req_len); "
            "prints(buf_ptr, min(req_len, 16)); "
            "printf(\"\\n\"); "
            // 2) 覆盖伪造数据
            "u32 copy_len = min(req_len, sizeof(.fixed_rnd_data)); "
            "memcpy(buf_ptr, .fixed_rnd_data, copy_len); "
            // 3) 打印伪造数据
            "printf(\"[AFTER-CryptGenRandom]  %%s len=%%d data=\", $procname, req_len); "
            "prints(buf_ptr, min(req_len, 16)); "
            "printf(\"\\n\"); "
        "}");

    // 2. BCryptGenRandom
    RunCommand(
        "!epthook bcrypt!BCryptGenRandom stage post script { "
            "if (strstr($procname, \"" TARGET_PROCESS_NAME "\") == 0) return; "
            "void* buf_ptr = ptr(@rdx); "
            "u32 req_len = dword(@r8); "
            "printf(\"[BEFORE-BCryptGenRandom] %%s len=%%d data=\", $procname, req_len); "
            "prints(buf_ptr, min(req_len, 16)); "
            "printf(\"\\n\"); "
            "u32 copy_len = min(req_len, sizeof(.fixed_rnd_data)); "
            "memcpy(buf_ptr, .fixed_rnd_data, copy_len); "
            "printf(\"[AFTER-BCryptGenRandom]  %%s len=%%d data=\", $procname, req_len); "
            "prints(buf_ptr, min(req_len, 16)); "
            "printf(\"\\n\"); "
        "}");

    // 3. RtlGenRandom
    RunCommand(
        "!epthook advapi32!RtlGenRandom stage post script { "
            "if (strstr($procname, \"" TARGET_PROCESS_NAME "\") == 0) return; "
            "void* buf_ptr = ptr(@rcx); "
            "u32 req_len = dword(@rdx); "
            "printf(\"[BEFORE-RtlGenRandom] %%s len=%%d data=\", $procname, req_len); "
            "prints(buf_ptr, min(req_len, 16)); "
            "printf(\"\\n\"); "
            "u32 copy_len = min(req_len, sizeof(.fixed_rnd_data)); "
            "memcpy(buf_ptr, .fixed_rnd_data, copy_len); "
            "printf(\"[AFTER-RtlGenRandom]  %%s len=%%d data=\", $procname, req_len); "
            "prints(buf_ptr, min(req_len, 16)); "
            "printf(\"\\n\"); "
        "}");

    // 4. CRT rand() - stage post时@rax已有原始返回值
    RunCommand(
        "!epthook msvcrt!rand stage post script { "
            "if (strstr($procname, \"" TARGET_PROCESS_NAME "\") != 0) { "
                // 1) 打印原始返回值
                "printf(\"[BEFORE-rand] %%s Original: %%d\\n\", $procname, @rax); "
                // 2) 覆盖伪造值
                "@rax = 0x88776655; "
                // 3) 打印伪造值
                "printf(\"[AFTER-rand]  %%s Spoofed: 0x88776655\\n\", $procname); "
            "} "
        "}");

    printf("[+] Random API hooks registered\n");
}

// ============================================================
// 一键初始化 / 一键卸载
// ============================================================

void
InitAll()
{
    printf("\n[INIT] Loading VMM + connecting local\n");
    if (LoadHyperDbg() != 0)
    {
        printf("[-] Init failed\n");
        return;
    }

    RunCommand(".connect local");
    RunCommand("printf(\"[INIT] Environment ready\\n\");");
    printf("[INIT] Target filter: %s\n\n", TARGET_PROCESS_NAME);
}

void
SetupAllHooks()
{
    printf("\n=============================================\n");
    printf("  Setting up all hardware spoof hooks\n");
    printf("=============================================\n\n");

    // 全局变量初始化
    RunCommand(".g_disk_hook_flag = 0");
    RunCommand(".g_disk_output_ptr = 0");
    RunCommand(".g_disk_output_size = 0");
    RunCommand(".g_mac_hook_flag = 0");
    RunCommand(".g_mac_output_ptr = 0");
    RunCommand(".g_mac_output_size = 0");
    RunCommand(".fake_hdd_sn = \"" FAKE_HDD_SN "\"");
    RunCommand(".fixed_rnd_data = { 0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80, 0x90,0xA0,0xB0,0xC0,0xD0,0xE0,0xF0,0x01 }");

    SetupDiskAndMacHook();
    SetupCpuidHook();
    SetupRandomHooks();

    printf("\n=============================================\n");
    printf("  All hooks registered\n");
    printf("  Fake HDD SN : %s\n", FAKE_HDD_SN);
    printf("  Fake MAC    : %02X:%02X:%02X:%02X:%02X:%02X\n",
        FAKE_MAC_BYTE0, FAKE_MAC_BYTE1, FAKE_MAC_BYTE2,
        FAKE_MAC_BYTE3, FAKE_MAC_BYTE4, FAKE_MAC_BYTE5);
    printf("  Fake CPUID  : EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X\n",
        FAKE_CPUID_EAX, FAKE_CPUID_EBX, FAKE_CPUID_ECX, FAKE_CPUID_EDX);
    printf("=============================================\n\n");
}

void
UnloadAll()
{
    printf("\n[UNLOAD] Clearing all hooks\n");

    RunCommand("!epthook off nt!NtDeviceIoControlFile");
    RunCommand("!cpuid off");
    RunCommand("!epthook off advapi32!CryptGenRandom");
    RunCommand("!epthook off bcrypt!BCryptGenRandom");
    RunCommand("!epthook off advapi32!RtlGenRandom");
    RunCommand("!epthook off msvcrt!rand");

    printf("[UNLOAD] Disconnecting\n");
    RunCommand(".disconnect");

    UnloadHyperDbg();

    printf("[UNLOAD] All resources cleaned\n\n");
}

// ============================================================
// 进程查找
// ============================================================

#include <TlHelp32.h>

UINT32
FindProcessByName(const char * name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32 pe = {0};
    pe.dwSize = sizeof(pe);

    if (Process32First(snap, &pe))
    {
        do
        {
            if (_stricmp(pe.szExeFile, name) == 0)
            {
                CloseHandle(snap);
                return pe.th32ProcessID;
            }
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
    return 0;
}

// ============================================================
// 内存/寄存器 API
// ============================================================

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

UINT64
EvalSymbol(const char * expr, BOOLEAN * hasError)
{
    return hyperdbg_u_eval_expression((CHAR *)expr, hasError);
}

// ============================================================
// 交互式命令循环
// ============================================================

void
ShowHelp()
{
    printf("\n=== HyperDbg Hardware Spoof Tool ===\n");
    printf("  init          - Load VMM + connect local\n");
    printf("  setup         - Setup all spoof hooks\n");
    printf("  disk          - Setup disk serial + MAC hook\n");
    printf("  cpuid         - Setup CPUID hook only\n");
    printf("  random        - Setup random API hooks only\n");
    printf("  unload        - Clear hooks + disconnect + unload\n");
    printf("  events        - Show active events\n");
    printf("  clear         - Clear all events\n");
    printf("  sym <expr>    - Evaluate symbol/expression\n");
    printf("  pid <name>    - Find process PID\n");
    printf("  <cmd>         - Any HyperDbg command\n");
    printf("  quit / exit   - Unload and exit\n");
    printf("  help          - Show this help\n");
    printf("\n  Target : %s\n", TARGET_PROCESS_NAME);
    printf("  Fake SN : %s\n", FAKE_HDD_SN);
    printf("  Fake MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        FAKE_MAC_BYTE0, FAKE_MAC_BYTE1, FAKE_MAC_BYTE2,
        FAKE_MAC_BYTE3, FAKE_MAC_BYTE4, FAKE_MAC_BYTE5);
    printf("\n");
}

void
RunInteractiveLoop()
{
    char line[4096];

    printf("[+] Interactive mode. Type 'help' for commands, 'quit' to exit.\n\n");

    while (TRUE)
    {
        if (g_ShouldExit)
            break;

        printf("spoof> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
        {
            printf("\n");
            break;
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0)
            continue;

        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0)
        {
            break;
        }
        else if (strcmp(line, "help") == 0)
        {
            ShowHelp();
            continue;
        }
        else if (strcmp(line, "init") == 0)
        {
            InitAll();
            continue;
        }
        else if (strcmp(line, "setup") == 0)
        {
            SetupAllHooks();
            continue;
        }
        else if (strcmp(line, "disk") == 0)
        {
            SetupDiskAndMacHook();
            continue;
        }
        else if (strcmp(line, "cpuid") == 0)
        {
            SetupCpuidHook();
            continue;
        }
        else if (strcmp(line, "random") == 0)
        {
            SetupRandomHooks();
            continue;
        }
        else if (strcmp(line, "unload") == 0)
        {
            UnloadAll();
            continue;
        }
        else if (strcmp(line, "events") == 0)
        {
            RunCommand("events");
            continue;
        }
        else if (strcmp(line, "clear") == 0)
        {
            RunCommand("events all clear");
            printf("[+] All events cleared\n");
            continue;
        }
        else if (strncmp(line, "sym ", 4) == 0)
        {
            char * expr = line + 4;
            while (*expr == ' ') expr++;
            if (*expr != '\0')
            {
                BOOLEAN hasError = FALSE;
                UINT64  addr    = EvalSymbol(expr, &hasError);
                if (!hasError)
                    printf("[+] %s = 0x%llx\n", expr, addr);
                else
                    printf("[-] Failed to evaluate: %s\n", expr);
            }
            continue;
        }
        else if (strncmp(line, "pid ", 4) == 0)
        {
            char * name = line + 4;
            while (*name == ' ') name++;
            UINT32 pid = FindProcessByName(name);
            if (pid != 0)
                printf("[+] %s pid = %u (0x%x)\n", name, pid, pid);
            else
                printf("[-] Process not found: %s\n", name);
            continue;
        }

        INT ret = RunCommand(line);
        if (ret == 1)
            break;
    }
}

void
ShowUsage(const char * exe)
{
    printf("HyperDbg Hardware Spoof Tool\n");
    printf("\nUsage: %s [mode]\n", exe);
    printf("\nModes:\n");
    printf("  (no args)  = Interactive mode\n");
    printf("  1          = Auto: init + setup all hooks\n");
    printf("  2          = Auto: init + disk serial + MAC hook\n");
    printf("  3          = Auto: init + CPUID hook\n");
    printf("  4          = Auto: init + random API hooks\n");
    printf("\n");
}

int
main(int argc, char * argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    printf("=== HyperDbg Hardware Spoof Tool ===\n\n");
    fflush(stdout);

    if (argc >= 2)
    {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
        {
            ShowUsage(argv[0]);
            return 0;
        }
    }

    if (LoadHyperDbg() != 0)
    {
        printf("[-] Load failed\n");
        return 1;
    }

    RunCommand(".connect local");

    if (argc < 2)
    {
        RunInteractiveLoop();
    }
    else
    {
        int mode = atoi(argv[1]);

        switch (mode)
        {
        case 1:
            SetupAllHooks();
            break;
        case 2:
            SetupDiskAndMacHook();
            break;
        case 3:
            SetupCpuidHook();
            break;
        case 4:
            SetupRandomHooks();
            break;
        default:
            printf("[-] Unknown mode: %d\n", mode);
            ShowUsage(argv[0]);
            break;
        }

        printf("\n[+] Hooks active. Press Enter to unload...\n");
        getchar();
    }

    UnloadAll();
    return 0;
}
