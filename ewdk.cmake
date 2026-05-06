
# EWDK CMake configuration for Windows Driver Kit

# Find Windows Kits installation path
if(DEFINED ENV{WindowsSdkDir})
    set(WDK_ROOT "$ENV{WindowsSdkDir}")
elseif(DEFINED ENV{WDKContentRoot})
    set(WDK_ROOT "$ENV{WDKContentRoot}")
else()
    # Try common installation paths
    find_path(WDK_ROOT
        NAMES
            "Include/10.0.26100.0/km"
            "Include/10.0.22621.0/km"
            "Include/10.0.22000.0/km"
            "Include/10.0.20348.0/km"
            "Include/10.0.19041.0/km"
        PATHS
            "C:/Program Files (x86)/Windows Kits/10"
            "C:/Program Files/Windows Kits/10"
        DOC "Windows Driver Kit root directory"
    )
endif()

if(NOT WDK_ROOT)
    message(FATAL_ERROR "Windows Driver Kit not found. Please install WDK.")
endif()

# Find the latest Windows SDK version
file(GLOB WDK_VERSIONS RELATIVE "${WDK_ROOT}/Include" "${WDK_ROOT}/Include/10.*")
list(SORT WDK_VERSIONS)
list(REVERSE WDK_VERSIONS)
list(GET WDK_VERSIONS 0 WDK_VERSION)

message(STATUS "Found WDK: ${WDK_ROOT} (version: ${WDK_VERSION})")

# Set WDK paths
set(WDK_INCLUDE_DIR "${WDK_ROOT}/Include/${WDK_VERSION}")
set(WDK_LIB_DIR "${WDK_ROOT}/Lib/${WDK_VERSION}")

# Function to create a kernel-mode library
function(km_lib name)
    cmake_parse_arguments(PARSE_ARGV 1 KMLIB "" "" "")
    
    add_library(${name} STATIC ${KMLIB_UNPARSED_ARGUMENTS})
    
    # Set include directories
    target_include_directories(${name} PRIVATE
        "${WDK_INCLUDE_DIR}/km"
        "${WDK_INCLUDE_DIR}/km/crt"
        "${WDK_INCLUDE_DIR}/shared"
        "${WDK_INCLUDE_DIR}/um"
    )
    
    # Set compile definitions
    target_compile_definitions(${name} PRIVATE
        _AMD64_
        _WIN64
        POOL_NX_OPTIN=1
    )
    
    # Set compile options
    target_compile_options(${name} PRIVATE
        /W4 /WX
        /MT
        /GS
        /GL-
        /sdl
    )
    
    # Set language standards if needed
    target_compile_features(${name} PRIVATE c_std_11 cxx_std_17)
endfunction()

# Function to create a kernel-mode driver
function(km_sys name)
    cmake_parse_arguments(PARSE_ARGV 1 KMSYS "" "KMDF" "")
    
    add_executable(${name} SHARED ${KMSYS_UNPARSED_ARGUMENTS})
    
    # Set include directories
    target_include_directories(${name} PRIVATE
        "${WDK_INCLUDE_DIR}/km"
        "${WDK_INCLUDE_DIR}/km/crt"
        "${WDK_INCLUDE_DIR}/shared"
        "${WDK_INCLUDE_DIR}/um"
    )
    
    # Set compile definitions
    target_compile_definitions(${name} PRIVATE
        _AMD64_
        _WIN64
        POOL_NX_OPTIN=1
    )
    
    # Set compile options
    target_compile_options(${name} PRIVATE
        /W4 /WX
        /MT
        /GS
        /GL-
        /sdl
    )
    
    # Set link options
    target_link_options(${name} PRIVATE
        /DRIVER
        /SUBSYSTEM:NATIVE
        /MANIFEST:NO
        /DEBUG
        /MACHINE:X64
        /INTEGRITYCHECK
    )
    
    # Set libraries to link
    target_link_libraries(${name}
        "${WDK_LIB_DIR}/km/x64/ntoskrnl.lib"
        "${WDK_LIB_DIR}/km/x64/hal.lib"
        "${WDK_LIB_DIR}/km/x64/wdmsec.lib"
    )
    
    # If KMDF is specified
    if(KMSYS_KMDF)
        target_compile_definitions(${name} PRIVATE KMDF_VERSION_MAJOR=1 KMDF_VERSION_MINOR=15)
        target_link_libraries(${name}
            "${WDK_LIB_DIR}/km/x64/WdfLdr.lib"
            "${WDK_LIB_DIR}/km/x64/WdfDriverEntry.lib"
        )
    endif()
    
    # Set output to sys
    set_target_properties(${name} PROPERTIES
        SUFFIX ".sys"
        PREFIX ""
    )
endfunction()

# Function to create a user-mode library
function(um_lib name)
    cmake_parse_arguments(PARSE_ARGV 1 UMLIB "" "" "")
    
    add_library(${name} STATIC ${UMLIB_UNPARSED_ARGUMENTS})
    
    # Set include directories
    target_include_directories(${name} PRIVATE
        "${WDK_INCLUDE_DIR}/um"
        "${WDK_INCLUDE_DIR}/shared"
    )
    
    # Set compile definitions
    target_compile_definitions(${name} PRIVATE
        _AMD64_
        _WIN64
        _CRT_SECURE_NO_WARNINGS
    )
    
    # Set compile options
    target_compile_options(${name} PRIVATE /W4)
    
    # Set language standards
    target_compile_features(${name} PRIVATE c_std_11 cxx_std_17)
endfunction()

# Function to create a user-mode executable
function(um_exe name)
    cmake_parse_arguments(PARSE_ARGV 1 UMEXE "" "" "")
    
    add_executable(${name} ${UMEXE_UNPARSED_ARGUMENTS})
    
    # Set include directories
    target_include_directories(${name} PRIVATE
        "${WDK_INCLUDE_DIR}/um"
        "${WDK_INCLUDE_DIR}/shared"
    )
    
    # Set compile definitions
    target_compile_definitions(${name} PRIVATE
        _AMD64_
        _WIN64
        _CRT_SECURE_NO_WARNINGS
    )
    
    # Set compile options
    target_compile_options(${name} PRIVATE /W4)
    
    # Set link options
    target_link_options(${name} PRIVATE
        /SUBSYSTEM:CONSOLE
        /DEBUG
        /MACHINE:X64
    )
endfunction()
