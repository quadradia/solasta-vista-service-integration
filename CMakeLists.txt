cmake_minimum_required(VERSION 3.15)
# CMP0091: CMAKE_MSVC_RUNTIME_LIBRARY selects the static CRT (no VC++ redistributable on the XP box).
cmake_policy(SET CMP0091 NEW)
project(SolastaVistaSync C CXX)

if(NOT MSVC)
  message(FATAL_ERROR "Build with MSVC: the v141_xp toolset for Windows XP, see README.md")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Configure a 32-bit build: cmake -A Win32 -T v141_xp ...")
endif()

if(NOT CMAKE_GENERATOR_TOOLSET STREQUAL "v141_xp")
  message(WARNING "Toolset is '${CMAKE_GENERATOR_TOOLSET}', not v141_xp. The exe will still say it needs Windows XP "
                  "(subsystem 5.01), but only v141_xp is supported by Microsoft for XP. Use it for builds that go on the XP machine.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Windows XP SP3 on a Pentium II: no SSE/SSE2 code, XP API level, XP PE header.
add_compile_definitions(_WIN32_WINNT=0x0501 WINVER=0x0501 UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
                        _CRT_SECURE_NO_WARNINGS)
string(APPEND CMAKE_C_FLAGS " /arch:IA32 /GS")
string(APPEND CMAKE_CXX_FLAGS " /arch:IA32 /Zc:threadSafeInit- /GS")
add_link_options(/SUBSYSTEM:CONSOLE,5.01 /OSVERSION:5.1 /DYNAMICBASE /NXCOMPAT)

# ---- mbedTLS 3.6 LTS (vendored, unmodified; options in src/mbedtls_user_config.h) ----
file(GLOB MBEDTLS_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/mbedtls/library/*.c)
add_library(mbedtls STATIC ${MBEDTLS_SOURCES})
target_include_directories(mbedtls PUBLIC ${CMAKE_SOURCE_DIR}/third_party/mbedtls/include ${CMAKE_SOURCE_DIR}/src
                                   PRIVATE ${CMAKE_SOURCE_DIR}/third_party/mbedtls/library)
target_compile_definitions(mbedtls PUBLIC "MBEDTLS_USER_CONFIG_FILE=\"mbedtls_user_config.h\"")
# inet_pton() is Vista+; MBEDTLS_TEST_SW_INET_PTON selects mbedTLS's own parser (used for IP SANs).
target_compile_definitions(mbedtls PRIVATE MBEDTLS_TEST_SW_INET_PTON)
target_compile_options(mbedtls PRIVATE /W0)

# ---- Event Log message table ----
set(MSG_OUT ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${MSG_OUT})
find_program(MC_EXE NAMES mc mc.exe
  HINTS "$ENV{WindowsSdkVerBinPath}/x86" "C:/Program Files (x86)/Microsoft SDKs/Windows/v7.1A/Bin"
  PATHS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.19041.0/x86" "C:/Program Files (x86)/Windows Kits/10/bin/x86")
if(NOT MC_EXE)
  message(FATAL_ERROR "mc.exe (Message Compiler from the Windows SDK) not found; set -DMC_EXE=<path>")
endif()
add_custom_command(
  OUTPUT ${MSG_OUT}/messages.h ${MSG_OUT}/messages.rc ${MSG_OUT}/MSG00409.bin
  COMMAND "${MC_EXE}" -h "${MSG_OUT}" -r "${MSG_OUT}" "${CMAKE_SOURCE_DIR}/src/messages.mc"
  DEPENDS ${CMAKE_SOURCE_DIR}/src/messages.mc
  COMMENT "Compiling the Event Log message table")

# ---- the service ----
add_executable(vista-sync
  src/main.cpp src/config.cpp src/dbf.cpp src/http.cpp src/log.cpp src/mapping.cpp src/secret.cpp
  src/service.cpp src/sync.cpp src/util.cpp src/vista-sync.rc
  ${MSG_OUT}/messages.h)  # messages.rc is #included by vista-sync.rc
target_include_directories(vista-sync PRIVATE ${MSG_OUT} src)
target_compile_options(vista-sync PRIVATE /W4 /EHsc)
target_link_libraries(vista-sync PRIVATE mbedtls ws2_32 advapi32 crypt32)
