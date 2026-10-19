cmake_minimum_required(VERSION 3.21)
project(mcp-cpp-server VERSION 0.1.0 LANGUAGES CXX)

option(MCP_BUILD_EXAMPLES "Build the example server" ${PROJECT_IS_TOP_LEVEL})
option(MCP_BUILD_BRIDGE "Build the mcp-stdio-bridge executable" ${PROJECT_IS_TOP_LEVEL})
option(MCP_FETCH_DEPS "Fetch RapidJSON and cpp-httplib when no targets are provided" ON)

include(FetchContent)

if(NOT TARGET RapidJSON::RapidJSON)
    if(NOT MCP_FETCH_DEPS)
        message(FATAL_ERROR "mcp-cpp-server: define a RapidJSON::RapidJSON target or enable MCP_FETCH_DEPS")
    endif()
    FetchContent_Declare(rapidjson
        GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
        GIT_TAG        24b5e7a8b27f42fa16b96fc70aade9106cf7102f
        SOURCE_SUBDIR  _no_cmake)
    FetchContent_MakeAvailable(rapidjson)
    add_library(mcp_rapidjson INTERFACE)
    target_include_directories(mcp_rapidjson SYSTEM INTERFACE ${rapidjson_SOURCE_DIR}/include)
    add_library(RapidJSON::RapidJSON ALIAS mcp_rapidjson)
endif()

if(NOT TARGET httplib::httplib)
    if(NOT MCP_FETCH_DEPS)
        message(FATAL_ERROR "mcp-cpp-server: define an httplib::httplib target or enable MCP_FETCH_DEPS")
    endif()
    FetchContent_Declare(httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG        v0.61.0
        SOURCE_SUBDIR  _no_cmake)
    FetchContent_MakeAvailable(httplib)
    add_library(mcp_httplib INTERFACE)
    target_include_directories(mcp_httplib SYSTEM INTERFACE ${httplib_SOURCE_DIR})
    find_package(Threads REQUIRED)
    target_link_libraries(mcp_httplib INTERFACE Threads::Threads $<$<PLATFORM_ID:Windows>:ws2_32>)
    add_library(httplib::httplib ALIAS mcp_httplib)
endif()

add_library(mcp-cpp-server
    src/client_config.cpp
    src/dispatcher.cpp
    src/http_transport.cpp
    src/protocol.cpp
    src/server.cpp
    src/stdio_bridge.cpp
    src/tool_registry.cpp)
add_library(mcp::server ALIAS mcp-cpp-server)

target_compile_features(mcp-cpp-server PUBLIC cxx_std_20)
target_include_directories(mcp-cpp-server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(mcp-cpp-server PUBLIC RAPIDJSON_HAS_STDSTRING=1 RAPIDJSON_NOMEMBERITERATORCLASS)
target_link_libraries(mcp-cpp-server
    PUBLIC  RapidJSON::RapidJSON
    PRIVATE httplib::httplib)
if(MSVC)
    target_compile_options(mcp-cpp-server PRIVATE /W4 /utf-8)
else()
    target_compile_options(mcp-cpp-server PRIVATE -Wall -Wextra)
endif()

if(MCP_BUILD_EXAMPLES)
    add_executable(mcp-echo examples/echo/main.cpp)
    target_link_libraries(mcp-echo PRIVATE mcp::server)
endif()

if(MCP_BUILD_BRIDGE)
    add_executable(mcp-stdio-bridge tools/stdio_bridge/main.cpp)
    target_link_libraries(mcp-stdio-bridge PRIVATE mcp::server)
endif()
