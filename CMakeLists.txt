cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_SOURCE_DIR}/ewdk.cmake")

project(HyperDbgUnified C CXX ASM_MASM)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)




set(WDK_NTDDI_VERSION "0x0A000008")

set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
set(CMAKE_C_FLAGS_DEBUG "/Od /Zi /RTC1")
set(CMAKE_C_FLAGS "/Od /Zi /RTC1")
set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi /RTC1")
set(CMAKE_CXX_FLAGS "/Od /Zi /RTC1")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    "."
    "HyperDbg/hyperdbg/include"
    "HyperDbg/hyperdbg/dependencies/zydis/include"
    "HyperDbg/hyperdbg/dependencies/zydis/src"
    "HyperDbg/hyperdbg/dependencies/zydis/dependencies/zycore/include"
    "HyperDbg/hyperdbg/dependencies"
)

add_definitions(
    -DZYCORE_STATIC_BUILD
    -DZYDIS_STATIC_BUILD
)

file(GLOB ZydisSourceFiles "HyperDbg/hyperdbg/dependencies/zydis/src/*.c")
file(GLOB ZycoreSourceFiles "HyperDbg/hyperdbg/dependencies/zydis/dependencies/zycore/src/*.c")
set(ZydisSourceFiles ${ZydisSourceFiles} ${ZycoreSourceFiles})

file(GLOB_RECURSE HyperkdSourceFiles "HyperDbg/hyperdbg/hyperkd/code/*.c")
file(GLOB HyperkdAsmFiles "HyperDbg/hyperdbg/hyperkd/code/assembly/*.asm")

file(GLOB_RECURSE HyperhvSourceFiles "HyperDbg/hyperdbg/hyperhv/code/*.c")
list(FILTER HyperhvSourceFiles EXCLUDE REGEX ".*code/common/UnloadDll\\.c$")
file(GLOB HyperhvAsmFiles "HyperDbg/hyperdbg/hyperhv/code/assembly/*.asm")

file(GLOB HyperlogSourceFiles "HyperDbg/hyperdbg/hyperlog/code/*.c")

file(GLOB KdserialSourceFiles "HyperDbg/hyperdbg/kdserial/*.c")

file(GLOB ScriptEvalSourceFiles "HyperDbg/hyperdbg/script-eval/code/*.c")

file(GLOB HyperevadeSourceFiles "HyperDbg/hyperdbg/hyperevade/code/*.c")

file(GLOB_RECURSE HypertraceSourceFiles "HyperDbg/hyperdbg/hypertrace/code/*.c")

file(GLOB IncludeOptimizationsSourceFiles "HyperDbg/hyperdbg/include/components/optimizations/code/*.c")
file(GLOB IncludeSpinlockSourceFiles "HyperDbg/hyperdbg/include/components/spinlock/code/*.c")
file(GLOB IncludePlatformSourceFiles "HyperDbg/hyperdbg/include/platform/kernel/code/*.c")

km_lib(zydis_kernel ${ZydisSourceFiles})
target_compile_definitions(zydis_kernel PRIVATE ZYAN_NO_LIBC ZYDIS_NO_LIBC)
target_compile_options(zydis_kernel PRIVATE /Od /Zi /RTC1)

km_lib(Hyperhv ${HyperhvSourceFiles} ${HyperhvAsmFiles} ${IncludeOptimizationsSourceFiles} ${IncludeSpinlockSourceFiles})
target_include_directories(Hyperhv PRIVATE
    "HyperDbg/hyperdbg/hyperhv"
    "HyperDbg/hyperdbg/hyperhv/header"
    "HyperDbg/hyperdbg/hyperhv/code"
    "HyperDbg/hyperdbg/include/components/optimizations/code"
    "HyperDbg/hyperdbg/include/components/spinlock/code"
)
target_compile_definitions(Hyperhv PRIVATE DHYPERDBG_VMM HYPERDBG_HYPEREVADE HYPERDBG_HYPER_LOG HYPERDBG_HYPERTRACE NTDDI_VERSION=0x0A000008)
target_compile_options(Hyperhv PRIVATE /Od /Zi /RTC1)
target_precompile_headers(Hyperhv PRIVATE "HyperDbg/hyperdbg/hyperhv/pch.h")

km_lib(Hyperlog ${HyperlogSourceFiles})
target_include_directories(Hyperlog PRIVATE
    "HyperDbg/hyperdbg/hyperlog"
    "HyperDbg/hyperdbg/hyperlog/header"
    "HyperDbg/hyperdbg/hyperlog/code"
)
target_compile_definitions(Hyperlog PRIVATE DHYPERDBG_HYPERLOG HYPERDBG_HYPER_LOG HYPERDBG_HYPERTRACE HYPERDBG_HYPEREVADE HYPERDBG_VMM NTDDI_VERSION=0x0A000008)
target_compile_options(Hyperlog PRIVATE /Od /Zi /RTC1)
target_precompile_headers(Hyperlog PRIVATE "HyperDbg/hyperdbg/hyperlog/header/pch.h")

km_lib(Kdserial ${KdserialSourceFiles})
target_include_directories(Kdserial PRIVATE
    "HyperDbg/hyperdbg/kdserial"
)
target_compile_options(Kdserial PRIVATE /Od /Zi /RTC1)

km_lib(Hyperevade ${HyperevadeSourceFiles})
target_include_directories(Hyperevade PRIVATE
    "HyperDbg/hyperdbg/hyperevade"
    "HyperDbg/hyperdbg/hyperevade/header"
    "HyperDbg/hyperdbg/hyperevade/code"
)
target_compile_definitions(Hyperevade PRIVATE DHYPERDBG_HYPEREVADE HYPERDBG_HYPEREVADE HYPERDBG_HYPER_LOG HYPERDBG_HYPERTRACE HYPERDBG_VMM NTDDI_VERSION=0x0A000008)
target_compile_options(Hyperevade PRIVATE /Od /Zi /RTC1)
target_precompile_headers(Hyperevade PRIVATE "HyperDbg/hyperdbg/hyperevade/header/pch.h")

km_lib(Hypertrace ${HypertraceSourceFiles})
target_include_directories(Hypertrace PRIVATE
    "HyperDbg/hyperdbg/hypertrace"
    "HyperDbg/hyperdbg/hypertrace/header"
    "HyperDbg/hyperdbg/hypertrace/code"
)
target_compile_definitions(Hypertrace PRIVATE DHYPERDBG_HYPERTRACE HYPERDBG_HYPERTRACE HYPERDBG_HYPER_LOG HYPERDBG_HYPEREVADE HYPERDBG_VMM NTDDI_VERSION=0x0A000008)
target_compile_options(Hypertrace PRIVATE /Od /Zi /RTC1)
target_precompile_headers(Hypertrace PRIVATE "HyperDbg/hyperdbg/hypertrace/header/pch.h")

km_lib(Platform ${IncludePlatformSourceFiles})
target_include_directories(Platform PRIVATE
    "HyperDbg/hyperdbg/include/platform/kernel/code"
    "HyperDbg/hyperdbg/include/platform/kernel/header"
    "HyperDbg/hyperdbg/include"
)
target_compile_definitions(Platform PRIVATE NTDDI_VERSION=0x0A000008)
target_compile_options(Platform PRIVATE /Od /Zi /RTC1 /FIntddk.h /FIntstrsafe.h /FISDK/headers/Constants.h)

km_sys(hyperkd
    KMDF 1.15
    ${HyperkdSourceFiles}
    ${HyperkdAsmFiles}
    ${IncludeOptimizationsSourceFiles}
    ${IncludeSpinlockSourceFiles}
    ${ScriptEvalSourceFiles}
)
target_include_directories(hyperkd PRIVATE
    "HyperDbg/hyperdbg/hyperkd"
    "HyperDbg/hyperdbg/hyperkd/header"
    "HyperDbg/hyperdbg/hyperkd/code"
    "HyperDbg/hyperdbg/include/components/optimizations/code"
    "HyperDbg/hyperdbg/include/components/spinlock/code"
    "HyperDbg/hyperdbg/script-eval"
)
target_compile_definitions(hyperkd PRIVATE DHYPERDBG_DEBUGGER HYPERDBG_HYPEREVADE HYPERDBG_HYPER_LOG HYPERDBG_HYPERTRACE HYPERDBG_VMM NTDDI_VERSION=0x0A000008)
target_precompile_headers(hyperkd PRIVATE "HyperDbg/hyperdbg/hyperkd/header/pch.h")
target_compile_options(hyperkd PRIVATE /Od /Zi /RTC1)
target_link_options(hyperkd PRIVATE /DEBUG /FORCE:MULTIPLE)

target_link_libraries(hyperkd
    zydis_kernel
    Hyperhv
    Hyperlog
    Kdserial
    Hyperevade
    Hypertrace
    Platform
)

# ============================================================
# User-mode targets
# ============================================================

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

um_lib(Zydis ${ZydisSourceFiles})
target_compile_options(Zydis PRIVATE /Od /Zi)

file(GLOB_RECURSE PdbexSourceFiles "HyperDbg/hyperdbg/dependencies/pdbex/Source/*.cpp")
list(FILTER PdbexSourceFiles EXCLUDE REGEX ".*main\\.cpp$")

um_lib(Pdbex ${PdbexSourceFiles})
target_include_directories(Pdbex PRIVATE
    "HyperDbg/hyperdbg/dependencies/pdbex/Source"
    "$ENV{VSINSTALLDIR}DIA SDK\\include"
)
target_compile_definitions(Pdbex PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(Pdbex PRIVATE /Od /Zi /std:c++latest)

file(GLOB SymbolParserSourceFiles "HyperDbg/hyperdbg/symbol-parser/code/*.cpp")

um_lib(SymbolParser ${SymbolParserSourceFiles})
target_include_directories(SymbolParser PRIVATE
    "HyperDbg/hyperdbg/symbol-parser"
    "HyperDbg/hyperdbg/symbol-parser/header"
    "HyperDbg/hyperdbg/symbol-parser/code"
    "HyperDbg/hyperdbg/include"
    "HyperDbg/hyperdbg/dependencies"
    "HyperDbg/hyperdbg/dependencies/pdbex/Source"
)
target_compile_definitions(SymbolParser PRIVATE HYPERDBG_SYMBOL_PARSER _CRT_SECURE_NO_WARNINGS)
target_compile_options(SymbolParser PRIVATE /Od /Zi)
target_precompile_headers(SymbolParser PRIVATE "HyperDbg/hyperdbg/symbol-parser/pch.h")
target_link_libraries(SymbolParser Pdbex)

file(GLOB ScriptEngineSourceFiles "HyperDbg/hyperdbg/script-engine/code/*.c")
list(APPEND ScriptEngineSourceFiles "HyperDbg/hyperdbg/script-engine/script_include.c")

um_lib(ScriptEngine ${ScriptEngineSourceFiles})
target_include_directories(ScriptEngine PRIVATE
    "HyperDbg/hyperdbg/script-engine"
    "HyperDbg/hyperdbg/script-engine/header"
    "HyperDbg/hyperdbg/script-engine/code"
    "HyperDbg/hyperdbg/include"
    "HyperDbg/hyperdbg/script-eval"
)
target_compile_definitions(ScriptEngine PRIVATE SCRIPTENGINE_EXPORTS HYPERDBG_SCRIPT_ENGINE HYPERDBG_SYMBOL_PARSER _CRT_SECURE_NO_WARNINGS)
target_compile_options(ScriptEngine PRIVATE /Od /Zi)
target_precompile_headers(ScriptEngine PRIVATE "HyperDbg/hyperdbg/script-engine/header/pch.h")
target_link_libraries(ScriptEngine SymbolParser)

file(GLOB_RECURSE LibhyperdbgSourceFiles "HyperDbg/hyperdbg/libhyperdbg/code/*.cpp")
list(FILTER LibhyperdbgSourceFiles EXCLUDE REGEX ".*dllmain\\.cpp$")
list(APPEND LibhyperdbgSourceFiles "HyperDbg/hyperdbg/libhyperdbg/pch.cpp")
file(GLOB LibhyperdbgAsmFiles "HyperDbg/hyperdbg/libhyperdbg/code/assembly/*.asm")
file(GLOB ScriptEvalCppFiles "HyperDbg/hyperdbg/script-eval/code/*.c")

foreach(SRC ${ScriptEvalCppFiles})
    get_filename_component(SRC_NAME ${SRC} NAME)
    configure_file(${SRC} "${CMAKE_CURRENT_BINARY_DIR}/script-eval-cpp/${SRC_NAME}pp" COPYONLY)
endforeach()

file(GLOB ScriptEvalCppRenamedFiles "${CMAKE_CURRENT_BINARY_DIR}/script-eval-cpp/*.cpp")

um_lib(ScriptEvalUser ${ScriptEvalCppRenamedFiles})
target_include_directories(ScriptEvalUser PRIVATE
    "HyperDbg/hyperdbg/script-eval"
    "HyperDbg/hyperdbg/include"
    "HyperDbg/hyperdbg/dependencies"
    "HyperDbg/hyperdbg/dependencies/phnt"
    "HyperDbg/hyperdbg/dependencies/zydis/include"
    "HyperDbg/hyperdbg/dependencies/zydis/dependencies/zycore/include"
    "HyperDbg/hyperdbg/libhyperdbg"
    "HyperDbg/hyperdbg/script-engine/header"
    "HyperDbg/hyperdbg/symbol-parser/header"
)
target_compile_definitions(ScriptEvalUser PRIVATE
    HYPERDBG_LIBHYPERDBG
    HYPERDBG_SCRIPT_ENGINE
    HYPERDBG_SYMBOL_PARSER
    ZYCORE_STATIC_DEFINE
    ZYDIS_STATIC_DEFINE
    HPRDBGCTRL_EXPORTS
    NTDDI_VERSION=0x06010000
    WINVER=0x0601
    _WIN32_WINNT=0x0601
    _CRT_SECURE_NO_WARNINGS
)
target_compile_options(ScriptEvalUser PRIVATE /Od /Zi)
target_precompile_headers(ScriptEvalUser PRIVATE "HyperDbg/hyperdbg/libhyperdbg/pch.h")

um_lib(Libhyperdbg
    ${LibhyperdbgSourceFiles}
    ${LibhyperdbgAsmFiles}
)
target_include_directories(Libhyperdbg PRIVATE
    "HyperDbg/hyperdbg/libhyperdbg"
    "HyperDbg/hyperdbg/libhyperdbg/header"
    "HyperDbg/hyperdbg/libhyperdbg/code"
    "HyperDbg/hyperdbg/include"
    "HyperDbg/hyperdbg/dependencies"
    "HyperDbg/hyperdbg/dependencies/phnt"
    "HyperDbg/hyperdbg/dependencies/zydis/include"
    "HyperDbg/hyperdbg/dependencies/zydis/dependencies/zycore/include"
    "HyperDbg/hyperdbg/dependencies/keystone/include"
    "HyperDbg/hyperdbg/dependencies/pdbex/Source"
    "HyperDbg/hyperdbg/script-eval"
    "HyperDbg/hyperdbg/script-engine/header"
    "HyperDbg/hyperdbg/symbol-parser/header"
)
target_compile_definitions(Libhyperdbg PRIVATE
    HYPERDBG_LIBHYPERDBG
    HYPERDBG_SCRIPT_ENGINE
    HYPERDBG_SYMBOL_PARSER
    ZYCORE_STATIC_DEFINE
    ZYDIS_STATIC_DEFINE
    HPRDBGCTRL_EXPORTS
    NTDDI_VERSION=0x06010000
    WINVER=0x0601
    _WIN32_WINNT=0x0601
    _CRT_SECURE_NO_WARNINGS
)
target_compile_options(Libhyperdbg PRIVATE /Od /Zi)
set_source_files_properties(${LibhyperdbgAsmFiles} PROPERTIES LANGUAGE ASM_MASM)
target_precompile_headers(Libhyperdbg PRIVATE "HyperDbg/hyperdbg/libhyperdbg/pch.h")
target_link_libraries(Libhyperdbg
    ScriptEngine
    SymbolParser
    Pdbex
    Zydis
    ScriptEvalUser
)

um_exe(hyperdbg-cli "HyperDbg/hyperdbg/hyperdbg-cli/hyperdbg-cli.cpp")
target_include_directories(hyperdbg-cli PRIVATE
    "HyperDbg/hyperdbg/include"
    "HyperDbg/hyperdbg/dependencies"
)
target_compile_definitions(hyperdbg-cli PRIVATE
    HYPERDBG_LIBHYPERDBG
    HYPERDBG_SCRIPT_ENGINE
    HYPERDBG_SYMBOL_PARSER
    HYPERDBG_DEBUG
    _CRT_SECURE_NO_WARNINGS
)
target_compile_options(hyperdbg-cli PRIVATE /Od /Zi)
target_link_options(hyperdbg-cli PRIVATE /DEBUG /FORCE:MULTIPLE
    /ALTERNATENAME:__imp_pdbex_export=pdbex_export
    /ALTERNATENAME:__imp_pdbex_set_logging_method_export=pdbex_set_logging_method_export
)
target_link_libraries(hyperdbg-cli
    Libhyperdbg
    dbghelp
    urlmon
    shell32
    ole32
    oleaut32
    keystone
)
target_link_directories(hyperdbg-cli PRIVATE
    "HyperDbg/hyperdbg/libraries/keystone/debug-lib"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

add_custom_command(TARGET hyperdbg-cli PRE_LINK
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/Zydis.lib"
        "${CMAKE_CURRENT_BINARY_DIR}/Zycore.lib"
)
