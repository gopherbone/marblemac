cmake_minimum_required(VERSION 3.14)
set(CMAKE_C_STANDARD 11)

set(ENVSDK $ENV{PLAYDATE_SDK_PATH})

if (NOT ${ENVSDK} STREQUAL "")
	file(TO_CMAKE_PATH ${ENVSDK} SDK)
else()
	execute_process(
			COMMAND bash -c "egrep '^\\s*SDKRoot' $HOME/.Playdate/config"
			COMMAND head -n 1
			COMMAND cut -c9-
			OUTPUT_VARIABLE SDK
			OUTPUT_STRIP_TRAILING_WHITESPACE
	)
endif()

if (NOT EXISTS ${SDK})
	message(FATAL_ERROR "SDK Path not found; set ENV value PLAYDATE_SDK_PATH")
	return()
endif()

set(CMAKE_CONFIGURATION_TYPES "Debug;Release")

set(PLAYDATE_GAME_NAME marblemac)
set(PLAYDATE_GAME_DEVICE marblemac_DEVICE)

project(${PLAYDATE_GAME_NAME} C ASM)

# Mac config: 4MB "Mac Plus", so System 6/7 boot
set(UMAC_MEMSIZE 4096 CACHE STRING "Emulated Mac RAM in KB")

set(UMAC ${CMAKE_CURRENT_SOURCE_DIR}/external/umac)
set(MUSASHI ${UMAC}/external/Musashi)

if (NOT EXISTS ${MUSASHI}/m68kops.c)
	message(FATAL_ERROR "Run 'make -C external/umac prepare' first to generate Musashi's opcode tables")
endif()

set(SOURCES
	src/main.c
	src/cursor.c
	src/macglue.c
	src/vdisk.c
	src/bench.c
	src/tuning.c
	src/sfx.c
	src/dust.c
	src/wallfx.c
	src/clickfx.c
	src/bungee.c
	src/lander.c
	src/jit/m68kjit.c
	src/jit/m68knative.c
	src/jit/m68kinterp.c
	src/jit_playdate.c
	${UMAC}/src/main.c
	${UMAC}/src/disc.c
	${UMAC}/src/rom.c
	${UMAC}/src/scc.c
	${UMAC}/src/via.c
	${MUSASHI}/m68kcpu.c
	${MUSASHI}/m68kops.c
	${MUSASHI}/m68kdasm.c
	${MUSASHI}/softfloat/softfloat.c
)

if (TOOLCHAIN STREQUAL "armgcc")
	add_executable(${PLAYDATE_GAME_DEVICE} ${SOURCES} src/device_stubs.c)
	set(TARGET ${PLAYDATE_GAME_DEVICE})
else()
	add_library(${PLAYDATE_GAME_NAME} SHARED ${SOURCES})
	set(TARGET ${PLAYDATE_GAME_NAME})
endif()

target_include_directories(${TARGET} PRIVATE src src/jit ${UMAC}/include ${MUSASHI})
target_compile_definitions(${TARGET} PRIVATE
	MUSASHI_CNF="${UMAC}/include/m68kconf.h"
	ENABLE_DASM=1
	UMAC_MEMSIZE=${UMAC_MEMSIZE}
	DISP_WIDTH=512
	DISP_HEIGHT=342
	# Playdate runs from PSRAM with small caches; see docs in README
	M68K_OPCODE_CACHE_BITS=10
)
if (TOOLCHAIN STREQUAL "armgcc")
	# 68k -> Thumb-2 JIT (device only: the simulator isn't ARM M-profile)
	# Tiering between the interpreter (src/jit/m68kinterp.c) and the JIT,
	# via CMAKE_C_FLAGS: -DJIT_TIER=0 (interpret only), 1 (always JIT, as
	# before), 2 (JIT hot loops) or 3 (JIT hot code, the default);
	# -DJIT_HOT_THRESHOLD=n, -DJIT_EXIT_HEAT=n, -DJIT_HEAT_DECAY=n (see m68kjit.c)
	option(MARBLE_JIT "Use the 68k JIT on the device" ON)
	if (MARBLE_JIT)
		target_compile_definitions(${TARGET} PRIVATE UMAC_JIT=1)
	endif()
	# Code runs from PSRAM through a 16KB I-cache, so for the CPU core
	# smaller code beats "faster" code, and the hot parts are packed
	# together (see src/m68k_platform.h)
	set_source_files_properties(${MUSASHI}/m68kops.c ${MUSASHI}/m68kcpu.c ${UMAC}/src/main.c
		PROPERTIES COMPILE_OPTIONS "-Os;-falign-functions=4;-include;${CMAKE_CURRENT_SOURCE_DIR}/src/m68k_platform.h")
endif()
target_compile_options(${TARGET} PRIVATE -O2 -Wno-unused-parameter)

option(MARBLE_BENCH "Log memory/cache measurements on the device" OFF)
if (MARBLE_BENCH)
	target_compile_definitions(${TARGET} PRIVATE MARBLE_BENCH=1 M68K_COUNT_INSTRUCTIONS=1)
endif()

include(${SDK}/C_API/buildsupport/playdate_game.cmake)
