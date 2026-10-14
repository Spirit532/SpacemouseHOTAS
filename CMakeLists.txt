cmake_minimum_required(VERSION 3.16)
project(SpacemouseHOTAS VERSION 1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(SHARED_SOURCES
  src/app.cpp
  src/core/capture_apps.cpp
  src/core/config.cpp
  src/core/keys.cpp
  src/core/mapping.cpp
  src/core/spacemouse.cpp
  src/ui/canvas.cpp
  src/ui/icon.cpp
  src/ui/widgets.cpp
)

if(WIN32)
  set(WINDOWS_ICON ${CMAKE_SOURCE_DIR}/packaging/windows/app.ico)
  if(EXISTS ${WINDOWS_ICON})
    set(WINDOWS_ICON_LINE "1 ICON \"${WINDOWS_ICON}\"")
  endif()
  configure_file(packaging/windows/app.rc.in ${CMAKE_BINARY_DIR}/app.rc @ONLY)
  # Static CRT: a single exe without redistributables.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_executable(SpacemouseHOTAS WIN32 ${SHARED_SOURCES}
    src/platform/win32/debug_tools.cpp
    src/platform/win32/font_win32.cpp
    src/platform/win32/main_win32.cpp
    src/platform/win32/system_win32.cpp
    src/platform/win32/threedx_capture.cpp
    src/platform/win32/vjoy_pad.cpp
    ${CMAKE_BINARY_DIR}/app.rc)
  target_link_libraries(SpacemouseHOTAS PRIVATE user32 gdi32 shell32 advapi32 comdlg32 hid setupapi)
  target_compile_definitions(SpacemouseHOTAS PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(SpacemouseHOTAS PRIVATE /W3 /O2 /GR-)
    target_link_options(SpacemouseHOTAS PRIVATE /OPT:REF /OPT:ICF)
  endif()
elseif(APPLE)
  enable_language(OBJCXX)
  add_executable(SpacemouseHOTAS MACOSX_BUNDLE ${SHARED_SOURCES} src/platform/macos/main_macos.mm)
  set_target_properties(SpacemouseHOTAS PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
  target_link_libraries(SpacemouseHOTAS PRIVATE
    "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation" "-framework CoreText")
  target_compile_options(SpacemouseHOTAS PRIVATE -Os -fno-rtti $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
else()
  find_package(X11 REQUIRED)
  find_package(Freetype REQUIRED)  # headers only, Xft.h includes them
  if(NOT X11_Xft_FOUND)
    message(FATAL_ERROR "libXft development files not found (libxft-dev)")
  endif()
  add_executable(SpacemouseHOTAS ${SHARED_SOURCES}
    src/platform/linux/font_linux.cpp
    src/platform/linux/main_linux.cpp
    src/platform/linux/system_linux.cpp
    src/platform/linux/tray_linux.cpp
    src/platform/linux/uinput_linux.cpp)
  target_include_directories(SpacemouseHOTAS PRIVATE ${X11_INCLUDE_DIR} ${X11_Xft_INCLUDE_PATH} ${FREETYPE_INCLUDE_DIRS})
  target_link_libraries(SpacemouseHOTAS PRIVATE ${X11_LIBRARIES} ${X11_Xft_LIB} ${CMAKE_DL_LIBS})
  target_compile_options(SpacemouseHOTAS PRIVATE -Os -fno-rtti -Wall -Wextra -Wno-unused-parameter)
  target_link_options(SpacemouseHOTAS PRIVATE -Wl,--gc-sections -s)
endif()

target_include_directories(SpacemouseHOTAS PRIVATE src)
target_compile_definitions(SpacemouseHOTAS PRIVATE APP_VERSION="${PROJECT_VERSION}")
