cmake_minimum_required(VERSION 3.21)

project(texlink VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(TEXLINK_BUILD_SHARED_LIBS "Build texlink as a shared library" OFF)
option(TEXLINK_BUILD_EXAMPLES "Build example programs" ON)
option(TEXLINK_WITH_EGL "Build the EGL/OpenGL texlink backend utility" OFF)
option(TEXLINK_WITH_VULKAN "Build the Vulkan texlink backend utility" OFF)
option(TEXLINK_WITH_GBM "Build the Linux GBM/dma-buf allocator" OFF)

set(TEXLINK_CORE_SOURCES src/session.c)
set(TEXLINK_CORE_PRIVATE_INCLUDES src)
set(TEXLINK_CORE_PRIVATE_LIBS)
set(TEXLINK_CORE_PUBLIC_LIBS)
set(TEXLINK_CORE_PRIVATE_COMPILE_OPTIONS)

if(TEXLINK_BUILD_SHARED_LIBS)
  set(TEXLINK_LIBRARY_TYPE SHARED)
else()
  set(TEXLINK_LIBRARY_TYPE STATIC)
endif()

if(WIN32)
  list(APPEND TEXLINK_CORE_SOURCES src/buffer_win32.c src/socket_win32.c
       src/sync_win32.c src/registry_win32.c src/platform_win32.c)
else()
  list(APPEND TEXLINK_CORE_SOURCES src/socket_posix.c src/sync_posix.c
       src/registry_posix.c src/platform_posix.c)
  list(APPEND TEXLINK_CORE_PUBLIC_LIBS rt)
  if(TEXLINK_WITH_GBM)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GBM REQUIRED gbm)
    list(APPEND TEXLINK_CORE_SOURCES src/buffer_posix.c)
    list(APPEND TEXLINK_CORE_PRIVATE_INCLUDES ${GBM_INCLUDE_DIRS})
    list(APPEND TEXLINK_CORE_PUBLIC_LIBS ${GBM_LIBRARIES})
    list(APPEND TEXLINK_CORE_PRIVATE_COMPILE_OPTIONS ${GBM_CFLAGS_OTHER})
  else()
    list(APPEND TEXLINK_CORE_SOURCES src/buffer_posix_shm.c)
  endif()
endif()

add_library(texlink ${TEXLINK_LIBRARY_TYPE} ${TEXLINK_CORE_SOURCES})

if(TEXLINK_BUILD_SHARED_LIBS)
    set_target_properties(texlink PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
    if(WIN32)
      set_target_properties(texlink PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endif()
endif()

target_include_directories(
  texlink
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${TEXLINK_CORE_PRIVATE_INCLUDES})

target_link_libraries(texlink PUBLIC ${TEXLINK_CORE_PUBLIC_LIBS}
                             PRIVATE ${TEXLINK_CORE_PRIVATE_LIBS})

if(MSVC)
  target_compile_definitions(texlink PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(texlink PRIVATE /W4 /experimental:c11atomics)
else()
  target_compile_options(texlink PRIVATE -Wall -Wextra -Wpedantic
                                         ${TEXLINK_CORE_PRIVATE_COMPILE_OPTIONS})
endif()

set(TEXLINK_INSTALL_TARGETS texlink)
set(TEXLINK_INSTALL_HEADERS include/texlink.h)

if(NOT WIN32 AND TEXLINK_BUILD_EXAMPLES AND (TEXLINK_WITH_EGL OR TEXLINK_WITH_VULKAN))
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLFW3 glfw3)
endif()

if(NOT WIN32 AND TEXLINK_WITH_EGL)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(EGL egl)
  find_package(OpenGL COMPONENTS OpenGL)
  find_package(GLEW)
endif()

if(TEXLINK_WITH_VULKAN)
  find_package(Vulkan)
endif()

if(TEXLINK_WITH_EGL
   AND EGL_FOUND
   AND OpenGL_OpenGL_FOUND
   AND GLEW_FOUND)
  add_library(texlink_egl src/egl.c)
  target_link_libraries(
    texlink_egl
    PUBLIC texlink ${EGL_LIBRARIES} OpenGL::OpenGL
    PRIVATE GLEW::GLEW)
  target_include_directories(
    texlink_egl
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>
    PRIVATE ${EGL_INCLUDE_DIRS})
  target_compile_options(texlink_egl PRIVATE -Wall -Wextra -Wpedantic
                                             ${EGL_CFLAGS_OTHER})
  list(APPEND TEXLINK_INSTALL_TARGETS texlink_egl)
  list(APPEND TEXLINK_INSTALL_HEADERS include/texlink_egl.h)
endif()

if(TEXLINK_WITH_VULKAN AND Vulkan_FOUND)
  add_library(texlink_vulkan src/vulkan.c)
  target_link_libraries(texlink_vulkan PUBLIC texlink Vulkan::Vulkan)
  target_include_directories(
    texlink_vulkan PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                          $<INSTALL_INTERFACE:include> ${Vulkan_INCLUDE_DIRS})
  target_compile_options(texlink_vulkan PRIVATE -Wall -Wextra -Wpedantic)
  list(APPEND TEXLINK_INSTALL_TARGETS texlink_vulkan)
  list(APPEND TEXLINK_INSTALL_HEADERS include/texlink_vulkan.h)
endif()

if(TEXLINK_BUILD_EXAMPLES)
  foreach(example simple_producer simple_consumer)
    add_executable(${example} examples/${example}.c)
    target_link_libraries(${example} PRIVATE texlink)
    target_include_directories(${example} PRIVATE include)
    if(MSVC)
      target_compile_options(${example} PRIVATE /W4)
    else()
      target_compile_options(${example} PRIVATE -Wall -Wextra)
    endif()
  endforeach()

  if(TARGET texlink_egl AND GLFW3_FOUND)
    foreach(example egl_tex_producer egl_tex_consumer)
      add_executable(${example} examples/${example}.c)
      target_link_libraries(${example} PRIVATE texlink_egl ${GLFW3_LIBRARIES}
                                               GLEW::GLEW m)
      target_include_directories(
        ${example} PRIVATE include ${GLFW3_INCLUDE_DIRS} ${EGL_INCLUDE_DIRS})
      target_compile_options(
        ${example} PRIVATE -Wall -Wextra ${GLFW3_CFLAGS_OTHER}
                           ${EGL_CFLAGS_OTHER})
    endforeach()
  endif()

  if(TARGET texlink_vulkan AND GLFW3_FOUND)
    foreach(example vulkan_tex_producer vulkan_tex_consumer)
      add_executable(${example} examples/${example}.c)
      target_link_libraries(${example} PRIVATE texlink_vulkan
                                               ${GLFW3_LIBRARIES} m)
      target_include_directories(
        ${example} PRIVATE include ${GLFW3_INCLUDE_DIRS} ${Vulkan_INCLUDE_DIRS})
      target_compile_definitions(${example} PRIVATE GLFW_INCLUDE_VULKAN)
      target_compile_options(${example} PRIVATE -Wall -Wextra -Wpedantic
                                                ${GLFW3_CFLAGS_OTHER})
    endforeach()
  endif()
endif()

include(GNUInstallDirs)

install(
  TARGETS ${TEXLINK_INSTALL_TARGETS}
  EXPORT texlink-targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(FILES ${TEXLINK_INSTALL_HEADERS}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
  EXPORT texlink-targets
  FILE texlink-config.cmake
  NAMESPACE texlink::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/texlink)
