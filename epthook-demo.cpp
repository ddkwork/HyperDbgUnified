/**
 * @file epthook-demo.cpp
 * @brief HyperDbg 一体化硬件伪造工具 (C++ Class Design)
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

// ====================== 自定义配置区（自行修改） ======================
#define TARGET_PROCESS_NAME "SuperRecovery 7.0.exe"
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

// 自定义固定随机字节数组
static const BYTE g_FixedRndData[] = {
    0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
    0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x01
};
// =================================================================


// ============================================================
// Logger: 统一日志输出
// ============================================================
class Logger {
public:
    enum Level { INFO, OK, WARN, ERR, HOOK_BEFORE, HOOK_AFTER };

    static void Log(Level lvl, const char* fmt, ...)
    {
        const char* prefix = "";
        switch (lvl) {
            case HOOK_BEFORE: prefix = "[BEFORE]"; break;
            case HOOK_AFTER:  prefix = "[AFTER] "; break;
            case INFO:        prefix = "[*]     "; break;
            case OK:          prefix = "[+]     "; break;
            case WARN:        prefix = "[!]     "; break;
            case ERR:         prefix = "[-]     "; break;
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
#pragma pack(push, 1)
class HardwareInfo {
public:
    // Disk
    char  DiskSerial[64];
    char  DiskModel[128];
    // MAC
    BYTE  MacAddress[6];
    char  MacDescription[256];
    char  MacAdapterName[260];
    // CPUID
    int   CpuLeaf0_EBX, CpuLeaf0_ECX, CpuLeaf0_EDX;
    int   CpuLeaf1_EAX, CpuLeaf1_EBX, CpuLeaf1_ECX, CpuLeaf1_EDX;
    int   CpuLeaf3_ECX, CpuLeaf3_EDX;
    char  CpuBrandString[49];
    char  CpuVendorString[13];
    // Random APIs
    BYTE  RndCryptGen[16];
    BYTE  RndBCryptGen[16];
    BYTE  RndRtlGen[16];
    int   RndRandVal;

    HardwareInfo() { Clear(); }
    void Clear() { memset(this, 0, sizeof(*this)); }

    void Collect()
    {
        Clear();
        CollectDisk();
        CollectMac();
        CollectCpuid();
        CollectRandom();
    }

    void Print()
    {
        Logger::Separator();
        Logger::Raw("  REAL Hardware Information (pre-hook)\n");
        Logger::Separator();

        PrintDisk();
        PrintMac();
        PrintCpuid();
        PrintRandom();

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
                strncpy(MacAdapterName, p->AdapterName, sizeof(MacAdapterName) - 1);
                break;
            }
        }
        delete[] pai;
    }

    void CollectCpuid()
    {
        int ci[4];

        __cpuid(ci, 0);
        memcpy(CpuVendorString,      &ci[1], 4); // EBX
        memcpy(CpuVendorString + 4,  &ci[3], 4); // EDX
        memcpy(CpuVendorString + 8,  &ci[2], 4); // ECX
        CpuLeaf0_EBX = ci[1]; CpuLeaf0_ECX = ci[2]; CpuLeaf0_EDX = ci[3];

        __cpuid(ci, 1);
        CpuLeaf1_EAX = ci[0]; CpuLeaf1_EBX = ci[1]; CpuLeaf1_ECX = ci[2]; CpuLeaf1_EDX = ci[3];

        __cpuid(ci, 3);
        CpuLeaf3_ECX = ci[1]; CpuLeaf3_EDX = ci[2];

        __cpuid((int*)(CpuBrandString + 0),  0x80000002);
        __cpuid((int*)(CpuBrandString + 16), 0x80000003);
        __cpuid((int*)(CpuBrandString + 32), 0x80000004);
        CpuBrandString[48] = '\0';
    }

    void PrintDisk()
    {
        if (DiskSerial[0])
            Logger::Raw("[REAL-DISK]  Serial: %s\n", DiskSerial);
        else
            Logger::Raw("[REAL-DISK]  Serial: <need admin / PhysicalDrive0>\n");
        if (DiskModel[0])
            Logger::Raw("[REAL-DISK]  Model : %s\n", DiskModel);
    }

    void PrintMac()
    {
        if (MacAddress[0] || MacAddress[1])
        {
            Logger::Raw("[REAL-MAC]   %02X:%02X:%02X:%02X:%02X:%02x  [%s]\n",
                MacAddress[0], MacAddress[1], MacAddress[2],
                MacAddress[3], MacAddress[4], MacAddress[5],
                MacDescription);
            Logger::Raw("[REAL-MAC]   GUID : %s\n", MacAdapterName);
        }
        else
        {
            Logger::Raw("[REAL-MAC]   No ethernet adapter found\n");
        }
    }

    void PrintCpuid()
    {
        Logger::Raw("[REAL-CPUID] Vendor : %s\n", CpuVendorString);
        Logger::Raw("[REAL-CPUID] Leaf=0 : EBX=0x%08X ECX=0x%08X EDX=0x%08X\n",
            CpuLeaf0_EBX, CpuLeaf0_ECX, CpuLeaf0_EDX);
        Logger::Raw("[REAL-CPUID] Leaf=1 : EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X\n",
            CpuLeaf1_EAX, CpuLeaf1_EBX, CpuLeaf1_ECX, CpuLeaf1_EDX);
        Logger::Raw("[REAL-CPUID] Leaf=3 : ECX=0x%08X EDX=0x%08X (PSN)\n",
            CpuLeaf3_ECX, CpuLeaf3_EDX);
        Logger::Raw("[REAL-CPUID] Brand  : %s\n", CpuBrandString);
    }

    void CollectRandom()
    {
        // 1. CryptGenRandom
        HCRYPTPROV hProv = 0;
        if (CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        {
            CryptGenRandom(hProv, sizeof(RndCryptGen), RndCryptGen);
            CryptReleaseContext(hProv, 0);
        }

        // 2. BCryptGenRandom
        BCryptGenRandom(NULL, RndBCryptGen, sizeof(RndBCryptGen), BCRYPT_USE_SYSTEM_PREFERRED_RNG);

        // 3. RtlGenRandom (= SystemFunction036)
        RtlGenRandom(RndRtlGen, sizeof(RndRtlGen));

        // 4. CRT rand()
        srand((unsigned int)GetTickCount64());
        RndRandVal = rand();
    }

    void PrintRandom()
    {
        Logger::Raw("[REAL-RND]   CryptGenRandom: ");
        for (int i = 0; i < 16; i++)
            Logger::Raw("%02X", RndCryptGen[i]);
        Logger::Raw("\n");

        Logger::Raw("[REAL-RND]   BCryptGenRandom: ");
        for (int i = 0; i < 16; i++)
            Logger::Raw("%02X", RndBCryptGen[i]);
        Logger::Raw("\n");

        Logger::Raw("[REAL-RND]   RtlGenRandom:    ");
        for (int i = 0; i < 16; i++)
            Logger::Raw("%02X", RndRtlGen[i]);
        Logger::Raw("\n");

        Logger::Raw("[REAL-RND]   rand():          %d (0x%08X)\n", RndRandVal, (unsigned)RndRandVal);
    }
};
#pragma pack(pop)


// ============================================================
// HyperDbgEngine: VMM 加载/卸载/连接管理
// ============================================================
class HyperDbgEngine {
public:
    bool m_Loaded;

    HyperDbgEngine() : m_Loaded(false) {}
    ~HyperDbgEngine() { if (m_Loaded) Unload(); }

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
        hyperdbg_u_unload_vmm();
        hyperdbg_u_unload_kd();
        hyperdbg_u_stop_kd_driver();
        hyperdbg_u_uninstall_kd_driver();
        m_Loaded = false;
        Logger::Log(Logger::OK, "Unloaded");
    }

    bool ConnectLocal()
    {
        Logger::Log(Logger::INFO, "Connecting to local debug session...");
        fflush(stdout);
        INT ret = RunCmd(".connect local");
        Logger::Log(Logger::INFO, "connect returned: %d", ret);
        RunCmd("printf(\"[INIT] Environment ready\\n\");");
        return true;
    }

    static INT RunCmd(const char* cmd)
    {
        INT ret = hyperdbg_u_run_command((CHAR*)cmd);
        fflush(stdout);
        return ret;
    }

private:
    static int MessageCallback(const char* text)
    {
        Logger::Raw("%s", text);
        return 0;
    }
};


// ============================================================
// HookManager: 所有 Hook 注册与管理
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

        m_Engine.ConnectLocal();
        Logger::Log(Logger::INFO, "Target filter: %s", TARGET_PROCESS_NAME);

        m_RealHard.Collect();
        m_RealHard.Print();
    }

    void SetupAll()
    {
        Logger::Separator();
        Logger::Raw("  Setting up all hardware spoof hooks\n");
        Logger::Separator();
        Logger::Raw("\n");

        InitGlobals();
        SetupDiskAndMacHook();
        SetupCpuidHook();
        SetupRandomHooks();

        Logger::Log(Logger::OK, "All hooks active. Waiting for target process...");
    }

private:
    void InitGlobals()
    {
        m_Engine.RunCmd(".g_disk_hook_flag = 0");
        m_Engine.RunCmd(".g_disk_output_ptr = 0");
        m_Engine.RunCmd(".g_disk_output_size = 0");
        m_Engine.RunCmd(".g_mac_hook_flag = 0");
        m_Engine.RunCmd(".g_mac_output_ptr = 0");
        m_Engine.RunCmd(".g_mac_output_size = 0");
        m_Engine.RunCmd(std::string(".fake_hdd_sn = \"" FAKE_HDD_SN "\"").c_str());
        m_Engine.RunCmd(std::string(".fake_hdd_model = \"" FAKE_HDD_MODEL "\"").c_str());
        m_Engine.RunCmd(".fixed_rnd_data = { 0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80, 0x90,0xA0,0xB0,0xC0,0xD0,0xE0,0xF0,0x01 }");
    }

    // ---- 模块1：硬盘序列号 + 网卡MAC 无痕伪造 ----
    void SetupDiskAndMacHook()
    {
        Logger::Log(Logger::INFO, "Setting up disk serial + MAC address spoof hook");

        // Pre-hook: 标记目标IOCTL，记录buffer指针
        m_Engine.RunCmd(
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

        // Post-hook (sysret): 系统调用已返回，buffer中有原始数据
        m_Engine.RunCmd(
            "!sysret stage post script { "
                // 磁盘序列号 + 型号（STORAGE_DEVICE_DESCRIPTOR）
                "if (.g_disk_hook_flag == 1 && .g_disk_output_ptr != 0 && .g_disk_output_size > 0) { "
                    "if (strstr($procname, \"" TARGET_PROCESS_NAME "\") != 0) { "
                        // 读 STORAGE_DEVICE_DESCRIPTOR 头部获取偏移
                        "u32 sn_off = dword(.g_disk_output_ptr + 10); "   // SerialNumberOffset
                        "u32 mdl_off = dword(.g_disk_output_ptr + 14); "  // ProductIdOffset
                        // 打印原始SN
                        "printf(\"[BEFORE-DISK] %s SN: \", $procname); "
                        "if (sn_off > 0 && sn_off < .g_disk_output_size) { prints(.g_disk_output_ptr + sn_off, 40); } "
                        "else { printf(\"<none>\"); } "
                        "printf(\"\\n\"); "
                        // 打印原始Model
                        "printf(\"[BEFORE-DISK] %s Model: \", $procname); "
                        "if (mdl_off > 0 && mdl_off < .g_disk_output_size) { prints(.g_disk_output_ptr + mdl_off, 80); } "
                        "else { printf(\"<none>\"); } "
                        "printf(\"\\n\"); "
                        // 覆盖伪造SN
                        "if (sn_off > 0 && sn_off < .g_disk_output_size) { "
                            "memcpy(.g_disk_output_ptr + sn_off, .fake_hdd_sn, min(strlen(.fake_hdd_sn), .g_disk_output_size - sn_off)); "
                        "} "
                        // 覆盖伪造Model
                        "if (mdl_off > 0 && mdl_off < .g_disk_output_size) { "
                            "memcpy(.g_disk_output_ptr + mdl_off, .fake_hdd_model, min(strlen(.fake_hdd_model), .g_disk_output_size - mdl_off)); "
                        "} "
                        // 打印伪造值
                        "printf(\"[AFTER-DISK]  %s SN:    \", $procname); "
                        "if (sn_off > 0 && sn_off < .g_disk_output_size) { prints(.g_disk_output_ptr + sn_off, 40); } "
                        "printf(\"\\n\"); "
                        "printf(\"[AFTER-DISK]  %s Model: \", $procname); "
                        "if (mdl_off > 0 && mdl_off < .g_disk_output_size) { prints(.g_disk_output_ptr + mdl_off, 80); } "
                        "printf(\"\\n\"); "
                    "} "
                    ".g_disk_hook_flag = 0; "
                    ".g_disk_output_ptr = 0; "
                    ".g_disk_output_size = 0; "
                "} "

                // 网卡MAC
                "if (.g_mac_hook_flag == 1 && .g_mac_output_ptr != 0 && .g_mac_output_size >= 6) { "
                    "if (strstr($procname, \"" TARGET_PROCESS_NAME "\") != 0) { "
                        "printf(\"[BEFORE-MAC]  %s Original MAC: %%02x:%%02x:%%02x:%%02x:%%02x:%%02x\\n\", $procname, "
                            "byte(.g_mac_output_ptr), byte(.g_mac_output_ptr+1), byte(.g_mac_output_ptr+2), "
                            "byte(.g_mac_output_ptr+3), byte(.g_mac_output_ptr+4), byte(.g_mac_output_ptr+5)); "
                        "byte(.g_mac_output_ptr)   = 0xAA; "
                        "byte(.g_mac_output_ptr+1) = 0xBB; "
                        "byte(.g_mac_output_ptr+2) = 0xCC; "
                        "byte(.g_mac_output_ptr+3) = 0xDD; "
                        "byte(.g_mac_output_ptr+4) = 0xEE; "
                        "byte(.g_mac_output_ptr+5) = 0xFF; "
                        "printf(\"[AFTER-MAC]   %s Spoofed MAC: %%02x:%%02x:%%02x:%%02x:%%02x:%%02x\\n\", $procname, "
                            "byte(.g_mac_output_ptr), byte(.g_mac_output_ptr+1), byte(.g_mac_output_ptr+2), "
                            "byte(.g_mac_output_ptr+3), byte(.g_mac_output_ptr+4), byte(.g_mac_output_ptr+5)); "
                    "} "
                    ".g_mac_hook_flag = 0; "
                    ".g_mac_output_ptr = 0; "
                    ".g_mac_output_size = 0; "
                "} "
            "}");

        Logger::Log(Logger::OK, "Disk serial + MAC address hook registered");
    }

    // ---- 模块2：CPUID 伪造 ----
    void SetupCpuidHook()
    {
        Logger::Log(Logger::INFO, "Setting up CPUID fake hook");

        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
            "!cpuid script { "
                "if (strstr($procname, \"" TARGET_PROCESS_NAME "\") == 0) return; "
                "if ($cpuid_leaf == 1) { "
                    "printf(\"[BEFORE-CPUID] %%s Leaf=1 Original: EAX=0x%%llx RBX=0x%%llx RCX=0x%%llx RDX=0x%%llx\\n\", $procname, @rax, @rbx, @rcx, @rdx); "
                    "@rax = 0x%08X; "
                    "@rbx = 0x%08X; "
                    "@rcx = 0x%08X; "
                    "@rdx = 0x%08X; "
                    "printf(\"[AFTER-CPUID]  %%s Leaf=1 Spoofed: EAX=0x%%08x RBX=0x%%08x RCX=0x%%08x RDX=0x%%08x\\n\", $procname, @rax, @rbx, @rcx, @rdx); "
                "} "
            "}",
            FAKE_CPUID_EAX, FAKE_CPUID_EBX, FAKE_CPUID_ECX, FAKE_CPUID_EDX);

        m_Engine.RunCmd(cmd);
        Logger::Log(Logger::OK, "CPUID hook registered");
    }

    // ---- 模块3：随机数API劫持 ----
    void SetupRandomHooks()
    {
        Logger::Log(Logger::INFO, "Setting up random API hooks");

        // CryptGenRandom
        m_Engine.RunCmd(
            "!epthook advapi32!CryptGenRandom stage post script { "
                "if (strstr($procname, \"" TARGET_PROCESS_NAME "\") == 0) return; "
                "u32 req_len = dword(@rdx); "
                "void* buf_ptr = ptr(@r8); "
                "printf(\"[BEFORE-CryptGenRandom] %%s len=%%d data=\", $procname, req_len); "
                "prints(buf_ptr, min(req_len, 16)); "
                "printf(\"\\n\"); "
                "u32 copy_len = min(req_len, sizeof(.fixed_rnd_data)); "
                "memcpy(buf_ptr, .fixed_rnd_data, copy_len); "
                "printf(\"[AFTER-CryptGenRandom]  %%s len=%%d data=\", $procname, req_len); "
                "prints(buf_ptr, min(req_len, 16)); "
                "printf(\"\\n\"); "
            "}");

        // BCryptGenRandom
        m_Engine.RunCmd(
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

        // RtlGenRandom
        m_Engine.RunCmd(
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

        // CRT rand()
        m_Engine.RunCmd(
            "!epthook msvcrt!rand stage post script { "
                "if (strstr($procname, \"" TARGET_PROCESS_NAME "\") != 0) { "
                    "printf(\"[BEFORE-rand] %%s Original: %%d\\n\", $procname, @rax); "
                    "@rax = 0x88776655; "
                    "printf(\"[AFTER-rand]  %%s Spoofed: 0x88776655\\n\", $procname); "
                "} "
            "}");

        Logger::Log(Logger::OK, "Random API hooks registered");
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

        m_pHooks = new HookManager(m_Engine);
        m_pHooks->m_RealHard.Collect();
        m_pHooks->m_RealHard.Print();

        // ===== Phase 2: 加载驱动 + 注册 Hook =====
        Logger::Separator();
        Logger::Raw("  PHASE 2: Load VMM + Register hooks\n");
        Logger::Separator();
        Logger::Raw("\n");

        m_pHooks->InitAll();   // Load VMM + connect local
        m_pHooks->SetupAll();  // Register all EPT/CPUID/random hooks

        // ===== Phase 3: 再次采集（Hook生效后，应返回伪造值） =====
        Logger::Separator();
        Logger::Raw("  PHASE 3: Collect SPOOFED hardware (after hook)\n");
        Logger::Separator();
        Logger::Raw("\n");

        HardwareInfo spoofed;
        spoofed.Collect();
        Logger::Raw("[SPOOF-DISK]  Serial: %s\n", spoofed.DiskSerial);
        Logger::Raw("[SPOOF-DISK]  Model : %s\n", spoofed.DiskModel);
        if (spoofed.MacAddress[0] || spoofed.MacAddress[1])
            Logger::Raw("[SPOOF-MAC]   %02X:%02X:%02X:%02X:%02X:%02x  [%s]\n",
                spoofed.MacAddress[0], spoofed.MacAddress[1], spoofed.MacAddress[2],
                spoofed.MacAddress[3], spoofed.MacAddress[4], spoofed.MacAddress[5],
                spoofed.MacDescription);
        else
            Logger::Raw("[SPOOF-MAC]   <hooked>\n");
        Logger::Raw("[SPOOF-CPUID] Leaf=1: EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X\n",
            spoofed.CpuLeaf1_EAX, spoofed.CpuLeaf1_EBX,
            spoofed.CpuLeaf1_ECX, spoofed.CpuLeaf1_EDX);

        Logger::Separator();
        Logger::Raw("\n[+] Hooks active. Press Enter to unload...\n");
        getchar();

        // ===== Phase 4: 卸载（析构自动处理） =====
        Logger::Log(Logger::INFO, "PHASE 4: Unloading...");
        return 0;
    }

private:
    static BOOL WINAPI ConsoleHandler(DWORD ctrlType)
    {
        if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT)
        {
            Logger::Raw("\n[*] Ctrl+C received, exiting...\n");
            return TRUE;
        }
        return FALSE;
    }

    static void ShowUsage(const char* prog)
    {
        Logger::Raw("Usage: %s [command]\n", prog);
        Logger::Raw("Commands:\n");
        Logger::Raw("  init    - Load VMM + connect local\n");
        Logger::Raw("  hook    - Register all hardware spoof hooks\n");
        Logger::Raw("  run     - init + hook (default)\n");
        Logger::Raw("  unload  - Unload everything\n");
    }
};

int main(int argc, char* argv[])
{
    App app;
    return app.Run(argc, argv);
}
