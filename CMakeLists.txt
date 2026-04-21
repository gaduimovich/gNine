cmake_minimum_required(VERSION 3.2)
project(GNINE)


macro(copy_files TARGET_NAME GLOBPAT DESTINATION)
  file(GLOB COPY_FILES
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${GLOBPAT})
  add_custom_target(${TARGET_NAME} ALL
    COMMENT "Copying files: ${GLOBPAT}")

  foreach(FILENAME ${COPY_FILES})
    set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/${FILENAME}")
    set(DST "${DESTINATION}/${FILENAME}")

    add_custom_command(
      TARGET ${TARGET_NAME}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy ${SRC} ${DST}
      )
  endforeach(FILENAME)
endmacro(copy_files)

list(APPEND CMAKE_MODULE_PATH
cmake/Modules
${CMAKE_CURRENT_LIST_DIR}/libs/omr/cmake/modules/
)

if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    set(CMAKE_SYSTEM_PROCESSOR arm64)
  elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
  endif()
endif()

include(OmrPlatform)
include(FindPkgConfig)
include(CheckCXXSourceCompiles)

# Global Configuration

omr_platform_global_setup()

set(CMAKE_CXX_STANDARD 11)

set(CMAKE_EXPORT_COMPILE_COMMANDS true)

option(GNINE_ENABLE_SDL_PREVIEW "Enable SDL2-backed runtime preview support when SDL2 is link-compatible" ON)

enable_testing()

# OMR Configuration

set(OMR_COMPILER   ON  CACHE INTERNAL "Enable the Compiler.")
set(OMR_JITBUILDER ON  CACHE INTERNAL "We use OMR's jitbuilder tool for the b9 JIT")
set(OMR_GC         ON CACHE INTERNAL "We don't use the GC in b9 (yet)")
set(OMR_FVTEST     OFF CACHE INTERNAL "Disable OMR's internal test suite, it's incompatible with b9")
set(OMR_JITBUILDER_ADDITIONAL ON "Turn tests on")
set(OMR_WARNINGS_AS_ERRORS OFF CACHE INTERNAL "OMR doesn't compile cleanly on my laptop :p")


add_subdirectory(libs)
add_subdirectory(examples)
add_subdirectory(example_data)

include_directories(${GNINE_SOURCE_DIR}/src ${GNINE_SOURCE_DIR}/libs/stb_image)

# aux_source_directory(src ${SOURCES})
add_library(gnine_core src/Image.cpp src/Parser.cpp src/ImageArray.cpp src/VectorProgram.cpp src/Runtime.cpp)
target_include_directories(gnine_core PRIVATE
  ${GNINE_SOURCE_DIR}/libs/omr
  ${GNINE_SOURCE_DIR}/libs/omr/jitbuilder
  ${GNINE_SOURCE_DIR}/libs/omr/compiler
)
target_link_libraries(gnine_core PUBLIC stb_image jitbuilder)

add_executable(gnine src/main.cpp)
target_link_libraries(gnine PUBLIC gnine_core)

if(GNINE_ENABLE_SDL_PREVIEW AND PKG_CONFIG_FOUND)
  pkg_check_modules(SDL2 QUIET sdl2)
  if(SDL2_FOUND)
    list(JOIN SDL2_CFLAGS_OTHER " " GNINE_SDL2_REQUIRED_FLAGS)
    set(CMAKE_REQUIRED_INCLUDES ${SDL2_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_FLAGS "${GNINE_SDL2_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_LIBRARIES ${SDL2_LDFLAGS})
    check_cxx_source_compiles(
      "#include <SDL.h>\nint main() { return SDL_InitSubSystem(SDL_INIT_VIDEO); }"
      GNINE_CAN_LINK_SDL2)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LIBRARIES)

    if(GNINE_CAN_LINK_SDL2)
      target_include_directories(gnine PRIVATE ${SDL2_INCLUDE_DIRS})
      target_compile_options(gnine PRIVATE ${SDL2_CFLAGS_OTHER})
      target_link_libraries(gnine PUBLIC ${SDL2_LDFLAGS})
      target_compile_definitions(gnine PRIVATE GNINE_HAVE_SDL2=1)
    else()
      message(WARNING "SDL2 was found but is not link-compatible with this toolchain; building without preview support.")
    endif()
  endif()
elseif(NOT GNINE_ENABLE_SDL_PREVIEW)
  message(STATUS "GNINE SDL preview support disabled by configuration.")
endif()

add_subdirectory(tests)
