#!/bin/sh
# Builds and runs the OG routing check (see fuzz_routing_og.cpp). Run from anywhere.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
J=$(cd "$HERE/../.." && pwd)
OUT=${TMPDIR:-/tmp}/fuzz_routing_og
mkdir -p "$OUT"
# The router is compiled through a symlink so its own directory's headers
# (States.h, NetManager.h, ...) lose to the stubs in include order.
ln -sf "$J/src/routing/NetsToChipConnections_OG.cpp" "$OUT/router_og.cpp"
ln -sf "$J/src/routing/RouteSafety.cpp" "$OUT/routesafety.cpp"
g++ -std=c++17 -O1 -DOG_JUMPERLESS -include config.h \
    -I "$HERE/stubs" -I "$J/src/routing" -I "$J/src" -I "$J/src/sensing" \
    -o "$OUT/fuzz_routing_og" \
    "$HERE/fuzz_routing_og.cpp" "$OUT/router_og.cpp" "$OUT/routesafety.cpp" \
    "$J/src/boards/board.cpp" "$J/src/boards/og/board_og.cpp" "$J/src/boards/v5/board_v5.cpp" \
    2>&1 | grep -E "error" && exit 1
"$OUT/fuzz_routing_og" regress
"$OUT/fuzz_routing_og" 5000 1 12
ROWBIAS=75 "$OUT/fuzz_routing_og" 5000 2 8
