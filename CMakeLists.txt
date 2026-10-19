cmake_minimum_required(VERSION 3.16)
project(completion LANGUAGES CXX)

# Completion-based file I/O: native backends (io_uring on Linux, kqueue on BSD and macOS,
# a thread pool elsewhere), image publication, and directory scans. Header-only.
include(FetchContent)
FetchContent_Declare(metrics GIT_REPOSITORY https://github.com/Kronuz/metrics.git GIT_TAG c975765bafe61fa79a0aa417be6deb30edf3b1db)
# Shared owning PMR allocation contexts, without global allocation overrides.
FetchContent_Declare(allocators GIT_REPOSITORY https://github.com/Kronuz/allocators.git GIT_TAG b7daa52f1b6a316e04bba2817f0f527d64d704eb)
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
  if(BUILD_TESTING)
    foreach(t core allocation resource posix_resource scan inspection cursor_close reuse image_close create_into sync_files map dir_lock dir_trust)
      add_executable(completion_${t}_test test/${t}_test.cc)
      target_link_libraries(completion_${t}_test PRIVATE completion::completion)
    endforeach()
    # File tests take a state directory; sync_files also models independent device caches.
    foreach(t allocation resource)
      add_test(NAME completion_${t} COMMAND completion_${t}_test)
    endforeach()
    foreach(t core posix_resource scan inspection cursor_close reuse image_close create_into sync_files map dir_lock dir_trust)
      add_test(NAME completion_${t} COMMAND completion_${t}_test "${CMAKE_CURRENT_BINARY_DIR}/${t}-state")
    endforeach()
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
