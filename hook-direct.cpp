/**
 * @file hook-direct.cpp
 * @brief Direct ioctl-based hook registration for HyperDbg
 *
 * Bypasses the command parser (InterpretGeneralEventAndActionsFields)
 * to directly construct event/action structures and send ioctls.
 * This removes limitations like "!epthook stage post" rejection
 * and script variable scoping issues.
 */

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "SDK/HyperDbgSdk.h"
#include "SDK/imports/user/HyperDbgLibImports.h"
#include "SDK/imports/user/HyperDbgScriptImports.h"

#include "hook-direct.h"

// ====================== Internal globals ======================
static HANDLE g_HookDeviceHandle = NULL;
static UINT64 g_HookEventTag     = 0x1000; // Start from a safe value

// ====================== Internal helpers ======================

static UINT64 GetNextEventTag()
{
    return g_HookEventTag++;
}

/**
 * @brief Compile a script string into a symbol buffer
 * @param scriptBody The script text
 * @param[out] outBufferAddress Address of compiled script buffer
 * @param[out] outBufferLength Length of compiled script buffer
 * @param[out] outBufferPointer Pointer count in script buffer
 * @param[out] outCodeBuffer The code buffer handle (to free later)
 * @return true on success
 */
static bool CompileScript(const char* scriptBody,
                          UINT64& outBufferAddress,
                          UINT32& outBufferLength,
                          UINT32& outBufferPointer,
                          UINT64& outCodeBuffer)
{
    if (!scriptBody || scriptBody[0] == '\0')
        return false;

    PVOID codeBuffer = ScriptEngineParse((CHAR*)scriptBody);
    if (!codeBuffer)
    {
        printf("[hook-direct] ScriptEngineParse returned NULL\n");
        return false;
    }

    PSYMBOL_BUFFER symBuf = (PSYMBOL_BUFFER)codeBuffer;

    // Check for syntax errors
    if (symBuf->Message != NULL)
    {
        printf("[hook-direct] Script syntax error: %s\n", symBuf->Message);
        RemoveSymbolBuffer(symBuf);
        return false;
    }

    // Extract buffer info
    outBufferAddress = (UINT64)symBuf->Head;
    outBufferLength  = (UINT32)(symBuf->Pointer * sizeof(SYMBOL));
    outBufferPointer = (UINT32)symBuf->Pointer;
    outCodeBuffer    = (UINT64)codeBuffer;

    return true;
}

/**
 * @brief Send an event to the kernel via ioctl
 */
static bool SendEventIoctl(PDEBUGGER_GENERAL_EVENT_DETAIL event, UINT32 eventLength)
{
    if (!g_HookDeviceHandle || g_HookDeviceHandle == INVALID_HANDLE_VALUE)
    {
        printf("[hook-direct] Device handle not open\n");
        return false;
    }

    DEBUGGER_EVENT_AND_ACTION_RESULT result = {0};
    DWORD returnedLength = 0;

    BOOL status = DeviceIoControl(
        g_HookDeviceHandle,
        IOCTL_DEBUGGER_REGISTER_EVENT,
        event,
        eventLength,
        &result,
        sizeof(result),
        &returnedLength,
        NULL
    );

    if (!status)
    {
        printf("[hook-direct] RegisterEvent ioctl failed, err=0x%x\n", GetLastError());
        return false;
    }

    if (!result.IsSuccessful || result.Error != 0)
    {
        printf("[hook-direct] RegisterEvent failed, error=0x%x\n", result.Error);
        return false;
    }

    return true;
}

/**
 * @brief Send an action to the kernel via ioctl
 */
static bool SendActionIoctl(PDEBUGGER_GENERAL_ACTION action, UINT32 actionLength)
{
    if (!g_HookDeviceHandle || g_HookDeviceHandle == INVALID_HANDLE_VALUE)
    {
        printf("[hook-direct] Device handle not open\n");
        return false;
    }

    DEBUGGER_EVENT_AND_ACTION_RESULT result = {0};
    DWORD returnedLength = 0;

    BOOL status = DeviceIoControl(
        g_HookDeviceHandle,
        IOCTL_DEBUGGER_ADD_ACTION_TO_EVENT,
        action,
        actionLength,
        &result,
        sizeof(result),
        &returnedLength,
        NULL
    );

    if (!status)
    {
        printf("[hook-direct] AddAction ioctl failed, err=0x%x\n", GetLastError());
        return false;
    }

    if (!result.IsSuccessful || result.Error != 0)
    {
        printf("[hook-direct] AddAction failed, error=0x%x\n", result.Error);
        return false;
    }

    return true;
}

/**
 * @brief Core function: register an event with a script action
 * @param eventType VMM_EVENT_TYPE_ENUM value
 * @param stage Calling stage
 * @param optionalParam1 Event-specific optional param 1
 * @param optionalParam2 Event-specific optional param 2
 * @param scriptBody The script to execute
 * @param commandStr Command string for tracing (can be empty)
 * @return true on success
 */
static bool RegisterEventWithScript(
    VMM_EVENT_TYPE_ENUM eventType,
    VMM_CALLBACK_EVENT_CALLING_STAGE_TYPE stage,
    UINT64 optionalParam1,
    UINT64 optionalParam2,
    const char* scriptBody,
    const char* commandStr)
{
    // 1. Compile the script
    UINT64 scriptBufAddr = 0;
    UINT32 scriptBufLen  = 0;
    UINT32 scriptBufPtr  = 0;
    UINT64 codeBufHandle = 0;

    if (!CompileScript(scriptBody, scriptBufAddr, scriptBufLen, scriptBufPtr, codeBufHandle))
    {
        return false;
    }

    // 2. Allocate and fill the Event structure
    UINT32 eventLength = sizeof(DEBUGGER_GENERAL_EVENT_DETAIL);
    PDEBUGGER_GENERAL_EVENT_DETAIL event = (PDEBUGGER_GENERAL_EVENT_DETAIL)malloc(eventLength);
    if (!event)
    {
        RemoveSymbolBuffer((PSYMBOL_BUFFER)codeBufHandle);
        return false;
    }
    memset(event, 0, eventLength);

    event->Tag          = GetNextEventTag();
    event->CoreId       = DEBUGGER_EVENT_APPLY_TO_ALL_CORES;
    event->ProcessId    = DEBUGGER_EVENT_APPLY_TO_ALL_PROCESSES;
    event->IsEnabled    = TRUE;
    event->EventStage   = stage;
    event->EventType    = eventType;
    event->CountOfActions = 1; // We'll add one script action

    event->Options.OptionalParam1 = optionalParam1;
    event->Options.OptionalParam2 = optionalParam2;

    // CommandStringBuffer is user-mode only (for tracing), can be NULL
    event->CommandStringBuffer = NULL;
    event->ConditionBufferSize = 0;

    // 3. Send the event to kernel
    bool ok = SendEventIoctl(event, eventLength);
    if (!ok)
    {
        printf("[hook-direct] Failed to register event (type=%d, tag=0x%llx)\n",
               eventType, event->Tag);
        free(event);
        RemoveSymbolBuffer((PSYMBOL_BUFFER)codeBufHandle);
        return false;
    }

    UINT64 eventTag = event->Tag;
    free(event);

    // 4. Allocate and fill the Action structure (script action)
    UINT32 actionLength = sizeof(DEBUGGER_GENERAL_ACTION) + scriptBufLen;
    PDEBUGGER_GENERAL_ACTION action = (PDEBUGGER_GENERAL_ACTION)malloc(actionLength);
    if (!action)
    {
        RemoveSymbolBuffer((PSYMBOL_BUFFER)codeBufHandle);
        return false;
    }
    memset(action, 0, actionLength);

    action->EventTag              = eventTag;
    action->ActionType            = RUN_SCRIPT;
    action->ImmediateMessagePassing = TRUE;
    action->PreAllocatedBuffer    = 0;
    action->CustomCodeBufferSize  = 0;
    action->ScriptBufferSize      = scriptBufLen;
    action->ScriptBufferPointer   = scriptBufPtr;

    // Copy script buffer after the action header
    if (scriptBufLen > 0 && scriptBufAddr != 0)
    {
        memcpy((PVOID)((UINT64)action + sizeof(DEBUGGER_GENERAL_ACTION)),
               (PVOID)scriptBufAddr,
               scriptBufLen);
    }

    // 5. Send the action to kernel
    ok = SendActionIoctl(action, actionLength);
    if (!ok)
    {
        printf("[hook-direct] Failed to add script action (tag=0x%llx)\n", eventTag);
    }

    // 6. Cleanup
    free(action);
    RemoveSymbolBuffer((PSYMBOL_BUFFER)codeBufHandle);

    return ok;
}

// ====================== Public API ======================

bool HookDeviceOpen()
{
    if (g_HookDeviceHandle && g_HookDeviceHandle != INVALID_HANDLE_VALUE)
        return true; // Already open

    g_HookDeviceHandle = CreateFileA(
        "\\\\.\\HyperDbgDebuggerDevice",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (g_HookDeviceHandle == INVALID_HANDLE_VALUE)
    {
        printf("[hook-direct] Failed to open HyperDbg device, err=0x%x\n", GetLastError());
        g_HookDeviceHandle = NULL;
        return false;
    }

    printf("[hook-direct] Device opened successfully\n");
    return true;
}

void HookDeviceClose()
{
    if (g_HookDeviceHandle && g_HookDeviceHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_HookDeviceHandle);
        g_HookDeviceHandle = NULL;
    }
}

UINT64 HookResolveSymbol(const char* symbolName)
{
    BOOLEAN wasFound = FALSE;
    UINT64 addr = ScriptEngineConvertNameToAddress(symbolName, &wasFound);
    if (!wasFound || addr == 0)
    {
        printf("[hook-direct] Failed to resolve symbol: %s\n", symbolName);
        return 0;
    }
    return addr;
}

bool HookCpuid(const char* scriptBody, HookCallingStage stage)
{
    VMM_CALLBACK_EVENT_CALLING_STAGE_TYPE vmmStage =
        (VMM_CALLBACK_EVENT_CALLING_STAGE_TYPE)stage;

    return RegisterEventWithScript(
        CPUID_INSTRUCTION_EXECUTION,
        vmmStage,
        0,  // OptionalParam1: 0 = all CPUID leaves
        0,  // OptionalParam2: not used when OptionalParam1=0
        scriptBody,
        "!cpuid script { ... }"
    );
}

bool HookSyscall(UINT64 syscallNumber, const char* scriptBody, HookCallingStage stage)
{
    VMM_CALLBACK_EVENT_CALLING_STAGE_TYPE vmmStage =
        (VMM_CALLBACK_EVENT_CALLING_STAGE_TYPE)stage;

    // OptionalParam1 = syscall number (0xFFFFFFFF = all)
    // OptionalParam2 = 0 = SAFE_ACCESS_MEMORY mode
    return RegisterEventWithScript(
        SYSCALL_HOOK_EFER_SYSCALL,
        vmmStage,
        syscallNumber,
        0,  // DEBUGGER_EVENT_SYSCALL_SYSRET_SAFE_ACCESS_MEMORY
        scriptBody,
        "!syscall script { ... }"
    );
}

bool HookSysret(const char* scriptBody, HookCallingStage stage)
{
    VMM_CALLBACK_EVENT_CALLING_STAGE_TYPE vmmStage =
        (VMM_CALLBACK_EVENT_CALLING_STAGE_TYPE)stage;

    // OptionalParam2 = 0 = SAFE_ACCESS_MEMORY mode
    return RegisterEventWithScript(
        SYSCALL_HOOK_EFER_SYSRET,
        vmmStage,
        0,  // No optional param for sysret
        0,  // DEBUGGER_EVENT_SYSCALL_SYSRET_SAFE_ACCESS_MEMORY
        scriptBody,
        "!sysret script { ... }"
    );
}

bool HookEptHiddenBp(UINT64 targetAddress, const char* scriptBody)
{
    if (targetAddress == 0)
    {
        printf("[hook-direct] HookEptHiddenBp: targetAddress is NULL\n");
        return false;
    }

    // !epthook only supports stage pre
    return RegisterEventWithScript(
        HIDDEN_HOOK_EXEC_CC,
        VMM_CALLBACK_CALLING_STAGE_PRE_EVENT_EMULATION,
        targetAddress,  // OptionalParam1 = hook address
        0,
        scriptBody,
        "!epthook script { ... }"
    );
}

bool HookEptDetours(UINT64 targetAddress, const char* scriptBody)
{
    if (targetAddress == 0)
    {
        printf("[hook-direct] HookEptDetours: targetAddress is NULL\n");
        return false;
    }

    // !epthook2 only supports stage pre
    return RegisterEventWithScript(
        HIDDEN_HOOK_EXEC_DETOURS,
        VMM_CALLBACK_CALLING_STAGE_PRE_EVENT_EMULATION,
        targetAddress,  // OptionalParam1 = hook address
        0,
        scriptBody,
        "!epthook2 script { ... }"
    );
}
