
# PI Esimd CPU  library
# Create Shared library for libpi_esimd_cpu.so.

include_directories("${sycl_inc_dir}")
include_directories(${OpenCL_INCLUDE_DIR})
include_directories(${LIBCMRT_INCLUDE})

add_library(pi_esimd_cpu SHARED
  "${sycl_inc_dir}/CL/sycl/detail/pi.h"
  "pi_esimd_cpu.cpp"
)

add_dependencies(sycl-toolchain pi_esimd_cpu)

add_dependencies(pi_esimd_cpu
  OpenCL-Headers)

target_link_libraries(pi_esimd_cpu PRIVATE sycl)
set_target_properties(pi_esimd_cpu PROPERTIES LINKER_LANGUAGE CXX)

add_common_options(pi_esimd_cpu)

install(TARGETS pi_esimd_cpu
    LIBRARY DESTINATION "lib${LLVM_LIBDIR_SUFFIX}" COMPONENT pi_esimd_cpu
    RUNTIME DESTINATION "bin" COMPONENT pi_esimd_cpu)
