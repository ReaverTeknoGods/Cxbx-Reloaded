cmake_minimum_required (VERSION 3.12)
project(nv2a-pfifo-verify)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)

add_executable(nv2a-pfifo-verify
 "${CXBXR_ROOT_DIR}/src/devices/video/NV2AConstDump.h"
 "${CXBXR_ROOT_DIR}/src/devices/video/NV2AConstDump.cpp"
 "${CXBXR_ROOT_DIR}/src/devices/video/tools/NV2AConstDumpVerify.cpp"
)

target_include_directories(nv2a-pfifo-verify PRIVATE
 "${CXBXR_ROOT_DIR}/src"
)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
 add_compile_options(/EHsc /MP)
endif()

set_target_properties(nv2a-pfifo-verify PROPERTIES
 FOLDER Cxbx-Reloaded
)