cmake_minimum_required(VERSION 3.24)
project(roadpilot_input LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "RoadPilot Input targets Windows x64 (named pipes, SCS plugin DLL).")
endif()

option(ROADPILOT_BUILD_PLUGIN "Build the SCS input plugin DLL (requires SCS_SDK_ROOT)" ON)
option(ROADPILOT_BUILD_TESTS "Build the tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform independent logic: protocol + state. No SDK, no Windows API.
add_library(roadpilot_input_core STATIC src/input_protocol.cpp src/input_state.cpp)
target_include_directories(roadpilot_input_core PUBLIC src)
if(MSVC)
  target_compile_options(roadpilot_input_core PRIVATE /W4 /permissive- /utf-8)
endif()

if(ROADPILOT_BUILD_PLUGIN)
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Build for x64 (use -A x64 with Visual Studio generators). The 64-bit games load only 64-bit plugins.")
  endif()
  # The SDK is not part of this repository. Point SCS_SDK_ROOT at the extracted SCS SDK 1.16.
  if(NOT DEFINED SCS_SDK_ROOT)
    set(SCS_SDK_ROOT "$ENV{SCS_SDK_ROOT}")
  endif()
  set(_scs_required
    include/scssdk.h include/scssdk_value.h
    include/scssdk_input.h include/scssdk_input_device.h include/scssdk_input_event.h
    include/eurotrucks2/scssdk_eut2.h include/eurotrucks2/scssdk_input_eut2.h
    include/amtrucks/scssdk_ats.h include/amtrucks/scssdk_input_ats.h)
  set(_scs_missing "")
  foreach(_f IN LISTS _scs_required)
    if("${SCS_SDK_ROOT}" STREQUAL "" OR NOT EXISTS "${SCS_SDK_ROOT}/${_f}")
      list(APPEND _scs_missing "${_f}")
    endif()
  endforeach()
  if(_scs_missing)
    string(REPLACE ";" "\n  " _scs_list "${_scs_missing}")
    message(FATAL_ERROR
      "SCS SDK files not found. Set SCS_SDK_ROOT (cmake -DSCS_SDK_ROOT=<dir> or environment variable) "
      "to the extracted SCS SDK 1.16 directory (the one containing readme.txt and include/).\n"
      "SCS_SDK_ROOT = '${SCS_SDK_ROOT}'\nMissing relative to it:\n  ${_scs_list}\n"
      "Or configure with -DROADPILOT_BUILD_PLUGIN=OFF to build only the SDK-independent core and tests.")
  endif()
  set(_scs_includes "${SCS_SDK_ROOT}/include" "${SCS_SDK_ROOT}/include/eurotrucks2" "${SCS_SDK_ROOT}/include/amtrucks")

  add_library(roadpilot_input_plugin SHARED src/plugin.cpp src/pipe_server.cpp src/roadpilot_input.def)
  target_include_directories(roadpilot_input_plugin PRIVATE ${_scs_includes})
  target_compile_definitions(roadpilot_input_plugin PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(roadpilot_input_plugin PRIVATE roadpilot_input_core)
  set_target_properties(roadpilot_input_plugin PROPERTIES OUTPUT_NAME "roadpilot-input")
  if(MSVC)
    target_compile_options(roadpilot_input_plugin PRIVATE /W4 /permissive- /utf-8)
  endif()
endif()

if(ROADPILOT_BUILD_TESTS)
  enable_testing()
  add_executable(roadpilot_core_tests tests/core_tests.cpp)
  target_link_libraries(roadpilot_core_tests PRIVATE roadpilot_input_core)
  if(MSVC)
    target_compile_options(roadpilot_core_tests PRIVATE /W4 /permissive- /utf-8)
  endif()
  add_test(NAME core_tests COMMAND roadpilot_core_tests)

  if(ROADPILOT_BUILD_PLUGIN)
    add_executable(roadpilot_plugin_host_tests tests/plugin_host_tests.cpp)
    target_include_directories(roadpilot_plugin_host_tests PRIVATE ${_scs_includes})
    target_link_libraries(roadpilot_plugin_host_tests PRIVATE roadpilot_input_core)
    target_compile_definitions(roadpilot_plugin_host_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
      ROADPILOT_PLUGIN_PATH="$<TARGET_FILE:roadpilot_input_plugin>")
    if(MSVC)
      target_compile_options(roadpilot_plugin_host_tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_dependencies(roadpilot_plugin_host_tests roadpilot_input_plugin)
    add_test(NAME plugin_host_tests COMMAND roadpilot_plugin_host_tests)
    set_tests_properties(plugin_host_tests PROPERTIES TIMEOUT 60)
  endif()
endif()
