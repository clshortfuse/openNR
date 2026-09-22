cmake_minimum_required(VERSION 3.23)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(opennr_native_tests LANGUAGES C CXX)
endif()

get_filename_component(OPENNR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(OPENNR_NATIVE_DEPS "${OPENNR_ROOT}/local/native-dependencies" CACHE PATH "Local SDK source dependency root")
if(NOT EXISTS "${OPENNR_NATIVE_DEPS}/manifest.json")
  message(FATAL_ERROR "Import SDK sources using benchmarks/import_native_dependencies.py first")
endif()
file(READ "${OPENNR_ROOT}/profiles/compat-fp16.json" OPENNR_PROFILE)
string(JSON OPENNR_CUDA_TOOLKIT_VERSION GET "${OPENNR_PROFILE}" cuda_toolkit version)
find_package(CUDAToolkit ${OPENNR_CUDA_TOOLKIT_VERSION} EXACT REQUIRED)
find_path(CUPTI_INCLUDE_DIR cupti.h HINTS "${CUDAToolkit_LIBRARY_ROOT}/extras/CUPTI/include" REQUIRED)
find_library(CUPTI_LIBRARY NAMES cupti HINTS "${CUDAToolkit_LIBRARY_ROOT}/extras/CUPTI/lib64" REQUIRED)

add_executable(dlssnr-repro d3d12/main.cpp d3d12/cupti_telemetry.cpp
  "${OPENNR_NATIVE_DEPS}/minhook/src/buffer.c"
  "${OPENNR_NATIVE_DEPS}/minhook/src/hde/hde64.c"
  "${OPENNR_NATIVE_DEPS}/minhook/src/hook.c"
  "${OPENNR_NATIVE_DEPS}/minhook/src/trampoline.c")
add_executable(dlssnr-d3d11-repro d3d11/main.cpp)
foreach(target dlssnr-repro dlssnr-d3d11-repro)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  target_include_directories(${target} PRIVATE include
    "${OPENNR_NATIVE_DEPS}/reshade" "${OPENNR_NATIVE_DEPS}/streamline/include"
    "${OPENNR_NATIVE_DEPS}/ngx/include")
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endforeach()
target_include_directories(dlssnr-repro PRIVATE "${OPENNR_NATIVE_DEPS}/minhook/include"
  "${OPENNR_NATIVE_DEPS}/minhook/src" "${CUPTI_INCLUDE_DIR}")
target_link_libraries(dlssnr-repro PRIVATE bcrypt CUDA::cuda_driver d3d12 dbghelp dxgi "${CUPTI_LIBRARY}")
target_link_libraries(dlssnr-d3d11-repro PRIVATE d3d11 d3d12 dxgi)