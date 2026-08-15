cmake_minimum_required(VERSION 3.10)
project(raknet-proxy C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# AddressSanitizer for pinpointing heap buffer overflows (Debug builds only)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-fsanitize=address -g -O1 -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

# Apply patches to RakNet/Source before building
set(RAKNET_SRC_DIR "${CMAKE_SOURCE_DIR}/RakNet/Source")
file(COPY ${CMAKE_SOURCE_DIR}/patch/ DESTINATION ${RAKNET_SRC_DIR})

# Apply miniupnpc patches
set(MINIUPNPC_PATCH_DIR "${CMAKE_SOURCE_DIR}/RakNet/DependentExtensions/miniupnpc-1.5")
if(EXISTS "${CMAKE_SOURCE_DIR}/patch/minissdpc.c")
  file(COPY "${CMAKE_SOURCE_DIR}/patch/minissdpc.c" DESTINATION "${MINIUPNPC_PATCH_DIR}")
endif()

# ── MiniUPnPc ──
# Use system-installed miniupnpc (newer API compatible)
find_library(MINIUPNPC_LIBRARY miniupnpc)
find_path(MINIUPNPC_INCLUDE_DIR miniupnpc/miniupnpc.h)

if(MINIUPNPC_LIBRARY AND MINIUPNPC_INCLUDE_DIR)
  message(STATUS "Using system miniupnpc: ${MINIUPNPC_LIBRARY}")
  add_library(miniupnpc INTERFACE IMPORTED)
  set_target_properties(miniupnpc PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${MINIUPNPC_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${MINIUPNPC_LIBRARY}"
  )
else()
  message(FATAL_ERROR "System miniupnpc not found. Please install libminiupnpc-dev (Linux) or miniupnpc (vcpkg/Windows)")
endif()

# ── RakNet (static library) ──
# RakNet's CMakeLists.txt requires CMake < 2.6, not compatible with modern CMake
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
# PREALLOCATE_LARGE_MESSAGES must be set globally — the #if checks are in
# RakNet's ReliabilityLayer.h/.cpp which compile as part of RakNetLibStatic.
add_compile_definitions(PREALLOCATE_LARGE_MESSAGES=1)

# Patch RakNet's LibStatic CMakeLists.txt to remove /NODEFAULTLIB which conflicts with /MT
set(RAKNET_LIBSTATIC_CMAKE "${CMAKE_SOURCE_DIR}/RakNet/Lib/LibStatic/CMakeLists.txt")
file(READ "${RAKNET_LIBSTATIC_CMAKE}" RAKNET_LIBSTATIC_CONTENT)
string(REPLACE "STATIC_LIBRARY_FLAGS \"/NODEFAULTLIB:\\\"LIBCD.lib LIBCMTD.lib MSVCRT.lib\\\"\"" "STATIC_LIBRARY_FLAGS \"\"" RAKNET_LIBSTATIC_CONTENT "${RAKNET_LIBSTATIC_CONTENT}")
string(REPLACE "STATIC_LIBRARY_FLAGS \"/NODEFAULTLIB:&quot;LIBCD.lib LIBCMTD.lib MSVCRT.lib&quot;\"" "STATIC_LIBRARY_FLAGS \"\"" RAKNET_LIBSTATIC_CONTENT "${RAKNET_LIBSTATIC_CONTENT}")
file(WRITE "${RAKNET_LIBSTATIC_CMAKE}" "${RAKNET_LIBSTATIC_CONTENT}")

add_subdirectory(RakNet RakNet_build)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# ── Proxy executable ──
add_executable(raknet_proxy raknet_proxy.cpp)
target_include_directories(raknet_proxy PRIVATE ${RAKNET_SRC_DIR} ${CMAKE_BINARY_DIR}/include)
target_link_libraries(raknet_proxy RakNetLibStatic miniupnpc pthread)
