cmake_minimum_required(VERSION 3.16)
project(completion LANGUAGES CXX)

# Completion-based file I/O: native backends (io_uring on Linux, kqueue on BSD and macOS,
# a thread pool elsewhere), image publication, and directory scans. Header-only.
include(FetchContent)
FetchContent_Declare(metrics GIT_REPOSITORY https://github.com/Kronuz/metrics.git GIT_TAG 9db73de5c5401061ad5ee4829c8f13416754d8a3)
# Shared owning PMR allocation contexts, without global allocation overrides.
FetchContent_Declare(allocators GIT_REPOSITORY https://github.com/Kronuz/allocators.git GIT_TAG 07de45d180f23e2552b9504a6b4d506651594d1e)
FetchContent_MakeAvailable(metrics allocators)

find_package(Threads REQUIRED)
add_library(completion INTERFACE)
target_include_directories(completion INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(completion INTERFACE cxx_std_20)
target_link_libraries(completion INTERFACE Threads::Threads metrics::metrics allocators::resources)
add_library(completion::completion ALIAS completion)

# Optional coroutine seam (completion::task): completion_ref, InlineCompletion and HeldCompletion
# over task.hh alone. completion::asio adds the Asio driver. The core above needs neither. Both
# fetch task, which fetches Asio only for completion::asio, unless ASIO_INCLUDE_DIR names a checkout.
set(completion_top_level OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(completion_top_level ON)
endif()
option(COMPLETION_TASK "Build completion::task, the seam over task::task" ${completion_top_level})
option(COMPLETION_ASIO "Build completion::asio, the task::task driver" ${completion_top_level})
if(COMPLETION_ASIO OR COMPLETION_TASK)
  if(NOT DEFINED TASK_ASIO)
    set(TASK_ASIO ${COMPLETION_ASIO})
  endif()
  FetchContent_Declare(task GIT_REPOSITORY https://github.com/Kronuz/task.git GIT_TAG 95798a02f68c86f070401329a05c78d326ac2579)
  FetchContent_MakeAvailable(task)
  add_library(completion_task INTERFACE)
  target_link_libraries(completion_task INTERFACE completion::completion task::task)
  add_library(completion::task ALIAS completion_task)
endif()
if(COMPLETION_ASIO)
  add_library(completion_asio INTERFACE)
  target_link_libraries(completion_asio INTERFACE completion::task task::asio)
  add_library(completion::asio ALIAS completion_asio)
endif()

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
    if(COMPLETION_ASIO)
      add_executable(completion_lane_probe bench/lane_probe.cc)
      target_link_libraries(completion_lane_probe PRIVATE completion::asio)
    endif()
    if(BUILD_TESTING)
      set(probe_dir "${CMAKE_CURRENT_BINARY_DIR}/probe-state")
      add_test(NAME completion_probe_sync COMMAND completion_sync_probe "${probe_dir}" 3 2)
      add_test(NAME completion_probe_commit COMMAND completion_commit_probe "${probe_dir}" 2 2)
      add_test(NAME completion_probe_unlink COMMAND completion_unlink_probe "${probe_dir}" 48 16)
      add_test(NAME completion_probe_dirflush COMMAND completion_dirflush_probe "${probe_dir}" 3)
      set_tests_properties(completion_probe_sync completion_probe_commit completion_probe_unlink completion_probe_dirflush
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)
      if(COMPLETION_ASIO)
        add_test(NAME completion_probe_lane COMMAND completion_lane_probe "${probe_dir}" 2 3 16)
        set_tests_properties(completion_probe_lane PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)
      endif()
    endif()
  endif()
  if(BUILD_TESTING)
    foreach(t core allocation resource posix_resource scan inspection cursor_close reuse image_close create_into sync_files sync_calls sync_primitives sync_data sync_seam map dir_lock dir_trust owner_lock)
      add_executable(completion_${t}_test test/${t}_test.cc)
      target_link_libraries(completion_${t}_test PRIVATE completion::completion)
    endforeach()
    # File tests take a state directory; sync_files also models independent device caches.
    foreach(t allocation resource)
      add_test(NAME completion_${t} COMMAND completion_${t}_test)
    endforeach()
    foreach(t core posix_resource scan inspection cursor_close reuse image_close create_into sync_files sync_calls sync_primitives sync_data sync_seam map dir_lock dir_trust owner_lock)
      add_test(NAME completion_${t} COMMAND completion_${t}_test "${CMAKE_CURRENT_BINARY_DIR}/${t}-state")
    endforeach()
    # acquire_owner({}) must not compile; the spellings that still work must.
    add_test(NAME completion_owner_braced_refused COMMAND ${CMAKE_COMMAND} -DCXX=${CMAKE_CXX_COMPILER}
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/test/owner_braced_refused.cc
      "-DINCLUDES=$<JOIN:$<TARGET_PROPERTY:completion,INTERFACE_INCLUDE_DIRECTORIES>;$<TARGET_PROPERTY:metrics::metrics,INTERFACE_INCLUDE_DIRECTORIES>;$<TARGET_PROPERTY:allocators::resources,INTERFACE_INCLUDE_DIRECTORIES>,|>"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/test/owner_braced_check.cmake)
    # tools/mutants.py: every owner-lock mutant's anchor still occurs exactly once. Running the mutants
    # (`python3 tools/mutants.py`) rebuilds the suite once per mutant and is not a test.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME completion_mutants_anchors COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/mutants.py --check)
      # drive_synchronously stays the one synchronous loop (the seam's shape, by grep).
      add_test(NAME completion_seam_shape COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/check_seam.py)
    endif()
    # The directory lock again on tmpfs, where Linux has one (/dev/shm); the test checks it is.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND IS_DIRECTORY /dev/shm)
      string(MD5 dir_lock_id "${CMAKE_CURRENT_BINARY_DIR}")
      add_test(NAME completion_dir_lock_tmpfs COMMAND completion_dir_lock_test "/dev/shm/completion-dir-lock-${dir_lock_id}" tmpfs)
    endif()
    if(COMPLETION_TASK)
      add_executable(completion_seam_test test/seam_test.cc)
      target_link_libraries(completion_seam_test PRIVATE completion::task)
      add_test(NAME completion_seam COMMAND completion_seam_test)
    endif()
    if(COMPLETION_ASIO)
      add_executable(completion_asio_extension_test test/asio_extension_test.cc)
      target_link_libraries(completion_asio_extension_test PRIVATE completion::asio)
      add_test(NAME completion_asio_extension COMMAND completion_asio_extension_test "${CMAKE_CURRENT_BINARY_DIR}/asio-extension-state")
      add_executable(completion_asio_seam_test test/asio_seam_test.cc)
      target_link_libraries(completion_asio_seam_test PRIVATE completion::asio)
      add_test(NAME completion_asio_seam COMMAND completion_asio_seam_test)
    endif()
  endif()
endif()
