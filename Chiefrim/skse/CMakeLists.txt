# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

# The Skyrim side of Chiefrim (docs/DESIGN.md): an SKSE plugin built with
# CommonLibSSE-NG. On Linux it cross-compiles with cmake/clang-cl-xwin.cmake
# (tools/setup_skse.sh); on Windows any MSVC or clang-cl setup works.

project(Chiefrim VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# ---- CommonLibSSE-NG's dependencies, fetched instead of vcpkg (vcpkg's
# Windows ports don't cross-compile from Linux). find_package() calls in
# CommonLib are answered through FetchContent's package redirects.

FetchContent_Declare(spdlog
	GIT_REPOSITORY https://github.com/gabime/spdlog.git
	GIT_TAG v1.17.0
	GIT_SHALLOW TRUE
	OVERRIDE_FIND_PACKAGE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL ON CACHE BOOL "" FORCE) # CommonLib's install(EXPORT) needs spdlog in an export set
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(rapidcsv
	GIT_REPOSITORY https://github.com/d99kris/rapidcsv.git
	GIT_TAG v8.99
	GIT_SHALLOW TRUE
	SOURCE_SUBDIR do-not-build)
FetchContent_MakeAvailable(rapidcsv)
set(RAPIDCSV_INCLUDE_DIRS "${rapidcsv_SOURCE_DIR}/src" CACHE PATH "" FORCE)

# CommonLib uses only DirectXTK's SimpleMath (RE/S/State.h). Building all of
# DirectXTK needs Microsoft's shader compiler, so build SimpleMath alone.
FetchContent_Declare(directxtk
	GIT_REPOSITORY https://github.com/microsoft/DirectXTK.git
	GIT_TAG may2026
	GIT_SHALLOW TRUE
	SOURCE_SUBDIR do-not-build)
FetchContent_MakeAvailable(directxtk)
add_library(chiefrim_simplemath STATIC "${directxtk_SOURCE_DIR}/Src/SimpleMath.cpp")
target_include_directories(chiefrim_simplemath
	PUBLIC "$<BUILD_INTERFACE:${directxtk_SOURCE_DIR}/Inc>"
	PRIVATE "${directxtk_SOURCE_DIR}/Src")
target_compile_definitions(chiefrim_simplemath PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_library(Microsoft::DirectXTK ALIAS chiefrim_simplemath)
install(TARGETS chiefrim_simplemath EXPORT chiefrim-simplemath) # for CommonLib's install(EXPORT); never installed
install(EXPORT chiefrim-simplemath DESTINATION share/chiefrim-simplemath)
file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/directxtk-config.cmake" "# Microsoft::DirectXTK is SimpleMath only (skse/CMakeLists.txt)\n")

# ---- CommonLibSSE-NG (SE + AE; VR off to keep the build smaller)

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(COMMONLIB_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)
if(NOT COMMAND add_commonlibsse_plugin)
	include(extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)
endif()

# ---- the plugin

file(GLOB_RECURSE CHIEFRIM_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)

add_commonlibsse_plugin(
	Chiefrim
	NAME "Chiefrim"
	AUTHOR "Chiefrim"
	USE_ADDRESS_LIBRARY
	SOURCES ${CHIEFRIM_SOURCES}
)

target_include_directories(Chiefrim PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
target_precompile_headers(Chiefrim PRIVATE src/PCH.h)
target_link_libraries(Chiefrim PRIVATE d3dcompiler.lib) # the compositor's shaders (src/Overlay.cpp)
target_compile_definitions(Chiefrim PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	target_compile_options(Chiefrim PRIVATE /W4 /utf-8 -Wno-c++98-compat -Wno-unused-parameter)
elseif(MSVC)
	target_compile_options(Chiefrim PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8)
endif()
