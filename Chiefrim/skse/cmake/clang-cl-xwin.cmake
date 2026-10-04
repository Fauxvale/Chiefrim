# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-compiles Windows x64 binaries on Linux with clang-cl and lld-link,
# against the MSVC CRT and Windows SDK that xwin downloads (docs/DESIGN.md §2).
#
#   XWIN_DIR: the folder `xwin splat` wrote (crt/ and sdk/); default
#             Chiefrim/.tools/xwin, which tools/setup_skse.sh fills.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT XWIN_DIR)
	get_filename_component(XWIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.tools/xwin" ABSOLUTE)
endif()
set(XWIN_DIR "${XWIN_DIR}" CACHE PATH "xwin splat output (crt/, sdk/)")
if(NOT EXISTS "${XWIN_DIR}/crt/include" OR NOT EXISTS "${XWIN_DIR}/sdk/include/um")
	message(FATAL_ERROR "No MSVC CRT/Windows SDK at ${XWIN_DIR}; run tools/setup_skse.sh")
endif()

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

set(_xwin_includes
	"${XWIN_DIR}/crt/include"
	"${XWIN_DIR}/sdk/include/ucrt"
	"${XWIN_DIR}/sdk/include/um"
	"${XWIN_DIR}/sdk/include/shared"
	"${XWIN_DIR}/sdk/include/winrt")
set(_xwin_libs
	"${XWIN_DIR}/crt/lib/x86_64"
	"${XWIN_DIR}/sdk/lib/ucrt/x86_64"
	"${XWIN_DIR}/sdk/lib/um/x86_64")

set(_compile_flags "--target=x86_64-pc-windows-msvc -fuse-ld=lld-link")
foreach(_dir IN LISTS _xwin_includes)
	string(APPEND _compile_flags " /imsvc \"${_dir}\"")
endforeach()
# CMake links through the clang-cl driver, so linker options go through -Xlinker.
set(_link_flags "")
foreach(_dir IN LISTS _xwin_libs)
	string(APPEND _link_flags " -Xlinker \"/libpath:${_dir}\"")
endforeach()

set(CMAKE_C_FLAGS_INIT "${_compile_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_compile_flags}")
set(CMAKE_RC_FLAGS_INIT "/I \"${XWIN_DIR}/sdk/include/um\" /I \"${XWIN_DIR}/sdk/include/shared\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_link_flags}")

# Never pick up Linux libraries or headers.
set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# xwin fetches the release CRT only (no msvcrtd.lib): always link /MD, also in
# CMake's own compiler checks.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
