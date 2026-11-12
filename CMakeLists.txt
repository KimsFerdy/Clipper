cmake_minimum_required(VERSION 3.21)
# static msvc runtime: Clipper.exe runs without the VC++ redistributable. has to be set before any target
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(Clipper VERSION 1.0.0 LANGUAGES C CXX)
set(CLIPPER_VERSION_SUFFIX "")  # "-beta" etc. for a pre-release

if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Windows)$")
    message(FATAL_ERROR "Clipper builds on Linux and Windows")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CLIPPER_BUILD_GUI   "Build the app" ON)
option(CLIPPER_BUILD_TESTS "Build the tests" ON)

find_package(Threads REQUIRED)

function(clipper_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# core: history model, journal file, search. no os stuff in here
add_library(clipper_core STATIC
    src/core/ClipStore.cpp
    src/core/FileList.cpp
    src/core/Filter.cpp
    src/core/Png.cpp
)
target_include_directories(clipper_core PUBLIC src)
clipper_warnings(clipper_core)

if(CLIPPER_BUILD_GUI)
    include(cmake/Dependencies.cmake)
    include(cmake/EmbedIcon.cmake)

    if(WIN32)
        # platform: win32 clipboard listener + SendInput, Shell_NotifyIcon tray, RegisterHotKey, HKCU Run
        add_library(clipper_platform STATIC
            src/platform/Accel.cpp
            src/platform/Win32AppBus.cpp
            src/platform/Win32Appearance.cpp
            src/platform/Win32Autostart.cpp
            src/platform/Win32Clipboard.cpp
            src/platform/Win32Desktop.cpp
            src/platform/Win32GlobalShortcut.cpp
            src/platform/Win32Paths.cpp
            src/platform/Win32Shell.cpp
            src/platform/Win32Tray.cpp
            src/platform/Win32Util.cpp
        )
        target_compile_definitions(clipper_platform PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
        target_link_libraries(clipper_platform PUBLIC clipper_core Threads::Threads
                                               PRIVATE image_decoders user32 gdi32 shell32 ole32 advapi32 dwmapi)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)  # sd-bus: tray icon + single instance
        pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11 xfixes)      # clipboard. libXtst gets dlopen'ed

        # platform: x11 clipboard + desktop, tray, d-bus, xdg, shortcuts
        add_library(clipper_platform STATIC
            src/platform/Accel.cpp
            src/platform/AppBus.cpp
            src/platform/Appearance.cpp
            src/platform/Autostart.cpp
            src/platform/GlobalShortcut.cpp
            src/platform/Paths.cpp
            src/platform/Process.cpp
            src/platform/Shell.cpp
            src/platform/Tray.cpp
            src/platform/X11Clipboard.cpp
            src/platform/X11Desktop.cpp
        )
        target_link_libraries(clipper_platform PUBLIC clipper_core PkgConfig::SYSTEMD PkgConfig::X11 Threads::Threads
                                                      ${CMAKE_DL_LIBS})
    endif()
    clipper_warnings(clipper_platform)

    add_executable(Clipper
        src/app/main.cpp
        src/app/App.cpp
        src/app/NativeWindow.cpp
        src/app/PreferencesDialog.cpp
        src/app/Settings.cpp
        src/app/Theme.cpp
        src/app/Thumbnails.cpp
    )
    target_link_libraries(Clipper PRIVATE clipper_platform imgui image_decoders)
    target_compile_definitions(Clipper PRIVATE CLIPPER_VERSION="${PROJECT_VERSION}${CLIPPER_VERSION_SUFFIX}")
    target_include_directories(Clipper PRIVATE ${CMAKE_BINARY_DIR}/generated)
    clipper_warnings(Clipper)

    if(WIN32)
        # icon (GLFW_ICON, glfw and the tray load it by that name), version info, utf-8 + dpi manifest
        configure_file(data/windows/clipper.rc.in ${CMAKE_BINARY_DIR}/generated/clipper.rc @ONLY)
        target_sources(Clipper PRIVATE ${CMAKE_BINARY_DIR}/generated/clipper.rc data/windows/clipper.manifest)
        set_target_properties(Clipper PROPERTIES WIN32_EXECUTABLE ON)
        target_link_options(Clipper PRIVATE /ENTRY:mainCRTStartup)  # keep main(), no console window
        target_link_libraries(Clipper PRIVATE dwmapi)
    else()
        # cmake --install build/release --prefix ~/.local  -> shows up in the app menu
        include(GNUInstallDirs)
        install(TARGETS Clipper RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES data/clipper.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        install(FILES assets/clipper.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)

        # cpack -G DEB
        set(CPACK_PACKAGE_NAME "clipper")
        # 0.1.0~beta sorts before 0.1.0 in dpkg, so the final release upgrades over the beta
        string(REPLACE "-" "~" CLIPPER_DEB_SUFFIX "${CLIPPER_VERSION_SUFFIX}")
        set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}${CLIPPER_DEB_SUFFIX}")
        set(CPACK_PACKAGE_VENDOR "KimsFerdy")
        set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Clipboard history for Linux, inspired by ClipClip")
        set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/KimsFerdy/Clipper")
        set(CPACK_PACKAGE_CONTACT "KimsFerdy <102543259+KimsFerdy@users.noreply.github.com>")
        set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
        set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
        set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
        set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
        set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libxtst6")  # auto-paste, dlopen'ed so shlibdeps can't see it
        include(CPack)
    endif()
endif()

if(CLIPPER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
