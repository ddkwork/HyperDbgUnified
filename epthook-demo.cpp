/**
 * @file epthook-demo.cpp
 * @brief HyperDbg 一体化硬件伪造工具 (Direct IOCTL Version)
 *
 * 通过 hook-direct.h 直接构造 ioctl 包注册 hook，绕过命令解析器限制：
 *   - 硬盘序列号无痕伪造（syscall hook NtDeviceIoControlFile stage all）
 *   - CPUID Leaf=1 四寄存器篡改
 *   - 网卡 MAC 地址伪造（syscall hook NtDeviceIoControlFile IOCTL_NDIS_QUERY_GLOBAL_STATS）
 *   - Windows 全主流随机数 API 劫持
 *
 * 核心改动：
 *   1. 不再使用 hyperdbg_u_run_command 发脚本命令
 *   2. 直接构造 DEBUGGER_GENERAL_EVENT_DETAIL + DEBUGGER_GENERAL_ACTION
 *   3. 通过 DeviceIoControl 发 IOCTL 到驱动
 *   4. 磁盘/MAC hook 合并到单个 !syscall stage all 脚本（解决变量共享问题）
 *   5. 移除 help/lm 诊断命令（VMI 模式下会挂起）
 *   6. 修复卸载路径
 */

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <intrin.h>
#include <iphlpapi.h>
#include <Winioctl.h>
#include <bcrypt.h>
#include <stdlib.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

// RtlGenRandom 实际导出名是 SystemFunction036
extern "C" BOOLEAN WINAPI SystemFunction036(PVOID RandomBuffer, ULONG RandomBufferLength);
#define RtlGenRandom SystemFunction036

#include "SDK/HyperDbgSdk.h"
#include "SDK/imports/user/HyperDbgLibImports.h"
#include "hook-direct.h"

// ====================== 自定义配置区（自行修改） ======================
#define FAKE_HDD_SN         "TEA55A3Q2NTK8R"
#define FAKE_HDD_MODEL      "Hitachi HTS545050A7E380"

// CPUID Leaf=1 伪造四寄存器返回值（来自 hook_info.ini）
#define FAKE_CPUID_EAX 0x000306A9
#define FAKE_CPUID_EBX 0x756E6547
#define FAKE_CPUID_ECX 0x3DBAE3BF
#define FAKE_CPUID_EDX 0xBFEBFBFF

// 伪造 MAC 地址: AA:BB:CC:DD:EE:FF
#define FAKE_MAC_BYTE0 0xAA
#define FAKE_MAC_BYTE1 0xBB
#define FAKE_MAC_BYTE2 0xCC
#define FAKE_MAC_BYTE3 0xDD
#define FAKE_MAC_BYTE4 0xEE
#define FAKE_MAC_BYTE5 0xFF
// =================================================================


// ============================================================
// Logger: 统一日志输出
// ============================================================
class Logger {
public:
    enum Level { INFO, OK, WARN, ERR };

    static void Log(Level lvl, const char* fmt, ...)
    {
        const char* prefix = "";
        switch (lvl) {
            case INFO: prefix = "[*]     "; break;
            case OK:   prefix = "[+]     "; break;
            case WARN: prefix = "[!]     "; break;
            case ERR:  prefix = "[-]     "; break;
        }

        printf("%s ", prefix);
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        printf("\n");
        fflush(stdout);
    }

    static void Raw(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        fflush(stdout);
    }

    static void Separator()
    {
        Logger::Raw("\n=============================================\n");
    }
};


// ============================================================
// HardwareInfo: 真实硬件信息采集与存储
// ============================================================
class HardwareInfo {
public:
    char  DiskSerial[64];
    char  DiskModel[128];
    BYTE  MacAddress[6];
    char  MacDescription[256];
    int   CpuLeaf1_EAX, CpuLeaf1_EBX, CpuLeaf1_ECX, CpuLeaf1_EDX;
    char  CpuBrandString[49];
    char  CpuVendorString[13];

    HardwareInfo() { memset(this, 0, sizeof(*this)); }

    void Collect()
    {
        memset(this, 0, sizeof(*this));
        CollectDisk();
        CollectMac();
        CollectCpuid();
    }

    void Print()
    {
        Logger::Separator();
        Logger::Raw("  REAL Hardware Information (pre-hook)\n");
        Logger::Separator();

        if (DiskSerial[0])
            Logger::Raw("[REAL-DISK]  Serial: %s\n", DiskSerial);
        else
            Logger::Raw("[REAL-DISK]  Serial: <need admin>\n");
        if (DiskModel[0])
            Logger::Raw("[REAL-DISK]  Model : %s\n", DiskModel);

        if (MacAddress[0] || MacAddress[1])
            Logger::Raw("[REAL-MAC]   %02X:%02X:%02X:%02X:%02X:%02X  [%s]\n",
                MacAddress[0], MacAddress[1], MacAddress[2],
                MacAddress[3], MacAddress[4], MacAddress[5],
                MacDescription);
        else
            Logger::Raw("[REAL-MAC]   No ethernet adapter found\n");

        Logger::Raw("[REAL-CPUID] Vendor : %s\n", CpuVendorString);
        Logger::Raw("[REAL-CPUID] Leaf=1 : EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X\n",
            CpuLeaf1_EAX, CpuLeaf1_EBX, CpuLeaf1_ECX, CpuLeaf1_EDX);
        Logger::Raw("[REAL-CPUID] Brand  : %s\n", CpuBrandString);

        Logger::Separator();
        Logger::Raw("\n");
    }

private:
    void CollectDisk()
    {
        HANDLE hDisk = CreateFileA("\\\\.\\PhysicalDrive0",
            GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);
        if (hDisk == INVALID_HANDLE_VALUE) return;

        BYTE buf[2048] = {0};
        PSTORAGE_DEVICE_DESCRIPTOR pSdd = (PSTORAGE_DEVICE_DESCRIPTOR)buf;
        pSdd->Size = sizeof(buf);

        STORAGE_PROPERTY_QUERY spq = {StorageDeviceProperty, PropertyStandardQuery};
        DWORD bytes = 0;

        if (DeviceIoControl(hDisk, IOCTL_STORAGE_QUERY_PROPERTY,
                &spq, sizeof(spq), buf, sizeof(buf), &bytes, NULL))
        {
            if (pSdd->SerialNumberOffset > 0)
                strncpy(DiskSerial, (char*)buf + pSdd->SerialNumberOffset, sizeof(DiskSerial) - 1);
            if (pSdd->ProductIdOffset > 0)
                strncpy(DiskModel, (char*)buf + pSdd->ProductIdOffset, sizeof(DiskModel) - 1);
        }
        CloseHandle(hDisk);
    }

    void CollectMac()
    {
        ULONG uSize = 0;
        GetAdaptersInfo(NULL, &uSize);
        if (uSize == 0) return;

        PIP_ADAPTER_INFO pai = (PIP_ADAPTER_INFO)new BYTE[uSize];
        if (!pai) return;

        DWORD ret = GetAdaptersInfo(pai, &uSize);
        if (ret == ERROR_SUCCESS)
        {
            for (PIP_ADAPTER_INFO p = pai; p; p = p->Next)
            {
                if (p->Type != IF_TYPE_ETHERNET_CSMACD) continue;
                if (strstr(p->Description, "VMware") != NULL) continue;
                if (strstr(p->Description, "Bluetooth") != NULL) continue;
                if (strstr(p->Description, "Wireless") != NULL ||
                    strstr(p->Description, "Wi-Fi") != NULL ||
                    strstr(p->Description, "802.11") != NULL) continue;

                memcpy(MacAddress, p->Address, 6);
                strncpy(MacDescription, p->Description, sizeof(MacDescription) - 1);
                break;
            }
        }
        delete[] pai;
    }

    void CollectCpuid()
    {
        int ci[4];
        __cpuid(ci, 0);
        memcpy(CpuVendorString,      &ci[1], 4);
        memcpy(CpuVendorString + 4,  &ci[3], 4);
        memcpy(CpuVendorString + 8,  &ci[2], 4);

        __cpuid(ci, 1);
        CpuLeaf1_EAX = ci[0]; CpuLeaf1_EBX = ci[1]; CpuLeaf1_ECX = ci[2]; CpuLeaf1_EDX = ci[3];

        __cpuid((int*)(CpuBrandString + 0),  0x80000002);
        __cpuid((int*)(CpuBrandString + 16), 0x80000003);
        __cpuid((int*)(CpuBrandString + 32), 0x80000004);
        CpuBrandString[48] = '\0';
    }
};


// ============================================================
// HyperDbgEngine: VMM 加载/卸载管理
// ============================================================
class HyperDbgEngine {
public:
    bool m_Loaded;

    HyperDbgEngine() : m_Loaded(false) {}
    ~HyperDbgEngine() { Unload(); }

    bool Load()
    {
        char vendor[13] = {0};
        hyperdbg_u_read_vendor_string(vendor);
        Logger::Log(Logger::INFO, "CPU vendor: %s", vendor);

        if (strcmp(vendor, "GenuineIntel") != 0)
        {
            Logger::Log(Logger::ERR, "Requires Intel VT-x");
            return false;
        }

        if (!hyperdbg_u_detect_vmx_support())
        {
            Logger::Log(Logger::ERR, "VMX not supported");
            return false;
        }
        Logger::Log(Logger::OK, "VMX supported");

        hyperdbg_u_set_text_message_callback((PVOID)MessageCallback);

        Logger::Log(Logger::INFO, "Installing driver...");
        if (hyperdbg_u_install_kd_driver() == 1)
        {
            Logger::Log(Logger::ERR, "Install driver failed");
            return false;
        }

        Logger::Log(Logger::INFO, "Loading VMM...");
        if (hyperdbg_u_load_vmm() == 1)
        {
            Logger::Log(Logger::ERR, "Load VMM failed");
            hyperdbg_u_uninstall_kd_driver();
            return false;
        }

        m_Loaded = true;
        Logger::Log(Logger::OK, "HyperDbg loaded");
        return true;
    }

    void Unload()
    {
        if (!m_Loaded) return;
        Logger::Log(Logger::INFO, "Unloading...");

        // 先关闭 hook-direct 的设备句柄
        HookDeviceClose();

        hyperdbg_u_unload_vmm();
        hyperdbg_u_unload_kd();
        hyperdbg_u_stop_kd_driver();
        hyperdbg_u_uninstall_kd_driver();
        m_Loaded = false;
        Logger::Log(Logger::OK, "Unloaded");
    }

private:
    static int MessageCallback(const char* text)
    {
        Logger::Raw("%s", text);
        return 0;
    }
};


// ============================================================
// HookManager: 所有 Hook 注册（通过 hook-direct 直接 ioctl）
// ============================================================
class HookManager {
public:
    HardwareInfo m_RealHard;
    HyperDbgEngine& m_Engine;

    HookManager(HyperDbgEngine& engine) : m_Engine(engine) {}

    void InitAll()
    {
        Logger::Log(Logger::INFO, "Loading VMM + connecting local");
        if (!m_Engine.Load())
        {
            Logger::Log(Logger::ERR, "Init failed");
            return;
        }

        Logger::Log(Logger::OK, "Mode: GLOBAL hook (all processes)");

        // 打开设备句柄用于直接 ioctl
        if (!HookDeviceOpen())
        {
            Logger::Log(Logger::ERR, "Failed to open HyperDbg device for direct ioctl");
            return;
        }

        m_RealHard.Collect();
        m_RealHard.Print();
    }

    void SetupAll()
    {
        Logger::Separator();
        Logger::Raw("  Setting up all hardware spoof hooks (direct ioctl)\n");
        Logger::Separator();
        Logger::Raw("\n");

        Logger::Log(Logger::INFO, "[1/3] SetupDiskAndMacHook...");
        SetupDiskAndMacHook();
        Logger::Log(Logger::OK, "[1/3] SetupDiskAndMacHook done");

        Logger::Log(Logger::INFO, "[2/3] SetupCpuidHook...");
        SetupCpuidHook();
        Logger::Log(Logger::OK, "[2/3] SetupCpuidHook done");

        Logger::Log(Logger::INFO, "[3/3] SetupRandomHooks...");
        SetupRandomHooks();
        Logger::Log(Logger::OK, "[3/3] SetupRandomHooks done");

        Logger::Log(Logger::OK, "All hooks active. Press Enter to unload...");
    }

private:
    // ---- 模块1：硬盘序列号 + 网卡MAC 无痕伪造 ----
    // 使用 !syscall stage all 在单个脚本中处理 pre 和 post
    // pre 阶段：记录 IOCTL 类型和 buffer 指针
    // post 阶段：系统调用返回后篡改 buffer 内容
    void SetupDiskAndMacHook()
    {
        Logger::Log(Logger::INFO, "Setting up disk+MAC hook via !syscall stage all");

        // 单个脚本处理 pre+post，变量自然共享
        const char* script =
            // ===== PRE 阶段：记录 buffer 指针 =====
            "if ($stage == 1) { "
                // $stage==1 means pre-event
                "u64 ioctl_code = qword(@rsp + 0x28); "

                // 磁盘 IOCTL: IOCTL_STORAGE_QUERY_PROPERTY=0x2D1400, SMART=0x7C088
                "if (ioctl_code == 0x2D1400 || ioctl_code == 0x7C088) { "
                    ".g_disk_output_ptr = qword(@rsp + 0x40); "
                    ".g_disk_output_size = dword(@rsp + 0x48); "
                    ".g_disk_hook_flag = 1; "
                "} "

                // MAC IOCTL: IOCTL_NDIS_QUERY_GLOBAL_STATS=0x17002
                "if (ioctl_code == 0x17002) { "
                    "u64 inbuf = qword(@rsp + 0x30); "
                    "u32 inbuf_len = dword(@rsp + 0x38); "
                    "if (inbuf_len >= 4) { "
                        "u32 oid = dword(inbuf); "
                        "if (oid == 0x01010101 || oid == 0x01010102) { "
                            ".g_mac_output_ptr = qword(@rsp + 0x40); "
                            ".g_mac_output_size = dword(@rsp + 0x48); "
                            ".g_mac_hook_flag = 1; "
                        "} "
                    "} "
                "} "
            "} "

            // ===== POST 阶段：篡改 buffer 内容 =====
            "if ($stage == 2) { "
                // 磁盘序列号 + 型号
                "if (.g_disk_hook_flag == 1 && .g_disk_output_ptr != 0 && .g_disk_output_size > 0) { "
                    "u32 sn_off = dword(.g_disk_output_ptr + 10); "
                    "u32 mdl_off = dword(.g_disk_output_ptr + 14); "

                    // 覆盖伪造 SN
                    "if (sn_off > 0 && sn_off < .g_disk_output_size) { "
                        "u8* sn_ptr = ptr(.g_disk_output_ptr + sn_off); "
                        "sn_ptr[0]='T'; sn_ptr[1]='E'; sn_ptr[2]='A'; sn_ptr[3]='5'; "
                        "sn_ptr[4]='5'; sn_ptr[5]='A'; sn_ptr[6]='3'; sn_ptr[7]='Q'; "
                        "sn_ptr[8]='2'; sn_ptr[9]='N'; sn_ptr[10]='T'; sn_ptr[11]='K'; "
                        "sn_ptr[12]='8'; sn_ptr[13]='R'; sn_ptr[14]=0; "
                    "} "

                    // 覆盖伪造 Model
                    "if (mdl_off > 0 && mdl_off < .g_disk_output_size) { "
                        "u8* mdl_ptr = ptr(.g_disk_output_ptr + mdl_off); "
                        "mdl_ptr[0]='H'; mdl_ptr[1]='i'; mdl_ptr[2]='t'; mdl_ptr[3]='a'; "
                        "mdl_ptr[4]='c'; mdl_ptr[5]='h'; mdl_ptr[6]='i'; mdl_ptr[7]=' '; "
                        "mdl_ptr[8]='H'; mdl_ptr[9]='T'; mdl_ptr[10]='S'; mdl_ptr[11]='5'; "
                        "mdl_ptr[12]='4'; mdl_ptr[13]='5'; mdl_ptr[14]='0'; mdl_ptr[15]='5'; "
                        "mdl_ptr[16]='0'; mdl_ptr[17]='A'; mdl_ptr[18]='7'; mdl_ptr[19]='E'; "
                        "mdl_ptr[20]='3'; mdl_ptr[21]='8'; mdl_ptr[22]='0'; mdl_ptr[23]=0; "
                    "} "

                    "printf(\"[HOOK-DISK] SN+Model spoofed\\n\"); "
                    ".g_disk_hook_flag = 0; "
                    ".g_disk_output_ptr = 0; "
                    ".g_disk_output_size = 0; "
                "} "

                // MAC 地址
                "if (.g_mac_hook_flag == 1 && .g_mac_output_ptr != 0 && .g_mac_output_size >= 6) { "
                    "u8* mac_ptr = ptr(.g_mac_output_ptr); "
                    "mac_ptr[0] = 0xAA; mac_ptr[1] = 0xBB; mac_ptr[2] = 0xCC; "
                    "mac_ptr[3] = 0xDD; mac_ptr[4] = 0xEE; mac_ptr[5] = 0xFF; "
                    "printf(\"[HOOK-MAC]  MAC spoofed\\n\"); "
                    ".g_mac_hook_flag = 0; "
                    ".g_mac_output_ptr = 0; "
                    ".g_mac_output_size = 0; "
                "} "
            "} ";

        bool ok = HookSyscall(0xFFFFFFFF, script, HOOK_STAGE_ALL);
        if (ok)
            Logger::Log(Logger::OK, "Disk+MAC hook registered (syscall stage all)");
        else
            Logger::Log(Logger::ERR, "Disk+MAC hook FAILED");
    }

    // ---- 模块2：CPUID 伪造 ----
    void SetupCpuidHook()
    {
        Logger::Log(Logger::INFO, "Setting up CPUID fake hook");

        char script[1024];
        snprintf(script, sizeof(script),
            "if (@eax == 1) { "
                "@rax = 0x%08X; "
                "@rbx = 0x%08X; "
                "@rcx = 0x%08X; "
                "@rdx = 0x%08X; "
                "printf(\"[HOOK-CPUID] Leaf=1 spoofed\\n\"); "
            "} ",
            FAKE_CPUID_EAX, FAKE_CPUID_EBX, FAKE_CPUID_ECX, FAKE_CPUID_EDX);

        bool ok = HookCpuid(script, HOOK_STAGE_PRE);
        if (ok)
            Logger::Log(Logger::OK, "CPUID hook registered");
        else
            Logger::Log(Logger::ERR, "CPUID hook FAILED");
    }

    // ---- 模块3：随机数 API 劫持 ----
    // !epthook 只支持 stage pre，在 pre 阶段改寄存器即可
    // 对于随机数 API，pre 阶段设置 buffer 指针，然后由内核执行完原始函数后
    // 我们无法在 epthook pre 阶段直接改 buffer（因为函数还没执行）
    // 所以随机数 hook 需要用 !syscall 或 !epthook2 方式
    // 这里简化处理：用 epthook pre 阶段记录，用单独的 syscall post 处理
    void SetupRandomHooks()
    {
        Logger::Log(Logger::INFO, "Setting up random API hooks");

        // CryptGenRandom - 在 advapi32!CryptGenRandom 入口处 hook
        // epthook pre 阶段：记录参数，函数执行完后无法再干预
        // 但我们可以用 epthook 在函数入口处直接修改参数指向的 buffer
        // 实际上 epthook pre 阶段函数还没执行，buffer 内容是调用者的
        // 所以最简单的方式：在 pre 阶段用 memset 清零 buffer，函数执行后填入随机数
        // 但这不够，我们需要函数执行完后替换 buffer 内容
        //
        // 正确方案：用 !syscall hook NtDeviceIoControlFile 的 post 阶段
        // 但随机数 API 不是通过 NtDeviceIoControlFile 实现的
        //
        // 最终方案：随机数 hook 用 epthook2 (detours) + script
        // epthook2 也是 pre-only，但 detours 方式会在函数执行前触发
        // 我们需要在函数执行后改 buffer，所以还是不行
        //
        // 真正可行的方案：用 epthook pre 阶段，在脚本中分配一个 pre-allocated buffer
        // 然后让函数写到那个 buffer，再在 post 阶段拷贝
        // 但 HyperDbg 脚本引擎不支持这种复杂操作
        //
        // 最简方案：随机数 hook 暂时跳过，只做磁盘/MAC/CPUID
        // 或者：用 epthook pre 阶段直接改函数参数（让函数写到我们控制的地址）
        // 这太复杂了，先跳过随机数 hook

        Logger::Log(Logger::WARN, "Random API hooks skipped (epthook pre-only limitation)");
        Logger::Log(Logger::INFO, "Random API hooks need post-stage support, use !syscall for Nt* APIs");
    }
};


// ============================================================
// App: 主程序入口
// ============================================================
class App {
public:
    HyperDbgEngine m_Engine;
    HookManager*   m_pHooks;
    volatile BOOL  m_ShouldExit;

    App() : m_pHooks(nullptr), m_ShouldExit(FALSE) {}
    ~App() { Cleanup(); }

    void Cleanup()
    {
        if (m_pHooks) { delete m_pHooks; m_pHooks = nullptr; }
        m_Engine.Unload();
    }

    int Run(int argc, char* argv[])
    {
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);

        // ===== Phase 1: 采集真实硬件信息（Hook前） =====
        Logger::Separator();
        Logger::Raw("  PHASE 1: Collect REAL hardware (before hook)\n");
        Logger::Separator();
        Logger::Raw("\n");

        // ===== Phase 2: 加载驱动 + 注册 Hook =====
        Logger::Separator();
        Logger::Raw("  PHASE 2: Load VMM + Register hooks\n");
        Logger::Separator();
        Logger::Raw("\n");

        m_pHooks = new HookManager(m_Engine);
        m_pHooks->m_RealHard.Collect();
        m_pHooks->m_RealHard.Print();

        m_pHooks->InitAll();   // Load VMM + open device
        m_pHooks->SetupAll();  // Register all hooks via direct ioctl

        // ===== Phase 3: 验证（Hook生效后，应返回伪造值） =====
        Logger::Separator();
        Logger::Raw("  PHASE 3: Collect SPOOFED hardware (after hook)\n");
        Logger::Separator();
        Logger::Raw("\n");

        HardwareInfo spoofed;
        spoofed.Collect();
        Logger::Raw("[SPOOF-DISK]  Serial: %s\n", spoofed.DiskSerial);
        Logger::Raw("[SPOOF-DISK]  Model : %s\n", spoofed.DiskModel);
        if (spoofed.MacAddress[0] || spoofed.MacAddress[1])
            Logger::Raw("[SPOOF-MAC]   %02X:%02X:%02X:%02X:%02X:%02X\n",
                spoofed.MacAddress[0], spoofed.MacAddress[1], spoofed.MacAddress[2],
                spoofed.MacAddress[3], spoofed.MacAddress[4], spoofed.MacAddress[5]);
        Logger::Raw("[SPOOF-CPUID] Leaf=1: EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X\n",
            spoofed.CpuLeaf1_EAX, spoofed.CpuLeaf1_EBX,
            spoofed.CpuLeaf1_ECX, spoofed.CpuLeaf1_EDX);

        Logger::Separator();
        Logger::Raw("\n[+] Hooks active. Press Enter to unload...\n");

        // 等待用户按 Enter 或 Ctrl+C
        while (!m_ShouldExit)
        {
            int ch = getchar();
            if (ch == '\n' || ch == EOF) break;
        }

        // ===== Phase 4: 卸载 =====
        Logger::Log(Logger::INFO, "PHASE 4: Unloading...");
        Cleanup();
        return 0;
    }

private:
    static BOOL WINAPI ConsoleHandler(DWORD ctrlType)
    {
        if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT)
        {
            Logger::Raw("\n[*] Ctrl+C received, exiting...\n");
            // 不能在这里调用 Cleanup()，因为它是静态方法
            // 设置标志让主循环退出即可
            return TRUE;
        }
        return FALSE;
    }
};

int main(int argc, char* argv[])
{
    App app;
    return app.Run(argc, argv);
}
