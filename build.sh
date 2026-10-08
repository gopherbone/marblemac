#!/bin/sh
# Build Marble Mac for the Simulator and the device, into marblemac.pdx.
set -e
cd "$(dirname "$0")"

# This machine's default macOS SDK doesn't link with the installed ld;
# 26.5 does.  Homebrew's arm-none-eabi-gcc has no libc, so prefer Arm's.
export SDKROOT="${SDKROOT:-/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk}"
ARMBIN=$(ls -d /Applications/ArmGNUToolchain/*/arm-none-eabi/bin 2>/dev/null | tail -1)
[ -n "$ARMBIN" ] && export PATH="$ARMBIN:$PATH"
SDK="${PLAYDATE_SDK_PATH:-$HOME/Developer/PlaydateSDK}"

[ -f external/umac/external/Musashi/m68kops.c ] || make -C external/umac prepare

cmake -S . -B build/sim -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/sim
cmake -S . -B build/device -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$SDK/C_API/buildsupport/arm.cmake" >/dev/null
cmake --build build/device

pdc -sdkpath "$SDK" Source marblemac.pdx
echo "Built marblemac.pdx"
