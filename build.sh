#!/bin/sh
# Build Marble Mac for the Simulator and the device, into marblemac.pdx.
#
# Environment overrides:
#   PLAYDATE_SDK_PATH  Playdate SDK (default ~/Developer/PlaydateSDK)
#   ARM_TOOLCHAIN_BIN  directory holding arm-none-eabi-gcc (default: the
#                      newest /Applications/ArmGNUToolchain, else PATH)
#   SDKROOT            macOS SDK for the Simulator build
#   MARBLE_SIM=0       skip the Simulator build (device only)
set -e
cd "$(dirname "$0")"

# This machine's default macOS SDK doesn't link with the installed ld;
# 26.5 does.  Homebrew's arm-none-eabi-gcc has no libc, so prefer Arm's.
if [ "$(uname)" = Darwin ] && [ -z "$SDKROOT" ] &&
   [ -d /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk ]; then
	export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
fi
ARMBIN="${ARM_TOOLCHAIN_BIN:-$(ls -d /Applications/ArmGNUToolchain/*/arm-none-eabi/bin 2>/dev/null | tail -1)}"
[ -n "$ARMBIN" ] && export PATH="$ARMBIN:$PATH"
SDK="${PLAYDATE_SDK_PATH:-$HOME/Developer/PlaydateSDK}"
export PLAYDATE_SDK_PATH="$SDK"
PDC=pdc
[ -x "$SDK/bin/pdc" ] && PDC="$SDK/bin/pdc"

[ -f external/umac/external/Musashi/m68kops.c ] || make -C external/umac prepare

if [ "${MARBLE_SIM:-1}" != 0 ]; then
	cmake -S . -B build/sim -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build build/sim
fi
cmake -S . -B build/device -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$SDK/C_API/buildsupport/arm.cmake" >/dev/null
cmake --build build/device

"$PDC" -sdkpath "$SDK" Source marblemac.pdx
echo "Built marblemac.pdx"
