cmake_minimum_required(VERSION 3.16)
project(completion LANGUAGES CXX)

# Completion-based file I/O: native backends (io_uring on Linux, kqueue on BSD and macOS,
# a thread pool elsewhere), image publication, and directory scans. Header-only.
include(FetchContent)
FetchContent_Declare(metrics GIT_REPOSITORY https://github.com/Kronuz/metrics.git GIT_TAG 7513d1e1042d6d9ce736b79c67f9c4e19e52e902)
# Shared owning PMR allocation contexts, without global allocation overrides.
FetchContent_Declare(allocators GIT_REPOSITORY https://github.com/Kronuz/allocators.git GIT_TAG c558fa76521857d8547ca9267bce8ed8aac70835)
FetchContent_MakeAvailable(metrics allocators)

find_package(Threads REQUIRED)
add_library(completion INTERFACE)
target_include_directories(completion INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(completion INTERFACE cxx_std_20)
target_link_libraries(completion INTERFACE Threads::Threads metrics::metrics allocators::resources)
add_library(completion::completion ALIAS completion)

# Optional Asio adapter; the host supplies standalone Asio. No fetch here.
add_library(completion_asio INTERFACE)
target_link_libraries(completion_asio INTERFACE completion::completion)
target_compile_definitions(completion_asio INTERFACE ASIO_STANDALONE)
if(ASIO_INCLUDE_DIR)
  target_include_directories(completion_asio INTERFACE "${ASIO_INCLUDE_DIR}")
endif()
add_library(completion::asio ALIAS completion_asio)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # Standalone demo: a durable batch may contain files from separate devices.
  add_executable(completion_demo examples/demo.cc)
  target_link_libraries(completion_demo PRIVATE completion::completion)
  include(CTest)
  # The platform probes behind docs/PLATFORM_FINDINGS.md: timing programs, not tests. The smoke
  # tests below run each once on a small input to show it builds and runs; they time nothing.
  option(COMPLETION_BUILD_BENCH "Build the platform probes in bench/" OFF)
  if(COMPLETION_BUILD_BENCH)
    foreach(p sync commit unlink dirflush)
      add_executable(completion_${p}_probe bench/${p}_probe.cc)
      target_link_libraries(completion_${p}_probe PRIVATE completion::completion)
    endforeach()
    if(BUILD_TESTING)
      set(probe_dir "${CMAKE_CURRENT_BINARY_DIR}/probe-state")
      add_test(NAME completion_probe_sync COMMAND completion_sync_probe "${probe_dir}" 3 2)
      add_test(NAME completion_probe_commit COMMAND completion_commit_probe "${probe_dir}" 2 2)
      add_test(NAME completion_probe_unlink COMMAND completion_unlink_probe "${probe_dir}" 48 16)
      add_test(NAME completion_probe_dirflush COMMAND completion_dirflush_probe "${probe_dir}" 3)
      set_tests_properties(completion_probe_sync completion_probe_commit completion_probe_unlink completion_probe_dirflush
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)
    endif()
  endif()
  if(BUILD_TESTING)
    foreach(t core allocation resource posix_resource scan inspection cursor_close reuse image_close create_into sync_files sync_calls map dir_lock dir_trust owner_lock)
      add_executable(completion_${t}_test test/${t}_test.cc)
      target_link_libraries(completion_${t}_test PRIVATE completion::completion)
    endforeach()
    # File tests take a state directory; sync_files also models independent device caches.
    foreach(t allocation resource)
      add_test(NAME completion_${t} COMMAND completion_${t}_test)
    endforeach()
    foreach(t core posix_resource scan inspection cursor_close reuse image_close create_into sync_files sync_calls map dir_lock dir_trust owner_lock)
      add_test(NAME completion_${t} COMMAND completion_${t}_test "${CMAKE_CURRENT_BINARY_DIR}/${t}-state")
    endforeach()
    # acquire_owner({}) must not compile; the spellings that still work must.
    add_test(NAME completion_owner_braced_refused COMMAND ${CMAKE_COMMAND} -DCXX=${CMAKE_CXX_COMPILER}
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/test/owner_braced_refused.cc
      "-DINCLUDES=$<JOIN:$<TARGET_PROPERTY:completion,INTERFACE_INCLUDE_DIRECTORIES>;$<TARGET_PROPERTY:metrics::metrics,INTERFACE_INCLUDE_DIRECTORIES>;$<TARGET_PROPERTY:allocators::resources,INTERFACE_INCLUDE_DIRECTORIES>,|>"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/test/owner_braced_check.cmake)
    # tools/mutants.py: every owner-lock mutant's anchor still occurs exactly once. Running the mutants
    # (`python3 tools/mutants.py`) rebuilds the suite 14 times and is not a test.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME completion_mutants_anchors COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/mutants.py --check)
    endif()
    # The directory lock again on tmpfs, where Linux has one (/dev/shm); the test checks it is.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND IS_DIRECTORY /dev/shm)
      string(MD5 dir_lock_id "${CMAKE_CURRENT_BINARY_DIR}")
      add_test(NAME completion_dir_lock_tmpfs COMMAND completion_dir_lock_test "/dev/shm/completion-dir-lock-${dir_lock_id}" tmpfs)
    endif()
    if(ASIO_INCLUDE_DIR)
      add_executable(completion_asio_extension_test test/asio_extension_test.cc)
      target_link_libraries(completion_asio_extension_test PRIVATE completion::asio)
      add_test(NAME completion_asio_extension COMMAND completion_asio_extension_test "${CMAKE_CURRENT_BINARY_DIR}/asio-extension-state")
    endif()
  endif()
endif()
