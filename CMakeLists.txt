cmake_minimum_required(VERSION 3.21)

project(texlink VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(TEXLINK_BUILD_SHARED "Build texlink as a shared library" ON)
option(TEXLINK_BUILD_EXAMPLES "Build example programs" ON)
option(TEXLINK_WITH_EGL "Build the EGL/OpenGL texlink backend utility" OFF)
option(TEXLINK_WITH_WGL "Build the WGL/OpenGL texlink backend utility" OFF)
option(TEXLINK_WITH_VULKAN "Build the Vulkan texlink backend utility" OFF)
option(TEXLINK_WITH_D3D11 "Build the Direct3D 11 texlink backend utility" OFF)
option(TEXLINK_WITH_D3D12 "Build the Direct3D 12 texlink backend utility" OFF)

set(TEXLINK_CORE_SOURCES src/session.c)
set(TEXLINK_CORE_PRIVATE_INCLUDES src)
set(TEXLINK_CORE_PRIVATE_LIBS)
set(TEXLINK_CORE_PUBLIC_LIBS)
set(TEXLINK_CORE_PRIVATE_COMPILE_OPTIONS)

if(TEXLINK_BUILD_SHARED)
  set(TEXLINK_LIBRARY_TYPE SHARED)
else()
  set(TEXLINK_LIBRARY_TYPE STATIC)
endif()

if(WIN32)
  list(APPEND TEXLINK_CORE_SOURCES src/frame_win32.c src/socket_win32.c
       src/sync_win32.c src/registry_win32.c src/platform_win32.c)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GBM REQUIRED gbm)
  list(APPEND TEXLINK_CORE_SOURCES src/socket_posix.c src/sync_posix.c
       src/registry_posix.c src/platform_posix.c src/frame_posix.c)
  list(APPEND TEXLINK_CORE_PRIVATE_INCLUDES ${GBM_INCLUDE_DIRS})
  list(APPEND TEXLINK_CORE_PUBLIC_LIBS rt ${GBM_LIBRARIES})
  list(APPEND TEXLINK_CORE_PRIVATE_COMPILE_OPTIONS ${GBM_CFLAGS_OTHER})
endif()

add_library(texlink ${TEXLINK_LIBRARY_TYPE} ${TEXLINK_CORE_SOURCES})

if(TEXLINK_BUILD_SHARED)
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

if(TEXLINK_BUILD_EXAMPLES
   AND (TEXLINK_WITH_EGL OR TEXLINK_WITH_WGL OR TEXLINK_WITH_VULKAN
        OR TEXLINK_WITH_D3D11 OR TEXLINK_WITH_D3D12))
  if(WIN32)
    find_package(glfw3 CONFIG QUIET)
    if(glfw3_FOUND)
      set(GLFW3_FOUND TRUE)
      set(TEXLINK_GLFW_TARGET glfw)
      if(NOT TARGET glfw)
        set(TEXLINK_GLFW_TARGET glfw3)
      endif()
    else()
      set(TEXLINK_GLFW_ROOT "$ENV{GLFW_ROOT}")
      if(NOT TEXLINK_GLFW_ROOT AND EXISTS "$ENV{USERPROFILE}/scoop/apps/glfw/current")
        set(TEXLINK_GLFW_ROOT "$ENV{USERPROFILE}/scoop/apps/glfw/current")
      endif()
      find_path(GLFW3_INCLUDE_DIR
                NAMES GLFW/glfw3.h
                PATHS "${TEXLINK_GLFW_ROOT}"
                PATH_SUFFIXES include
                NO_DEFAULT_PATH)
      find_library(GLFW3_LIBRARY
                   NAMES glfw3dll glfw3 glfw
                   PATHS "${TEXLINK_GLFW_ROOT}"
                   PATH_SUFFIXES lib-vc2022 lib-static-ucrt lib
                   NO_DEFAULT_PATH)
      find_file(GLFW3_RUNTIME
                NAMES glfw3.dll glfw.dll
                PATHS "${TEXLINK_GLFW_ROOT}"
                PATH_SUFFIXES lib-vc2022 lib-static-ucrt bin
                NO_DEFAULT_PATH)
      if(GLFW3_INCLUDE_DIR AND GLFW3_LIBRARY)
        set(GLFW3_FOUND TRUE)
        set(GLFW3_INCLUDE_DIRS "${GLFW3_INCLUDE_DIR}")
        set(GLFW3_LIBRARIES "${GLFW3_LIBRARY}")
        set(GLFW3_RUNTIME "${GLFW3_RUNTIME}")
      endif()
    endif()
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW3 glfw3)
  endif()
  if((TEXLINK_WITH_WGL OR TEXLINK_WITH_VULKAN OR TEXLINK_WITH_D3D11
      OR TEXLINK_WITH_D3D12)
     AND NOT GLFW3_FOUND)
    message(STATUS "GLFW3 not found; skipping windowed GPU example programs")
  endif()
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
  add_library(texlink_egl src/backend_egl.c)
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

if(WIN32 AND TEXLINK_WITH_WGL)
  add_library(texlink_wgl src/backend_wgl.c)
  target_link_libraries(texlink_wgl PUBLIC texlink PRIVATE opengl32 d3d11 dxgi
                                           dxguid)
  target_include_directories(
    texlink_wgl PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                       $<INSTALL_INTERFACE:include>)
  if(MSVC)
    target_compile_definitions(texlink_wgl PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(texlink_wgl PRIVATE /W4)
  else()
    target_compile_options(texlink_wgl PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  list(APPEND TEXLINK_INSTALL_TARGETS texlink_wgl)
  list(APPEND TEXLINK_INSTALL_HEADERS include/texlink_wgl.h)
endif()

if(WIN32 AND TEXLINK_WITH_D3D11)
  add_library(texlink_d3d11 src/backend_d3d11.c)
  target_link_libraries(texlink_d3d11 PUBLIC texlink PRIVATE d3d11 dxgi dxguid)
  target_include_directories(
    texlink_d3d11 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                         $<INSTALL_INTERFACE:include>)
  if(MSVC)
    target_compile_definitions(texlink_d3d11 PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(texlink_d3d11 PRIVATE /W4)
  else()
    target_compile_options(texlink_d3d11 PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  list(APPEND TEXLINK_INSTALL_TARGETS texlink_d3d11)
  list(APPEND TEXLINK_INSTALL_HEADERS include/texlink_d3d11.h)
endif()

if(WIN32 AND TEXLINK_WITH_D3D12)
  add_library(texlink_d3d12 src/backend_d3d12.c)
  target_link_libraries(texlink_d3d12 PUBLIC texlink PRIVATE d3d12 dxgi dxguid)
  target_include_directories(
    texlink_d3d12 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                         $<INSTALL_INTERFACE:include>)
  if(MSVC)
    target_compile_definitions(texlink_d3d12 PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(texlink_d3d12 PRIVATE /W4)
  else()
    target_compile_options(texlink_d3d12 PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  list(APPEND TEXLINK_INSTALL_TARGETS texlink_d3d12)
  list(APPEND TEXLINK_INSTALL_HEADERS include/texlink_d3d12.h)
endif()

if(TEXLINK_WITH_VULKAN AND Vulkan_FOUND)
  if(WIN32)
    set(TEXLINK_VULKAN_PLATFORM_SRC src/backend_vulkan_win32.c)
  else()
    set(TEXLINK_VULKAN_PLATFORM_SRC src/backend_vulkan_posix.c)
  endif()
  add_library(texlink_vulkan src/backend_vulkan.c ${TEXLINK_VULKAN_PLATFORM_SRC})
  target_link_libraries(texlink_vulkan PUBLIC texlink Vulkan::Vulkan)
  target_include_directories(
    texlink_vulkan PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                          $<INSTALL_INTERFACE:include> ${Vulkan_INCLUDE_DIRS}
                  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
  if(MSVC)
    target_compile_definitions(texlink_vulkan PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(texlink_vulkan PRIVATE /W4)
  else()
    target_compile_options(texlink_vulkan PRIVATE -Wall -Wextra -Wpedantic)
  endif()
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

  if(TARGET texlink_wgl AND GLFW3_FOUND)
    foreach(example wgl_tex_producer wgl_tex_consumer)
      add_executable(${example} examples/${example}.c)
      target_link_libraries(${example} PRIVATE texlink_wgl d3d12)
      if(TEXLINK_GLFW_TARGET)
        target_link_libraries(${example} PRIVATE ${TEXLINK_GLFW_TARGET})
      else()
        target_link_libraries(${example} PRIVATE ${GLFW3_LIBRARIES} user32
                                                 gdi32 shell32)
        if(GLFW3_RUNTIME)
          add_custom_command(
            TARGET ${example}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GLFW3_RUNTIME}"
                    $<TARGET_FILE_DIR:${example}>)
        endif()
      endif()
      target_include_directories(${example} PRIVATE include ${GLFW3_INCLUDE_DIRS})
      if(MSVC)
        target_compile_definitions(${example} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${example} PRIVATE /W4)
      else()
        target_compile_options(${example} PRIVATE -Wall -Wextra -Wpedantic
                                                  ${GLFW3_CFLAGS_OTHER})
      endif()
    endforeach()
  endif()

  if(TARGET texlink_d3d11 AND GLFW3_FOUND)
    foreach(example d3d11_tex_producer d3d11_tex_consumer)
      add_executable(${example} examples/${example}.c)
      target_link_libraries(${example} PRIVATE texlink_d3d11)
      if(example STREQUAL "d3d11_tex_producer")
        target_link_libraries(${example} PRIVATE d3d12)
      elseif(example STREQUAL "d3d11_tex_consumer")
        target_link_libraries(${example} PRIVATE d3d12 d3dcompiler)
      endif()
      if(TEXLINK_GLFW_TARGET)
        target_link_libraries(${example} PRIVATE ${TEXLINK_GLFW_TARGET})
      else()
        target_link_libraries(${example} PRIVATE ${GLFW3_LIBRARIES} user32
                                                 gdi32 shell32)
        if(GLFW3_RUNTIME)
          add_custom_command(
            TARGET ${example}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GLFW3_RUNTIME}"
                    $<TARGET_FILE_DIR:${example}>)
        endif()
      endif()
      target_include_directories(${example} PRIVATE include ${GLFW3_INCLUDE_DIRS})
      if(MSVC)
        target_compile_definitions(${example} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${example} PRIVATE /W4)
      else()
        target_compile_options(${example} PRIVATE -Wall -Wextra -Wpedantic)
      endif()
    endforeach()
  endif()

  if(TARGET texlink_d3d12 AND GLFW3_FOUND)
    foreach(example d3d12_tex_producer d3d12_tex_consumer)
      add_executable(${example} examples/${example}.c)
      target_link_libraries(${example} PRIVATE texlink_d3d12)
      if(example STREQUAL "d3d12_tex_producer")
        target_link_libraries(${example} PRIVATE d3d11)
      elseif(example STREQUAL "d3d12_tex_consumer")
        target_link_libraries(${example} PRIVATE d3dcompiler)
      endif()
      if(TEXLINK_GLFW_TARGET)
        target_link_libraries(${example} PRIVATE ${TEXLINK_GLFW_TARGET})
      else()
        target_link_libraries(${example} PRIVATE ${GLFW3_LIBRARIES} user32
                                                 gdi32 shell32)
        if(GLFW3_RUNTIME)
          add_custom_command(
            TARGET ${example}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GLFW3_RUNTIME}"
                    $<TARGET_FILE_DIR:${example}>)
        endif()
      endif()
      target_include_directories(${example} PRIVATE include ${GLFW3_INCLUDE_DIRS})
      if(MSVC)
        target_compile_definitions(${example} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${example} PRIVATE /W4)
      else()
        target_compile_options(${example} PRIVATE -Wall -Wextra -Wpedantic)
      endif()
    endforeach()
  endif()

  if(TARGET texlink_vulkan AND GLFW3_FOUND)
    foreach(example vulkan_tex_producer vulkan_tex_consumer)
      add_executable(${example} examples/${example}.c)
      target_link_libraries(${example} PRIVATE texlink_vulkan)
      if(WIN32)
        if(example STREQUAL "vulkan_tex_producer")
          target_link_libraries(${example} PRIVATE d3d11 dxguid)
        elseif(example STREQUAL "vulkan_tex_consumer")
          target_link_libraries(${example} PRIVATE d3d12 dxguid)
        endif()
        if(TEXLINK_GLFW_TARGET)
          target_link_libraries(${example} PRIVATE ${TEXLINK_GLFW_TARGET})
        else()
          target_link_libraries(${example} PRIVATE ${GLFW3_LIBRARIES} user32
                                                   gdi32 shell32)
          if(GLFW3_RUNTIME)
            add_custom_command(
              TARGET ${example}
              POST_BUILD
              COMMAND ${CMAKE_COMMAND} -E copy_if_different
                      "${GLFW3_RUNTIME}" $<TARGET_FILE_DIR:${example}>)
          endif()
        endif()
      else()
        target_link_libraries(${example} PRIVATE ${GLFW3_LIBRARIES} m)
      endif()
      target_include_directories(
        ${example} PRIVATE include ${GLFW3_INCLUDE_DIRS} ${Vulkan_INCLUDE_DIRS})
      target_compile_definitions(${example} PRIVATE GLFW_INCLUDE_VULKAN)
      if(WIN32)
        target_compile_definitions(${example} PRIVATE VK_USE_PLATFORM_WIN32_KHR)
      endif()
      if(MSVC)
        target_compile_definitions(${example} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${example} PRIVATE /W4)
      else()
        target_compile_options(${example} PRIVATE -Wall -Wextra -Wpedantic
                                                  ${GLFW3_CFLAGS_OTHER})
      endif()
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
