#!/bin/sh
set -e

# Detect compiler
if [ -n "$TERMUX_VERSION" ] || [ -d "/data/data/com.termux" ]; then
    CC="gcc-15"
    CXX="gcc-15"
    echo "[build] Termux detected, using gcc-15"
else
    CC="gcc"
    CXX="gcc"
    echo "[build] Using default gcc"
fi

command -v "$CC" >/dev/null 2>&1 || { echo "Error: $CC not found"; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

MINIUPNPC_DIR="RakNet/DependentExtensions/miniupnpc-1.5"
RAKNET_SRC="RakNet/Source"
OUTPUT="raknet_proxy"

MINIUPNPC_SRCS="connecthostport.c igd_desc_parse.c minisoap.c minissdpc.c miniupnpc.c miniwget.c minixml.c upnpcommands.c upnperrors.c upnpreplyparse.c"

cp patch/* $RAKNET_SRC

echo "[build] Compiling miniupnpc..."
for src in $MINIUPNPC_SRCS; do
    $CC -c -O2 "$MINIUPNPC_DIR/$src" \
        -I "$MINIUPNPC_DIR" -I "$RAKNET_SRC"
done

echo "[build] Creating libminiupnpc.a..."
OBJ_FILES=$(echo "$MINIUPNPC_SRCS" | sed 's/\.c/.o/g')
ar rcs libminiupnpc.a $OBJ_FILES

echo "[build] Compiling raknet_proxy..."
$CXX -O2 -o "$OUTPUT" \
    raknet_proxy.cpp \
    $RAKNET_SRC/*.cpp \
    libminiupnpc.a \
    -I "$RAKNET_SRC" \
    -I "$MINIUPNPC_DIR" \
    -lpthread -lstdc++ -std=c++11 \
    -DPREALLOCATE_LARGE_MESSAGES=1 # 非 PREALLOCATE 路径下，分片 Insert 追加到列表末尾，重组按列表顺序拼接，而不是按 splitPacketIndex，导致重组后的数据顺序错乱，proxy 把乱序数据转发给 server，PREALLOCATE_LARGE_MESSAGES=1 路径是正确的

echo "[build] Cleaning up..."
rm -f $OBJ_FILES libminiupnpc.a

echo "[build] Done: $OUTPUT"
