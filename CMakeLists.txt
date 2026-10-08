cmake_minimum_required(VERSION 3.20)

project(BoostyDownloader
    VERSION 1.1.0
    DESCRIPTION "Portable Win32 Boosty video downloader"
    LANGUAGES CXX
)

set(BOOSTY_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX)
endif()

set_source_files_properties(src/App/resources.rc PROPERTIES
    OBJECT_DEPENDS "${BOOSTY_APP_ICON}"
)

set(WEBVIEW2_SDK_DIR "" CACHE PATH "Path to Microsoft.Web.WebView2 NuGet package root")
option(BOOSTY_FETCH_WEBVIEW2_SDK "Download Microsoft.Web.WebView2 SDK into the build directory when WEBVIEW2_SDK_DIR is empty" ON)
set(BOOSTY_WEBVIEW2_SOURCES src/UI/LoginDialogStub.cpp)
if(NOT WEBVIEW2_SDK_DIR AND BOOSTY_FETCH_WEBVIEW2_SDK)
    set(WEBVIEW2_NUPKG "${CMAKE_BINARY_DIR}/_deps/webview2/Microsoft.Web.WebView2.nupkg")
    set(WEBVIEW2_EXTRACT_DIR "${CMAKE_BINARY_DIR}/_deps/webview2/package")
    if(NOT EXISTS "${WEBVIEW2_EXTRACT_DIR}/build/native/include/WebView2.h")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps/webview2")
        file(DOWNLOAD
            "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2"
            "${WEBVIEW2_NUPKG}"
            STATUS WEBVIEW2_DOWNLOAD_STATUS
            LOG WEBVIEW2_DOWNLOAD_LOG
        )
        list(GET WEBVIEW2_DOWNLOAD_STATUS 0 WEBVIEW2_DOWNLOAD_CODE)
        if(WEBVIEW2_DOWNLOAD_CODE EQUAL 0)
            file(ARCHIVE_EXTRACT INPUT "${WEBVIEW2_NUPKG}" DESTINATION "${WEBVIEW2_EXTRACT_DIR}")
        else()
            message(WARNING "WebView2 SDK download failed; login window will be disabled")
        endif()
    endif()
    if(EXISTS "${WEBVIEW2_EXTRACT_DIR}/build/native/include/WebView2.h")
        set(WEBVIEW2_SDK_DIR "${WEBVIEW2_EXTRACT_DIR}")
    endif()
endif()
if(WEBVIEW2_SDK_DIR)
    set(WEBVIEW2_INCLUDE_DIR "${WEBVIEW2_SDK_DIR}/build/native/include")
    set(WEBVIEW2_LIB "${WEBVIEW2_SDK_DIR}/build/native/x64/WebView2LoaderStatic.lib")
    if(EXISTS "${WEBVIEW2_INCLUDE_DIR}/WebView2.h" AND EXISTS "${WEBVIEW2_LIB}")
        set(BOOSTY_WEBVIEW2_SOURCES src/UI/LoginDialogWebView2.cpp)
        set(BOOSTY_HAS_WEBVIEW2 ON)
    endif()
endif()

add_executable(BoostyDownloader WIN32
    src/App/main.cpp
    src/Core/AppPaths.cpp
    src/Core/AppPaths.h
    src/Core/AppUpdateService.cpp
    src/Core/AppUpdateService.h
    src/Core/AppVersion.h
    src/Core/BoostyClient.cpp
    src/Core/BoostyClient.h
    src/Core/Config.cpp
    src/Core/Config.h
    src/Core/DownloadQueue.cpp
    src/Core/DownloadQueue.h
    src/Core/DownloadQueueStore.cpp
    src/Core/DownloadQueueStore.h
    src/Core/FileOperations.cpp
    src/Core/FileOperations.h
    src/Core/FfmpegTools.cpp
    src/Core/FfmpegTools.h
    src/Core/Logger.cpp
    src/Core/Logger.h
    src/Core/ErrorFormatting.cpp
    src/Core/ErrorFormatting.h
    src/Core/TaskFormatting.cpp
    src/Core/TaskFormatting.h
    src/Core/Text.cpp
    src/Core/Text.h
    src/Platform/WinHttpClient.cpp
    src/Platform/WinHttpClient.h
    src/UI/Application.cpp
    src/UI/Application.h
    src/App/resources.rc
    src/UI/LoginDialog.h
    ${BOOSTY_WEBVIEW2_SOURCES}
    src/UI/MessageDialog.cpp
    src/UI/MessageDialog.h
    src/UI/UiHelpers.cpp
    src/UI/UiHelpers.h
    src/UI/UiRenderer.cpp
    src/UI/UiRenderer.h
)

target_include_directories(BoostyDownloader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Platform
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UI
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

if(BOOSTY_HAS_WEBVIEW2)
    target_compile_definitions(BoostyDownloader PRIVATE BOOSTY_HAS_WEBVIEW2=1)
    target_include_directories(BoostyDownloader PRIVATE "${WEBVIEW2_INCLUDE_DIR}")
    target_link_libraries(BoostyDownloader PRIVATE "${WEBVIEW2_LIB}")
endif()

target_link_libraries(BoostyDownloader PRIVATE
    comctl32
    dwmapi
    gdiplus
    gdi32
    user32
    shell32
    ole32
    winhttp
    bcrypt
)

if(MSVC)
    target_compile_options(BoostyDownloader PRIVATE /W4 /permissive- /utf-8)
endif()

set_target_properties(BoostyDownloader PROPERTIES
    OUTPUT_NAME "BoostyDownloader"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_executable(BoostyDownloaderSelfTest
    tests/test_core.cpp
    src/Core/AppPaths.cpp
    src/Core/AppPaths.h
    src/Core/AppUpdateService.cpp
    src/Core/AppUpdateService.h
    src/Core/AppVersion.h
    src/Core/BoostyClient.h
    src/Core/DownloadQueueStore.cpp
    src/Core/DownloadQueueStore.h
    src/Core/DownloadQueue.cpp
    src/Core/DownloadQueue.h
    src/Core/Logger.cpp
    src/Core/Logger.h
    src/Core/ErrorFormatting.cpp
    src/Core/ErrorFormatting.h
    src/Core/TaskFormatting.cpp
    src/Core/TaskFormatting.h
    src/Core/FfmpegTools.cpp
    src/Core/FfmpegTools.h
    src/Core/FileOperations.cpp
    src/Core/FileOperations.h
    src/Core/Text.cpp
    src/Core/Text.h
    src/Platform/WinHttpClient.cpp
    src/Platform/WinHttpClient.h
)

target_include_directories(BoostyDownloaderSelfTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Platform
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(BoostyDownloaderSelfTest PRIVATE winhttp bcrypt)

if(MSVC)
    target_compile_options(BoostyDownloaderSelfTest PRIVATE /W4 /permissive- /utf-8)
endif()

enable_testing()
add_test(NAME BoostyDownloaderSelfTest COMMAND BoostyDownloaderSelfTest)
