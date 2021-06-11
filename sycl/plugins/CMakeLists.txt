set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang" )
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-covered-switch-default")
endif()

if(SYCL_BUILD_PI_CUDA)
  add_subdirectory(cuda)
endif()

if(MSVC)
  set(LEVEL_ZERO_LOADER
      "${LLVM_LIBRARY_OUTPUT_INTDIR}/${CMAKE_STATIC_LIBRARY_PREFIX}ze_loader${CMAKE_STATIC_LIBRARY_SUFFIX}")
else()
  set(LEVEL_ZERO_LOADER
      "${LLVM_LIBRARY_OUTPUT_INTDIR}/${CMAKE_SHARED_LIBRARY_PREFIX}ze_loader${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()

if (NOT DEFINED LEVEL_ZERO_LIBRARY OR NOT DEFINED LEVEL_ZERO_INCLUDE_DIR)
  include(ExternalProject)
  message(STATUS "Download Level Zero loader and headers from github.com")
  if (CMAKE_C_COMPILER)
    list(APPEND AUX_CMAKE_FLAGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
  endif()
  if (CMAKE_CXX_COMPILER)
    list(APPEND AUX_CMAKE_FLAGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
  endif()
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/level_zero_loader_build)
  set(LEVEL_ZERO_LOADER_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/level_zero/level_zero_loader")
  if (NOT DEFINED SYCL_EP_LEVEL_ZERO_LOADER_SKIP_AUTO_UPDATE)
    set(SYCL_EP_LEVEL_ZERO_LOADER_SKIP_AUTO_UPDATE ${SYCL_EXTERNAL_PROJECTS_SKIP_AUTO_UPDATE})
  endif()

  #TODO: Replace ExternalProject with FetchContent for better maintainance and
  #      cmake files simplification
  include(ExternalProject)
  set(LEVEL_ZERO_LOADER_SOURCE_VERSION v1.2.3)
  ExternalProject_Add(level-zero-loader
    GIT_REPOSITORY    https://github.com/oneapi-src/level-zero.git
    GIT_TAG           ${LEVEL_ZERO_LOADER_SOURCE_VERSION}
    UPDATE_DISCONNECTED ${SYCL_EP_LEVEL_ZERO_LOADER_SKIP_AUTO_UPDATE}
    SOURCE_DIR        ${LEVEL_ZERO_LOADER_SOURCE_DIR}
    BINARY_DIR        "${CMAKE_CURRENT_BINARY_DIR}/level_zero_loader_build"
    INSTALL_DIR       "${CMAKE_CURRENT_BINARY_DIR}/level_zero_loader_install"
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
               -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
               -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
               -DCMAKE_INSTALL_LIBDIR:PATH=lib${LLVM_LIBDIR_SUFFIX}
               ${AUX_CMAKE_FLAGS}
               LOG_DOWNLOAD 1
               LOG_UPDATE 1
               LOG_CONFIGURE 1
               LOG_BUILD 1
               LOG_INSTALL 1
    STEP_TARGETS      configure,build,install
    DEPENDS           OpenCL-Headers
    BUILD_BYPRODUCTS ${LEVEL_ZERO_LOADER}
  )
  ExternalProject_Add_Step(level-zero-loader llvminstall
    COMMAND ${CMAKE_COMMAND} -E copy_directory <INSTALL_DIR>/lib/ ${LLVM_BINARY_DIR}/lib
    COMMAND ${CMAKE_COMMAND} -E copy_directory <INSTALL_DIR>/include/ ${LLVM_BINARY_DIR}/include/sycl
    COMMENT "Installing level-zero-loader into the LLVM binary directory"
    DEPENDEES install
  )

  install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/level_zero_loader_install/"
    DESTINATION "."
    COMPONENT level-zero-loader
  )

  set(LEVEL_ZERO_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/level_zero_loader_install/include/
	  CACHE INTERNAL "Path containing Level_Zero header files.")
else()
  file(GLOB LEVEL_ZERO_LIBRARY_SRC "${LEVEL_ZERO_LIBRARY}*")
  get_filename_component(LEVEL_ZERO_LIB_NAME ${LEVEL_ZERO_LIBRARY} NAME)
  add_custom_target(level-zero-loader
      DEPENDS
	  ${LLVM_LIBRARY_OUTPUT_INTDIR}/${LEVEL_ZERO_LIB_NAME}
	  ${LLVM_BINARY_DIR}/include/sycl/level_zero
  )
  add_custom_command(
      OUTPUT
	  ${LLVM_LIBRARY_OUTPUT_INTDIR}/${LEVEL_ZERO_LIB_NAME}
	  ${LLVM_BINARY_DIR}/include/sycl/level_zero
      COMMENT
	  "Copying Level Zero loader and headers"
      COMMAND
	  ${CMAKE_COMMAND} -E copy ${LEVEL_ZERO_LIBRARY_SRC} ${LLVM_LIBRARY_OUTPUT_INTDIR}
      COMMAND
	  ${CMAKE_COMMAND} -E copy_directory ${LEVEL_ZERO_INCLUDE_DIR} ${LLVM_BINARY_DIR}/include/sycl
      DEPENDS
	  ${LEVEL_ZERO_LIBRARY}
          ${LEVEL_ZERO_INCLUDE_DIR}
  )
endif()

set(LEVEL_ZERO_LIBRARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/level_zero_loader_install/lib/)

add_subdirectory(opencl)
add_subdirectory(level_zero)

if (SYCL_BUILD_PI_ESIMD_CPU)
  add_subdirectory(esimd_cpu)
endif()
