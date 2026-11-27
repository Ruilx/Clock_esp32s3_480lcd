cmake_minimum_required(VERSION 3.20)

project(host_status_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(host-status-agent
    src/main.cpp
)

target_include_directories(host-status-agent PRIVATE src)

if(WIN32)
    target_sources(host-status-agent PRIVATE src/platform_windows.cpp)
    target_link_libraries(host-status-agent PRIVATE ws2_32 iphlpapi pdh)
elseif(APPLE)
    target_sources(host-status-agent PRIVATE src/platform_macos.cpp)
elseif(UNIX)
    target_sources(host-status-agent PRIVATE src/platform_linux.cpp)
endif()
