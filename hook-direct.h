/**
 * @file hook-direct.h
 * @brief Direct ioctl-based hook registration for HyperDbg
 *
 * Bypasses the command parser (InterpretGeneralEventAndActionsFields)
 * to directly construct event/action structures and send ioctls.
 * This removes limitations like "!epthook stage post" rejection
 * and script variable scoping issues.
 */

#pragma once

#include <Windows.h>
#include <string>

// Calling stages (matches VMM_CALLBACK_EVENT_CALLING_STAGE_TYPE)
enum HookCallingStage {
    HOOK_STAGE_PRE  = 1,  // VMM_CALLBACK_CALLING_STAGE_PRE_EVENT_EMULATION
    HOOK_STAGE_POST = 2,  // VMM_CALLBACK_CALLING_STAGE_POST_EVENT_EMULATION
    HOOK_STAGE_ALL  = 3,  // VMM_CALLBACK_CALLING_STAGE_ALL_EVENT_EMULATION
};

/**
 * @brief Register a CPUID hook with a script
 * @param scriptBody The script to execute on CPUID events
 * @param stage Calling stage (pre/post/all)
 * @return true on success
 */
bool HookCpuid(const char* scriptBody, HookCallingStage stage = HOOK_STAGE_PRE);

/**
 * @brief Register a syscall hook with a script
 * @param syscallNumber Specific syscall number, or 0xFFFFFFFF for all
 * @param scriptBody The script to execute on syscall events
 * @param stage Calling stage (pre/post/all)
 * @return true on success
 */
bool HookSyscall(UINT64 syscallNumber, const char* scriptBody, HookCallingStage stage = HOOK_STAGE_PRE);

/**
 * @brief Register a sysret hook with a script
 * @param scriptBody The script to execute on sysret events
 * @param stage Calling stage (pre/post/all)
 * @return true on success
 */
bool HookSysret(const char* scriptBody, HookCallingStage stage = HOOK_STAGE_POST);

/**
 * @brief Register an EPT hidden breakpoint hook (!epthook) with a script
 * @param targetAddress The address to hook (e.g. from HookResolveSymbol)
 * @param scriptBody The script to execute on hook trigger
 * @return true on success
 */
bool HookEptHiddenBp(UINT64 targetAddress, const char* scriptBody);

/**
 * @brief Register an EPT detours hook (!epthook2) with a script
 * @param targetAddress The address to hook
 * @param scriptBody The script to execute on hook trigger
 * @return true on success
 */
bool HookEptDetours(UINT64 targetAddress, const char* scriptBody);

/**
 * @brief Resolve a symbol name like "nt!NtDeviceIoControlFile" to an address
 * @param symbolName The symbol to resolve
 * @return The address, or 0 on failure
 */
UINT64 HookResolveSymbol(const char* symbolName);

/**
 * @brief Open the HyperDbg device handle for direct ioctl
 * @return true on success
 */
bool HookDeviceOpen();

/**
 * @brief Close the HyperDbg device handle
 */
void HookDeviceClose();
