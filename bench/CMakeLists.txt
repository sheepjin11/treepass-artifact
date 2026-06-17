# Ensure the bench module uses C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_library(SNAPPY
  NAMES libsnappy.so
  HINTS /usr/lib/x86_64-linux-gnu
  REQUIRED)

find_library(AIO_LIB aio REQUIRED)
find_package(TBB REQUIRED)

include_directories(./common ./interface)

add_executable(random_mixed_bench random_mixed_bench.cpp)
target_link_libraries(random_mixed_bench snappy)
target_link_libraries(random_mixed_bench gflags)
target_link_libraries(random_mixed_bench pthread)
target_link_libraries(random_mixed_bench ${AIO_LIB})
target_link_libraries(random_mixed_bench TBB::tbb)
target_include_directories(random_mixed_bench PUBLIC ${global_INCLUDE_DIR})

set(treepass_INCLUDE_DIR ${treepass_SOURCE_DIR}/include)
set(treepass_SRC_DIR ${treepass_SOURCE_DIR}/src)
set(treepass_PLATFORM_DIR ${treepass_SOURCE_DIR}/src/platform_linux)

target_include_directories(random_mixed_bench PUBLIC
    ${treepass_INCLUDE_DIR}
    ${treepass_SRC_DIR}
    ${treepass_PLATFORM_DIR}
)

if(DEFINED ENV{BUILD_ROOT})
  set(_treepass_build_root $ENV{BUILD_ROOT})
else()
  set(_treepass_build_root ${treepass_SOURCE_DIR}/build)
endif()

# TreePass Makefile lays out build/<MODE>[-asan][-ubsan]/lib/libsplinterdb.a
if("${BUILD_MODE}" STREQUAL "debug")
  set(_treepass_build_subdir "debug")
else()
  set(_treepass_build_subdir "release")
endif()
if(BUILD_ASAN AND NOT "${BUILD_ASAN}" STREQUAL "0")
  set(_treepass_build_subdir "${_treepass_build_subdir}-asan")
endif()
if(BUILD_UBSAN AND NOT "${BUILD_UBSAN}" STREQUAL "0")
  set(_treepass_build_subdir "${_treepass_build_subdir}-ubsan")
endif()

set(treepass_LIBRARY ${_treepass_build_root}/${_treepass_build_subdir}/lib/libsplinterdb.a)
message(STATUS "TreePass library: ${treepass_LIBRARY}")
message(STATUS "TreePass include: ${treepass_INCLUDE_DIR}")

target_link_libraries(random_mixed_bench ${treepass_LIBRARY})
target_link_libraries(random_mixed_bench m pthread aio xxhash)

if("${BUILD_ASAN}" STREQUAL "1")
  target_link_options(random_mixed_bench PUBLIC -fsanitize=address)
endif()
if("${BUILD_UBSAN}" STREQUAL "1")
  target_link_options(random_mixed_bench PUBLIC -fsanitize=undefined)
endif()
