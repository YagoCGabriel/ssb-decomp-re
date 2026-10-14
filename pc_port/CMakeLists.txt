cmake_minimum_required(VERSION 3.16)
project(ssb_pc_port C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(PCPORT_HAVE_VULKAN "Build the Vulkan backend" OFF)
option(PCPORT_HAVE_D3D    "Build the Direct3D 10/11 backends (Windows only)" OFF)

add_library(ssb_pc_core STATIC
    src/gbi_interpreter.c
    src/port_common.c
    src/libultra_stub.c
    src/rom_loader.c
    src/backend/sw_raster.c
    src/backend/null.c)
target_include_directories(ssb_pc_core PUBLIC include)
if(MSVC)
  target_compile_options(ssb_pc_core PRIVATE /W4)
else()
  target_compile_options(ssb_pc_core PRIVATE -Wall -Wextra)
endif()

set(PCPORT_BACKEND_SOURCES "")
if((WIN32 OR MINGW) AND PCPORT_HAVE_D3D)
  # Zero import-time dependencies: d3d11.dll, d3d10.dll and d3dcompiler.dll are
  # loaded at runtime (LoadLibrary/GetProcAddress), so the final .exe needs no
  # extra installs on the end-user machine. Only kernel/user/gdi are linked.
  list(APPEND PCPORT_BACKEND_SOURCES src/backend/d3d11.c src/backend/d3d10.c)
  target_compile_definitions(ssb_pc_core PUBLIC PCPORT_HAVE_D3D)
  target_link_libraries(ssb_pc_core PUBLIC user32 gdi32 ole32)
endif()
if(PCPORT_HAVE_VULKAN)
  # vulkan.c is self-contained (embedded ABI subset + dlopen of libvulkan at
  # runtime): no SDK headers or link libraries required, nothing to install.
  list(APPEND PCPORT_BACKEND_SOURCES src/backend/vulkan.c)
  target_compile_definitions(ssb_pc_core PUBLIC PCPORT_HAVE_VULKAN)
  if(NOT WIN32 AND NOT MINGW)
    target_link_libraries(ssb_pc_core PUBLIC dl)
  endif()
endif()
target_sources(ssb_pc_core PRIVATE ${PCPORT_BACKEND_SOURCES})

add_executable(ssb_pc src/main.c)
target_link_libraries(ssb_pc PRIVATE ssb_pc_core m)

enable_testing()
add_executable(test_gbi tests/test_gbi.c)
target_link_libraries(test_gbi PRIVATE ssb_pc_core m)
add_test(NAME gbi_headless COMMAND test_gbi)
set_tests_properties(gbi_headless PROPERTIES ENVIRONMENT "SSB_BACKEND=null")

# ---------------------------------------------------------------------------
# Native setup wizard ("Janela Setup"): ROM picker + validation + build/run.
# Built as an optional target so headless CI without a display still works.
# ---------------------------------------------------------------------------
add_executable(ssb_setup tools/setup_wizard.c)
if(WIN32 OR MINGW)
  # native GetOpenFileNameW file dialog
  target_link_libraries(ssb_setup PRIVATE comdlg32 ole32)
endif()
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND AND NOT WIN32)
  pkg_check_modules(GTK3 QUIET gtk+-3.0)
  if(GTK3_FOUND)
    target_compile_definitions(ssb_setup PRIVATE PCPORT_HAVE_GTK)
    target_include_directories(ssb_setup PRIVATE ${GTK3_INCLUDE_DIRS})
    target_link_libraries(ssb_setup PRIVATE ${GTK3_LIBRARIES})
    message(STATUS "ssb_setup: native GTK3 file dialog enabled")
  endif()
endif()
if(MSVC)
  target_compile_options(ssb_setup PRIVATE /W4)
else()
  target_compile_options(ssb_setup PRIVATE -Wall -Wextra)
endif()
