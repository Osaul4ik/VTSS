cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(VTSS)
include("${VITASDK}/share/vita.cmake" REQUIRED)

# Build id written into the log header (CI passes it; else from git)
if(NOT DEFINED VTSS_BUILD)
  execute_process(COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE VTSS_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT VTSS_BUILD)
    set(VTSS_BUILD "dev")
  endif()
endif()
add_definitions(-DVTSS_BUILD="${VTSS_BUILD}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -std=gnu99")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fno-rtti -fno-exceptions")

include_directories(
)

link_directories(
  ${CMAKE_CURRENT_BINARY_DIR}
)

add_executable(VTSS
  main.c
  renderer.c
  benchlog.c
  settings.c
  settings_page.c
)

target_link_libraries(VTSS
  taihen_stub
  SceLibKernel_stub_weak
  SceIofilemgr_stub
  SceAppMgr_stub_weak
  SceKernelThreadMgr_stub
  SceRtc_stub
  SceSysmem_stub
  SceDisplay_stub
  SceCtrl_stub
  k
  gcc
  ScePower_stub
)

set_target_properties(VTSS
  PROPERTIES LINK_FLAGS "-nostdlib"
)

vita_create_self(VTSS.suprx VTSS
  CONFIG ${CMAKE_SOURCE_DIR}/VTSS.yml
)
