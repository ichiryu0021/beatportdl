cmake_minimum_required(VERSION 3.21)
project(BPDownloader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# BP_STATIC_LINK=ON：配合静态 Qt prefix（MSYS2 qt6-static）做全静态单 exe。
# 用法：
#   cmake -S . -B build-static -G "MinGW Makefiles" \
#     -DCMAKE_PREFIX_PATH="<repo>/.toolchain/mingw64/qt6-static;C:/msys64/mingw64" \
#     -DBP_STATIC_LINK=ON
option(BP_STATIC_LINK "Fully static link (static Qt + static runtime) into a single exe" OFF)
if(BP_STATIC_LINK)
  set(OPENSSL_USE_STATIC_LIBS ON)
  # 强制 find_library 只选静态归档（含 taglib/openssl 及 Qt 的第三方依赖）。
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
  set(BUILD_SHARED_LIBS OFF)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network LinguistTools)
find_package(OpenSSL REQUIRED)

find_path(TAGLIB_INCLUDE_DIR taglib/tag.h
  PATHS ${CMAKE_PREFIX_PATH}/include /mingw64/include)
find_library(TAGLIB_LIBRARY NAMES tag taglib
  PATHS ${CMAKE_PREFIX_PATH}/lib /mingw64/lib)
if(NOT TAGLIB_INCLUDE_DIR OR NOT TAGLIB_LIBRARY)
  message(FATAL_ERROR "TagLib not found (need taglib 2.x headers and libtag)")
endif()

set(GUI_SOURCES
  src/main.cpp
  src/gui/main_window.cpp
  src/gui/backend_client.cpp
  src/gui/login_dialog.cpp
  src/gui/date_range_widget.cpp
  src/app.qrc
)

set(CORE_SOURCES
  src/core/util.cpp
  src/core/models.cpp
  src/core/config.cpp
  src/core/logger.cpp
  src/core/http.cpp
  src/core/auth.cpp
  src/core/beatport.cpp
  src/core/links.cpp
  src/core/genres_scrape.cpp
  src/core/tags.cpp
  src/core/integrity.cpp
  src/core/archiver.cpp
  src/core/hls.cpp
  src/core/downloader.cpp
  src/core/scheduler.cpp
  src/core/rpc_core.cpp
  src/core/rpcserver.cpp
  src/core/cli.cpp
)

if(WIN32)
  add_executable(BPDownloader WIN32 ${GUI_SOURCES} ${CORE_SOURCES})
  target_compile_definitions(BPDownloader PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_executable(BPDownloader ${GUI_SOURCES} ${CORE_SOURCES})
endif()

target_include_directories(BPDownloader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/gui
  ${TAGLIB_INCLUDE_DIR}
)

target_link_libraries(BPDownloader PRIVATE
  Qt6::Widgets
  Qt6::Network
  OpenSSL::Crypto
  ${TAGLIB_LIBRARY}
)

if(BP_STATIC_LINK AND WIN32)
  # 全静态：MinGW 运行时 + winpthread 也静态化；--gc-sections 裁掉未引用的
  # 静态 Qt 对象（如未使用的 Vulkan RHI），避免引入 vulkan-1.dll 等外部依赖。
  # TAGLIB_STATIC：让 taglib 头文件去掉 dllimport，否则引用 __imp_ 符号。
  target_compile_definitions(BPDownloader PRIVATE TAGLIB_STATIC)
  # 排除不需要的静态插件：
  # - QTiff：会把整个 libtiff（含 zstd/libdeflate 等一长串编解码依赖）拖进
  #   链接；本程序不处理 TIFF。
  # - QTlsBackendOpenSSLPlugin：Qt6 静态默认同时链入 SChannel 和 OpenSSL
  #   两个 TLS 后端。若 OpenSSL 后端抢占 SChannel，TLS 指纹（JA3/JA4）会
  #   从 Windows 原生变成 OpenSSL 的，导致 Beatport 的 Cloudflare 风控 403。
  #   动态版部署只有 qschannelbackend.dll（无 OpenSSL），此处强制对齐。
  qt6_import_plugins(BPDownloader EXCLUDE
    Qt6::QTiffPlugin
    Qt6::QTlsBackendOpenSSLPlugin)
  target_compile_options(BPDownloader PRIVATE
    -ffunction-sections -fdata-sections)
  # CMAKE_PREFIX_PATH 下的第三方静态库（libwebp/libsharpyuv/libtiff 等）以裸
  # -l 名由 Qt 的 Wrap 目标引入，需显式补上各前缀的 lib 搜索目录。
  foreach(_p ${CMAKE_PREFIX_PATH})
    target_link_directories(BPDownloader PRIVATE "${_p}/lib")
  endforeach()
  target_link_options(BPDownloader PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
    -Wl,--gc-sections)
  # 静态 OpenSSL / 静态第三方库需要的 Windows 系统库（系统自带，非随包 DLL）。
  # 其余是 Qt 静态库的传递依赖（MSYS2 的 freetype/harfbuzz 等 cmake config 不
  # 声明它们的静态依赖）。关键：这些库必须排在 Qt 静态库之后才能解析符号
  # （libfreetype.a 产生 BZ2_* 需求，-lbz2 必须在其后），用 INTERFACE 目标
  # 显式依赖 Qt 目标，拓扑排序时解算组自然落在 Qt 之后；--start-group 处理
  # 第三方库之间的循环引用。
  add_library(bp_static_rescan INTERFACE)
  target_link_libraries(bp_static_rescan INTERFACE
    Qt6::Widgets Qt6::Network)
  target_link_libraries(bp_static_rescan INTERFACE
    "-Wl,--start-group"
    ws2_32 crypt32 secur32 ncrypt bcrypt winmm version dwmapi uxtheme gdi32 imm32
    rpcrt4 usp10 opengl32 netapi32 powrprof wtsapi32 shlwapi ole32 shell32
    advapi32 user32 winspool comdlg32 setupapi
    bz2 zstd lzma brotlidec brotlicommon
    png16 jpeg freetype harfbuzz graphite2
    glib-2.0 pcre2-16 pcre2-8 double-conversion iconv intl ffi
    b2 md4c md4c-html webp sharpyuv
    "-Wl,--end-group"
    z)
  target_link_libraries(BPDownloader PRIVATE bp_static_rescan)
endif()

if(WIN32)
  target_link_libraries(BPDownloader PRIVATE ws2_32)
endif()

# ---- 国际化（Qt Linguist）----
# 英文为源语言（tr() 内建），简体中文翻译在 i18n/bpdownloader_zh_CN.ts。
# qt_add_translations 会把 .ts 编译为 .qm 并以资源形式嵌入可执行文件
# （:/i18n/bpdownloader_zh_CN.qm），单 exe 发布无需额外语言文件。
# 维护译文：构建后执行 `cmake --build . --target update_translations`
# 重新生成 .ts，再翻译并重新构建。
qt_add_translations(BPDownloader
  TS_FILES i18n/bpdownloader_zh_CN.ts
  RESOURCE_PREFIX "/i18n"
  SOURCES_TARGET BPDownloader)

# ---- standalone pure-logic smoke test (not shipped) ----
option(BUILD_SMOKE_TEST "Build pure-logic smoke test" ON)
if(BUILD_SMOKE_TEST)
  add_executable(bp_smoke
    tests/smoke_test.cpp
    src/core/util.cpp
    src/core/models.cpp
    src/core/config.cpp
    src/core/logger.cpp
    src/core/links.cpp
    src/core/genres_scrape.cpp
    src/core/hls.cpp
    src/core/http.cpp
    src/core/auth.cpp
    src/core/beatport.cpp
    src/core/archiver.cpp
  )
  target_include_directories(bp_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${TAGLIB_INCLUDE_DIR})
  target_compile_definitions(bp_smoke PRIVATE
    $<$<BOOL:${WIN32}>:UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN>)
  # hls.cpp 用 OpenSSL(EVP)；http.cpp/beatport.cpp 用 Qt Network。
  target_link_libraries(bp_smoke PRIVATE
    Qt6::Core
    Qt6::Network
    OpenSSL::Crypto
    ${TAGLIB_LIBRARY})
  if(BP_STATIC_LINK AND WIN32)
    target_compile_options(bp_smoke PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(bp_smoke PRIVATE
      -static -static-libgcc -static-libstdc++ -Wl,--gc-sections)
    target_link_libraries(bp_smoke PRIVATE
      ws2_32 crypt32 secur32 ncrypt bcrypt winmm z)
  endif()
endif()
