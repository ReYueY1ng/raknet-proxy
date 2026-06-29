cmake_minimum_required(VERSION 3.10)
project(raknet-proxy C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apply patches to RakNet/Source before building
set(RAKNET_SRC_DIR "${CMAKE_SOURCE_DIR}/RakNet/Source")
file(COPY ${CMAKE_SOURCE_DIR}/patch/ DESTINATION ${RAKNET_SRC_DIR})

# ── MiniUPnPc ──
set(MINIUPNPC_DIR "${CMAKE_SOURCE_DIR}/RakNet/DependentExtensions/miniupnpc-1.5")
set(MINIUPNPC_SOURCES
    ${MINIUPNPC_DIR}/connecthostport.c
    ${MINIUPNPC_DIR}/igd_desc_parse.c
    ${MINIUPNPC_DIR}/minisoap.c
    ${MINIUPNPC_DIR}/minissdpc.c
    ${MINIUPNPC_DIR}/miniupnpc.c
    ${MINIUPNPC_DIR}/miniwget.c
    ${MINIUPNPC_DIR}/minixml.c
    ${MINIUPNPC_DIR}/upnpcommands.c
    ${MINIUPNPC_DIR}/upnperrors.c
    ${MINIUPNPC_DIR}/upnpreplyparse.c
)

add_library(miniupnpc STATIC ${MINIUPNPC_SOURCES})
target_include_directories(miniupnpc PUBLIC ${MINIUPNPC_DIR} ${RAKNET_SRC_DIR})

# ── RakNet (static library) ──
# RakNet's CMakeLists.txt requires CMake < 2.6, not compatible with modern CMake
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(RakNet RakNet_build)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# ── Proxy executable ──
add_executable(raknet_proxy raknet_proxy.cpp)
target_include_directories(raknet_proxy PRIVATE ${RAKNET_SRC_DIR} ${MINIUPNPC_DIR})
target_compile_definitions(raknet_proxy PRIVATE PREALLOCATE_LARGE_MESSAGES=1)
target_link_libraries(raknet_proxy RakNetLibStatic miniupnpc pthread)
