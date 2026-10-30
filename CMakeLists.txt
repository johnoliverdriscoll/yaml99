cmake_minimum_required(VERSION 3.22)

project(yaml99
  VERSION 0.1.0
  LANGUAGES C
)

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(YAML99_BUILD_TESTING "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(YAML99_WITH_HEAP "Build allocating string getters" ON)
option(YAML99_USE_SYSTEM_DEPENDENCIES "Find installed sio99 and utf99" OFF)

if(NOT TARGET sio99)
  if(YAML99_USE_SYSTEM_DEPENDENCIES)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SIO99 REQUIRED IMPORTED_TARGET GLOBAL sio99)
    add_library(sio99 ALIAS PkgConfig::SIO99)
    foreach(adapter heap stdio)
      find_library(YAML99_SIO99_${adapter}_LIBRARY
        NAMES sio99_${adapter}
        HINTS ${SIO99_LIBRARY_DIRS}
      )
      if(YAML99_SIO99_${adapter}_LIBRARY)
        add_library(sio99_${adapter} UNKNOWN IMPORTED GLOBAL)
        set_target_properties(sio99_${adapter} PROPERTIES
          IMPORTED_LOCATION "${YAML99_SIO99_${adapter}_LIBRARY}"
          INTERFACE_LINK_LIBRARIES sio99
        )
      endif()
    endforeach()
  else()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/sio99/CMakeLists.txt")
      message(FATAL_ERROR
        "The sio99 submodule is missing. Run: "
        "git submodule update --init --recursive"
      )
    endif()
    function(yaml_add_sio99)
      if(NOT DEFINED SIO99_BUILD_TESTING)
        set(SIO99_BUILD_TESTING OFF)
      endif()
      if(NOT DEFINED SIO99_WITH_HEAP)
        set(SIO99_WITH_HEAP ${YAML99_WITH_HEAP})
      endif()
      add_subdirectory(src/sio99)
    endfunction()
    yaml_add_sio99()
  endif()
endif()

if(NOT TARGET utf99)
  if(YAML99_USE_SYSTEM_DEPENDENCIES)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(UTF99 REQUIRED IMPORTED_TARGET GLOBAL utf99)
    add_library(utf99 ALIAS PkgConfig::UTF99)
  else()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/utf99/CMakeLists.txt")
      message(FATAL_ERROR
        "The utf99 submodule is missing. Run: "
        "git submodule update --init --recursive"
      )
    endif()
    function(yaml_add_utf99)
      if(NOT DEFINED UTF99_BUILD_TESTING)
        set(UTF99_BUILD_TESTING OFF)
      endif()
      add_subdirectory(src/utf99)
    endfunction()
    yaml_add_utf99()
  endif()
endif()

if(YAML99_WITH_HEAP AND NOT TARGET sio99_heap)
  message(FATAL_ERROR
    "YAML99_WITH_HEAP requires sio99_heap. Enable SIO99_WITH_HEAP, "
    "install the sio99 heap adapter, or disable YAML99_WITH_HEAP."
  )
endif()

function(yaml_target_defaults target)

  set_target_properties(${target}
    PROPERTIES
      C_STANDARD 99
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF
  )

  target_include_directories(${target} PUBLIC include)

  target_compile_options(${target}
    PRIVATE
      "$<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:"
        "-Werror;"
        "-Wpedantic;"
        "-Wall;"
        "-Wextra;"
        "-Werror=vla;"
        "-Werror=alloca;"
        "-Werror=implicit-function-declaration;"
        "-Wstrict-prototypes;"
        "-Wmissing-prototypes;"
        "-fno-fast-math;"
        "-ffp-contract=off;"
      ">"
  )

endfunction()

add_library(yaml99
  src/yaml99/yaml.c
)
yaml_target_defaults(yaml99)
target_link_libraries(yaml99 PUBLIC sio99 utf99)
set_target_properties(yaml99
  PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION 0
)

if(YAML99_WITH_HEAP)
  add_library(yaml99_heap
    src/yaml99/yaml_heap.c
  )
  yaml_target_defaults(yaml99_heap)
  target_link_libraries(yaml99_heap
    PUBLIC
      yaml99
      sio99_heap
  )
  set_target_properties(yaml99_heap
    PROPERTIES
      VERSION "${PROJECT_VERSION}"
      SOVERSION 0
  )
endif()

include(GNUInstallDirs)

install(
  TARGETS yaml99
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
  FILES
    include/yaml99/yaml.h
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/yaml99"
)

if(YAML99_WITH_HEAP)
  install(
    TARGETS yaml99_heap
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  )
endif()

set(YAML99_PKGCONFIG_LIBS "")
if(YAML99_WITH_HEAP)
  string(APPEND YAML99_PKGCONFIG_LIBS "-lyaml99_heap ")
endif()
string(APPEND YAML99_PKGCONFIG_LIBS "-lyaml99")

configure_file(yaml99.pc.in yaml99.pc @ONLY)

install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/yaml99.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)

if(YAML99_BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()
