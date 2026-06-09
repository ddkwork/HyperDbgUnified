cmake_minimum_required(VERSION 3.28)

include("C:/Program Files/CMake/bin/ewdk.cmake")

# Generator-expression-wrapped KM compile flags for targets with .asm files.
# Prevents C-only flags (/GF, /GR-, /Gz, /kernel, /FI, /Oi) from being
# passed to MASM, which would cause A4018 warnings.
set(_KM_CXX_FLAGS "")
foreach(_flag /Zp8 /GF /GR- /Gz /kernel "/FIwarning.h" "/FI${KM_ADDITIONAL_FLAGS_FILE}" /Oi)
    list(APPEND _KM_CXX_FLAGS "$<$<COMPILE_LANGUAGE:C,CXX>:${_flag}>")
endforeach()

project(HyperDbgUnified C CXX ASM_MASM)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)




set(WDK_NTDDI_VERSION "0x0A000008")

set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
set(CMAKE_C_FLAGS_RELEASE "/Zi")
set(CMAKE_CXX_FLAGS_RELEASE "/Zi")

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS 1)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS 1)
set(CMAKE_C_USE_RESPONSE_FILE_FOR_INCLUDES 1)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_INCLUDES 1)

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
file(GLOB HyperhvAsmFiles "HyperDbg/hyperdbg/hyperhv/code/assembly/*.asm")

file(GLOB HyperlogSourceFiles "HyperDbg/hyperdbg/hyperlog/code/*.c")

file(GLOB KdserialSourceFiles "HyperDbg/hyperdbg/kdserial/*.c")

file(GLOB HyperevadeSourceFiles "HyperDbg/hyperdbg/hyperevade/code/*.c")

file(GLOB_RECURSE HypertraceSourceFiles "HyperDbg/hyperdbg/hypertrace/code/*.c")

file(GLOB IncludeOptimizationsSourceFiles "HyperDbg/hyperdbg/include/components/optimizations/code/*.c")
file(GLOB IncludeSpinlockSourceFiles "HyperDbg/hyperdbg/include/components/spinlock/code/*.c")
file(GLOB IncludePlatformSourceFiles "HyperDbg/hyperdbg/include/platform/kernel/code/*.c")

km_lib(zydis_kernel ${ZydisSourceFiles})
target_compile_definitions(zydis_kernel PRIVATE ZYAN_NO_LIBC ZYDIS_NO_LIBC)
target_compile_options(zydis_kernel PRIVATE /Od /Zi /RTC1)

km_lib(Hyperhv ${HyperhvSourceFiles} ${HyperhvAsmFiles} ${IncludeOptimizationsSourceFiles} ${IncludeSpinlockSourceFiles})
# Override COMPILE_OPTIONS with generator expressions (has .asm files)
set_target_properties(Hyperhv PROPERTIES COMPILE_OPTIONS "${_KM_CXX_FLAGS}")
target_include_directories(Hyperhv PRIVATE
    "HyperDbg/hyperdbg/hyperhv"
    "HyperDbg/hyperdbg/hyperhv/header"
    "HyperDbg/hyperdbg/hyperhv/code"
    "HyperDbg/hyperdbg/include/components/optimizations/code"
    "HyperDbg/hyperdbg/include/components/spinlock/code"
)
target_compile_definitions(Hyperhv PRIVATE DHYPERDBG_VMM HYPERDBG_HYPEREVADE HYPERDBG_HYPER_LOG HYPERDBG_HYPERTRACE NTDDI_VERSION=0x0A000008)
target_compile_options(Hyperhv PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/Od> $<$<COMPILE_LANGUAGE:C,CXX>:/Zi> $<$<COMPILE_LANGUAGE:C,CXX>:/RTC1>)
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
    "HyperDbg/hyperdbg/hyperhv"
    "HyperDbg/hyperdbg/hyperhv/header"
    "HyperDbg/hyperdbg/hyperhv/code"
)
target_compile_definitions(Platform PRIVATE HYPERDBG_KERNEL_MODE HYPERDBG_VMM DHYPERDBG_VMM HYPERDBG_HYPEREVADE HYPERDBG_HYPER_LOG HYPERDBG_HYPERTRACE NTDDI_VERSION=0x0A000008)
target_compile_options(Platform PRIVATE /Od /Zi /RTC1)
target_precompile_headers(Platform PRIVATE "HyperDbg/hyperdbg/hyperhv/pch.h")

km_sys(hyperkd
    KMDF 1.15
    ${HyperkdSourceFiles}
    ${HyperkdAsmFiles}
    ${IncludeOptimizationsSourceFiles}
    ${IncludeSpinlockSourceFiles}
)
# Override COMPILE_OPTIONS with generator expressions (has .asm files)
set_target_properties(hyperkd PROPERTIES COMPILE_OPTIONS "${_KM_CXX_FLAGS}")
target_include_directories(hyperkd PRIVATE
    "HyperDbg/hyperdbg/hyperkd"
    "HyperDbg/hyperdbg/hyperkd/header"
    "HyperDbg/hyperdbg/hyperkd/code"
    "HyperDbg/hyperdbg/include/components/optimizations/code"
    "HyperDbg/hyperdbg/include/components/spinlock/code"
)
target_compile_definitions(hyperkd PRIVATE DHYPERDBG_DEBUGGER HYPERDBG_HYPEREVADE HYPERDBG_HYPER_LOG HYPERDBG_HYPERTRACE HYPERDBG_VMM NTDDI_VERSION=0x0A000008)
target_precompile_headers(hyperkd PRIVATE "HyperDbg/hyperdbg/hyperkd/header/pch.h")
target_compile_options(hyperkd PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/Od> $<$<COMPILE_LANGUAGE:C,CXX>:/Zi> $<$<COMPILE_LANGUAGE:C,CXX>:/RTC1>)
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
# User-mode: epthook-gui (单exe，包含所有源文件)
# ============================================================

file(GLOB ZydisUserSourceFiles "HyperDbg/hyperdbg/dependencies/zydis/src/*.c")
file(GLOB ZycoreUserSourceFiles "HyperDbg/hyperdbg/dependencies/zydis/dependencies/zycore/src/*.c")
set(ZydisUserSourceFiles ${ZydisUserSourceFiles} ${ZycoreUserSourceFiles})

file(GLOB_RECURSE LibhyperdbgSourceFiles "HyperDbg/hyperdbg/libhyperdbg/code/*.cpp")
list(FILTER LibhyperdbgSourceFiles EXCLUDE REGEX ".*dllmain\\.cpp$")
list(APPEND LibhyperdbgSourceFiles "HyperDbg/hyperdbg/libhyperdbg/pch.cpp")
file(GLOB LibhyperdbgAsmFiles "HyperDbg/hyperdbg/libhyperdbg/code/assembly/*.asm")

set(EpthookGuiSources
    ${ZydisUserSourceFiles}
    ${LibhyperdbgSourceFiles}
    "HyperDbg/hyperdbg/include/components/pe/code/pe-image-reader.cpp"
    "HyperDbg/hyperdbg/include/platform/user/code/platform-intrinsics.c"
    "HyperDbg/hyperdbg/include/platform/user/code/windows-only/windows-privilege.c"
)

set_source_files_properties(
    "HyperDbg/hyperdbg/include/platform/user/code/platform-intrinsics.c"
    "HyperDbg/hyperdbg/include/platform/user/code/windows-only/windows-privilege.c"
    PROPERTIES LANGUAGE CXX
)
set_source_files_properties(${LibhyperdbgAsmFiles} PROPERTIES LANGUAGE ASM_MASM)

# Zydis files用C编译，其余用CXX
foreach(SRC ${ZydisUserSourceFiles})
    set_source_files_properties(${SRC} PROPERTIES LANGUAGE C)
endforeach()

add_executable(epthook-gui WIN32 ${EpthookGuiSources})
set_target_properties(epthook-gui PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

target_include_directories(epthook-gui PRIVATE
    ${WDK_UM_INCLUDE_DIRS}
    "HyperDbg/hyperdbg/libhyperdbg"
    "HyperDbg/hyperdbg/libhyperdbg/header"
    "HyperDbg/hyperdbg/libhyperdbg/code"
    "HyperDbg/hyperdbg/include"
    "HyperDbg/hyperdbg/include/components/pe/header"
    "HyperDbg/hyperdbg/include/components/pe/code"
    "HyperDbg/hyperdbg/include/platform/user/header"
    "HyperDbg/hyperdbg/include/platform/user/header/windows-only"
    "HyperDbg/hyperdbg/include/SDK/headers"
    "HyperDbg/hyperdbg/dependencies"
    "HyperDbg/hyperdbg/dependencies/phnt"
    "HyperDbg/hyperdbg/dependencies/zydis/include"
    "HyperDbg/hyperdbg/dependencies/zydis/src"
    "HyperDbg/hyperdbg/dependencies/zydis/dependencies/zycore/include"
    "HyperDbg/hyperdbg/dependencies/keystone/include"
    "HyperDbg/hyperdbg/include/keystone"
    ${QT_INCLUDE_DIRS}
)

target_compile_definitions(epthook-gui PRIVATE
    _CRT_SECURE_NO_WARNINGS
    HYPERDBG_LIBHYPERDBG
    HYPERDBG_USER_MODE
    ZYCORE_STATIC_BUILD
    ZYDIS_STATIC_BUILD
    NTDDI_VERSION=0x06010000
    WINVER=0x0601
    _WIN32_WINNT=0x0601
    MIQT_BUILDING_DLL
    MIQT_WINDOWSQTSTATIC
    QT_STATIC
    QT_VERSION_MAJOR=6
    QT_VERSION_MINOR=11
    QT_VERSION_PATCH=0
    Q_CORE_EXPORT=
    Q_GUI_EXPORT=
    Q_WIDGETS_EXPORT=
)

target_compile_options(epthook-gui PRIVATE /O2 /Zi /MT ${QT_COMPILE_OPTIONS})
target_link_options(epthook-gui PRIVATE
    /DEBUG /FORCE:MULTIPLE /SUBSYSTEM:WINDOWS
)
foreach(_lib_dir ${WDK_UM_LIB_DIRS})
    target_link_options(epthook-gui PRIVATE "/LIBPATH:${_lib_dir}")
endforeach()
target_link_options(epthook-gui PRIVATE "/LIBPATH:${CMAKE_SOURCE_DIR}/HyperDbg/hyperdbg/libraries/keystone/release-lib")

target_link_libraries(epthook-gui
    ${QT_LINK_LIBRARIES}
    dbghelp
    psapi
    keystone
    synchronization
)

