# PI Level Zero plugin library

list(APPEND SYCL_TOOLCHAIN_DEPLOY_COMPONENTS level-zero-loader level-zero-headers)

include_directories("${LEVEL_ZERO_INCLUDE_DIR}")

add_library (LevelZeroLoader-Headers INTERFACE)
add_library (LevelZeroLoader::Headers ALIAS LevelZeroLoader-Headers)
target_include_directories(LevelZeroLoader-Headers
  INTERFACE "${LEVEL_ZERO_INCLUDE_DIR}"
)


include_directories("${sycl_inc_dir}")

add_library(pi_level_zero SHARED
  "${sycl_inc_dir}/CL/sycl/detail/pi.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/pi_level_zero.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/pi_level_zero.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/usm_allocator.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/usm_allocator.hpp"
)

if (MSVC)
  # by defining __SYCL_BUILD_SYCL_DLL, we can use __declspec(dllexport)
  # which are individually tagged for all pi* symbols in pi.h
  target_compile_definitions(pi_level_zero PRIVATE __SYCL_BUILD_SYCL_DLL)
else()
  # we set the visibility of all symbols 'hidden' by default.
  # In pi.h file, we set exported symbols with visibility==default individually
  target_compile_options(pi_level_zero PUBLIC -fvisibility=hidden)

  # This script file is used to allow exporting pi* symbols only.
  # All other symbols are regarded as local (hidden)
  set(linker_script "${CMAKE_CURRENT_SOURCE_DIR}/../ld-version-script.txt")

  # Filter symbols based on the scope defined in the script file,
  # and export pi* function symbols in the library.
  target_link_libraries( pi_level_zero
    PRIVATE "-Wl,--version-script=${linker_script}"
  )
endif()

if (TARGET level-zero-loader)
  add_dependencies(pi_level_zero level-zero-loader)
endif()

add_dependencies(sycl-toolchain pi_level_zero)
target_link_libraries(pi_level_zero
  PRIVATE
    "${LEVEL_ZERO_LOADER}"
    OpenCL-Headers
)

if (UNIX)
  target_link_libraries(pi_level_zero PRIVATE pthread)
endif()

add_common_options(pi_level_zero)

install(TARGETS pi_level_zero
  LIBRARY DESTINATION "lib" COMPONENT pi_level_zero
  RUNTIME DESTINATION "bin" COMPONENT pi_level_zero)
