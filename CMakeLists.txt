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
# Priority: vendored source (downloaded by CI) > system package
set(MINIUPNPC_VENDORED "${CMAKE_SOURCE_DIR}/third_party/miniupnpc")
if(EXISTS "${MINIUPNPC_VENDORED}/CMakeLists.txt")
  message(STATUS "Building miniupnpc from vendored source: ${MINIUPNPC_VENDORED}")
  set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
  set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
  add_subdirectory(${MINIUPNPC_VENDORED} ${CMAKE_BINARY_DIR}/miniupnpc_vendored)
  add_library(miniupnpc ALIAS libminiupnpc-static)
  # Vendored headers live flat in include/, but code uses #include <miniupnpc/...>.
  # Create a miniupnpc/ subdir layout so <miniupnpc/miniupnpc.h> resolves.
  set(MINIUPNPC_INCLUDE_DIR "${CMAKE_BINARY_DIR}/include")
  file(MAKE_DIRECTORY "${MINIUPNPC_INCLUDE_DIR}/miniupnpc")
  file(GLOB MINIUPNPC_HEADERS "${MINIUPNPC_VENDORED}/include/*.h")
  foreach(HEADER ${MINIUPNPC_HEADERS})
    get_filename_component(HEADER_NAME ${HEADER} NAME)
    configure_file(${HEADER} "${MINIUPNPC_INCLUDE_DIR}/miniupnpc/${HEADER_NAME}" COPYONLY)
  endforeach()
  # miniupnpcstrings.h is generated into the miniupnpc build dir
  set(MINIUPNPC_GEN_STR "${CMAKE_BINARY_DIR}/miniupnpc_vendored/miniupnpcstrings.h")
  if(EXISTS "${MINIUPNPC_GEN_STR}")
    configure_file(${MINIUPNPC_GEN_STR} "${MINIUPNPC_INCLUDE_DIR}/miniupnpc/miniupnpcstrings.h" COPYONLY)
  endif()
else()
  # Fall back to system-installed miniupnpc
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
    message(FATAL_ERROR "miniupnpc not found. Download source to third_party/miniupnpc or install libminiupnpc-dev")
  endif()
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
target_include_directories(raknet_proxy PRIVATE ${RAKNET_SRC_DIR} ${MINIUPNPC_INCLUDE_DIR})
if(WIN32)
  # Windows: WinSock2 + miniupnpc; no pthread needed
  target_link_libraries(raknet_proxy RakNetLibStatic miniupnpc ws2_32)
else()
  target_link_libraries(raknet_proxy RakNetLibStatic miniupnpc pthread)
endif()
