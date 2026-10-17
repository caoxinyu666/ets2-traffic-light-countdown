#!/usr/bin/env bash
# Cross-compile the countdown HUD plugin for Windows x64.
#
# Zig ships its own libc / libc++ and MinGW headers, so this needs no Windows
# toolchain installed. Set ZIG to a zig binary, or let it find one on PATH.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD="${THIRD:-$ROOT/third_party/deps}"
IMGUI="$THIRD/imgui-1.91.9b"
MINHOOK="$THIRD/minhook-1.3.4"
SCSINC="$THIRD/scs-include"
ZIG="${ZIG:-zig}"

OUT="$ROOT/plugins"
mkdir -p "$OUT/build"

echo "zig: $ZIG"
echo "imgui: $IMGUI"

# MinHook's C sources are compiled separately from the C++ plugin.
for unit in buffer hook trampoline hde/hde64; do
  object="$OUT/build/$(echo "$unit" | tr '/' '_').o"
  "$ZIG" cc --target=x86_64-windows-gnu -O2 -c "$MINHOOK/src/$unit.c" -I"$MINHOOK/include" -o "$object"
done

"$ZIG" c++ --target=x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -static -shared \
  "$ROOT/src/plugin.cpp" "$ROOT/src/plugin.def" \
  "$IMGUI/imgui.cpp" "$IMGUI/imgui_draw.cpp" "$IMGUI/imgui_tables.cpp" "$IMGUI/imgui_widgets.cpp" \
  "$IMGUI/backends/imgui_impl_dx11.cpp" \
  "$OUT/build/buffer.o" "$OUT/build/hook.o" "$OUT/build/trampoline.o" "$OUT/build/hde_hde64.o" \
  -I"$IMGUI" -I"$IMGUI/backends" -I"$MINHOOK/include" -I"$SCSINC" -I"$ROOT/src" \
  -ld3d11 -ldxgi -ld3dcompiler_47 -luser32 -lgdi32 -limm32 \
  -o "$OUT/TrafficLightTimer.dll"

ls -la "$OUT/TrafficLightTimer.dll"
