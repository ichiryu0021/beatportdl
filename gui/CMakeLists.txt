cmake_minimum_required(VERSION 3.16)

project(beatportdl_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Auto-generate MOC/UIC/RCC files so Q_OBJECT classes and .ui/.qrc work.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# NOTE: The Go back-end binary must already be running for the GUI to do
# anything useful. From the repository root start it with:
#     beatportdl --serve
# It listens on http://127.0.0.1:8765 and exposes:
#   POST /rpc      JSON-RPC 2.0 (search/getLatest/getGenres/download/login/
#                 authStatus/startScheduler/stopScheduler/schedulerStatus)
#   GET  /events   Server-Sent Events (log, scheduler.found, download progress)
#   GET  /health   liveness probe
# This CMake project builds only the Qt front-end; it does not build or
# launch the Go back-end.

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

add_executable(beatportdl_gui WIN32
    main.cpp
    backend_client.cpp
    backend_client.h
    login_dialog.cpp
    login_dialog.h
    main_window.cpp
    main_window.h
    date_range_widget.cpp
    date_range_widget.h
)

target_link_libraries(beatportdl_gui PRIVATE Qt6::Widgets Qt6::Network)
