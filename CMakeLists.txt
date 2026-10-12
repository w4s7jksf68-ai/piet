cmake_minimum_required(VERSION 3.20)
project(cecon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(WIN32)
  # MinGW's unsuffixed stat/off_t ABI is otherwise 32-bit. Keep .hbin sizes
  # valid above 2 GiB; the separately-built mmap shim receives the same define
  # in docker/Dockerfile.windows.
  add_compile_definitions(_FILE_OFFSET_BITS=64)
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Default optimization. On Apple Silicon we can safely use -march=native (we
# only run on this Mac). On Linux we use a portable baseline so binaries built
# in a container run on any modern cloud CPU. The non-Apple x86 -march is a cache
# variable so a downstream build (e.g. Porting's docker/) can retarget the
# baseline without editing this src-governed file — e.g. -DCECON_X86_ARCH=x86-64
# for a no-AVX2 box. (The Apple/aarch64 paths are unaffected.)
set(CECON_X86_ARCH "x86-64-v3" CACHE STRING
    "x86 -march baseline for non-Apple builds (override e.g. -DCECON_X86_ARCH=x86-64)")
if(APPLE)
  add_compile_options(-O3 -march=native -Wall -Wextra -Wno-deprecated-declarations)
else()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    add_compile_options(-O3 -march=${CECON_X86_ARCH} -mtune=generic
                        -Wall -Wextra -Wno-deprecated-declarations)
  else()  # aarch64 / arm64 / other
    add_compile_options(-O3 -mtune=generic
                        -Wall -Wextra -Wno-deprecated-declarations)
  endif()
endif()

# BLAS/LAPACK backend: Apple Accelerate on macOS, OpenBLAS + LAPACK on Linux.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(APPLE)
  find_library(ACCELERATE_LIB Accelerate REQUIRED)
  set(CECON_LA_LIBS ${ACCELERATE_LIB})
  set(CECON_LA_DEFS ACCELERATE_NEW_LAPACK=1)
elseif(CECON_STATIC_LIBS)
  # Caller (typically the Docker static build) passed full .a archive paths
  # already including OpenBLAS / LAPACKE / gfortran / quadmath. Add system
  # dynamic deps that always live on a glibc box.
  set(CECON_LA_LIBS ${CECON_STATIC_LIBS})
  if(UNIX)
    list(APPEND CECON_LA_LIBS -ldl)  # libdl is glibc-only; MinGW-w64 (WIN32 cross) has none
  endif()
  set(CECON_LA_DEFS "")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
else()
  # Linux dynamic-link path (system OpenBLAS via .so).
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(OPENBLAS QUIET openblas)
  endif()
  if(OPENBLAS_FOUND)
    set(CECON_LA_LIBS ${OPENBLAS_LIBRARIES})
    include_directories(${OPENBLAS_INCLUDE_DIRS})
  else()
    find_library(OPENBLAS_LIB NAMES openblas REQUIRED)
    set(CECON_LA_LIBS ${OPENBLAS_LIB})
  endif()
  find_library(LAPACKE_LIB NAMES lapacke)
  if(LAPACKE_LIB)
    list(APPEND CECON_LA_LIBS ${LAPACKE_LIB})
  endif()
  set(CECON_LA_DEFS "")
endif()

add_library(cecon_core STATIC
  src/io/columnar.cpp
  src/io/dta_reader.cpp
  src/io/dta_writer.cpp
  src/data/frame.cpp
  src/data/frame2.cpp
  src/data/datasignature.cpp
  src/core/linalg.cpp
  src/estimators/aer_smooth.cpp
  src/estimators/oaxaca.cpp
  src/estimators/nnmatch.cpp
  src/estimators/pca.cpp
  src/core/matrix.cpp
  src/core/model.cpp
  src/core/threads.cpp
  src/hdfe/demean.cpp
  src/hdfe/dof.cpp
  src/vcov/vcov.cpp
  src/estimators/ols.cpp
  src/estimators/feols.cpp
  src/estimators/fepois.cpp
  src/estimators/logit.cpp
  src/estimators/probit.cpp
  src/estimators/csdid.cpp
  src/estimators/xtpois.cpp
  src/estimators/xtreg.cpp
  src/estimators/nbreg.cpp
  src/estimators/xtlogit.cpp
  src/estimators/posthest.cpp
  src/estimators/iv2sls.cpp
  src/estimators/qreg.cpp
  src/inference/wildboot.cpp
  src/estimators/oglm.cpp
  src/estimators/mlogit.cpp
  src/estimators/zicount.cpp
  src/estimators/ivml.cpp
  src/estimators/mixedlm.cpp
  src/estimators/survival.cpp
  src/estimators/varmodel.cpp
  src/estimators/xtabond.cpp
  src/estimators/nlgmm.cpp
  src/estimators/miimpute.cpp
  src/vcov/svy_vcov.cpp
  src/estimators/rdlocal.cpp
  src/estimators/sunab.cpp
  src/estimators/didimpute.cpp
  src/estimators/sdid.cpp
)
target_include_directories(cecon_core PUBLIC src vendor)
if(CECON_LA_DEFS)
  target_compile_definitions(cecon_core PUBLIC ${CECON_LA_DEFS})
endif()
target_link_libraries(cecon_core PUBLIC ${CECON_LA_LIBS} Threads::Threads)

add_executable(cobra apps/cecon_cli.cpp apps/cmds/common.cpp apps/cmds/options_table.cpp apps/cmds/cmds_linear.cpp apps/cmds/cmds_panel.cpp apps/cmds/cmds_mle.cpp apps/cmds/cmds_postest.cpp apps/cmds/cmds_diag.cpp apps/cmds/cmds_ts.cpp apps/cmds/cmds_summary.cpp apps/cmds/cmds_data.cpp apps/cmds/cmds_survey.cpp apps/cmds/cmds_oglm.cpp apps/cmds/cmds_mlogit.cpp apps/cmds/cmds_zicount.cpp apps/cmds/cmds_ivml.cpp apps/cmds/cmds_mixedlm.cpp apps/cmds/cmds_survival.cpp apps/cmds/cmds_varmodel.cpp apps/cmds/cmds_xtabond.cpp apps/cmds/cmds_nlgmm.cpp apps/cmds/cmds_miimpute.cpp apps/cmds/cmds_rd.cpp apps/cmds/cmds_eventstudy.cpp apps/cmds/cmds_sdid.cpp apps/cmds/cmds_binscatter.cpp apps/cmds/cmds_aer_estim.cpp apps/cmds/cmds_oaxaca.cpp apps/cmds/cmds_nnmatch.cpp)
target_link_libraries(cobra PRIVATE cecon_core)
target_compile_definitions(cobra PRIVATE COBRA_VERSION="0.41.0")

include(CTest)
if(BUILD_TESTING)
  foreach(test_name morsel_determinism thread_determinism threads_budget)
    add_executable(test_${test_name} tests/unit/test_${test_name}.cpp)
    target_link_libraries(test_${test_name} PRIVATE cecon_core)
    add_test(NAME ${test_name} COMMAND test_${test_name})
  endforeach()
endif()
