CC ?= gcc
PYTHON ?= python3
ROM ?= $(ELTALE_ROM)

OUT ?= out/p0
P1_OS ?= 0
AUDIO ?= 1
FRAMES ?= 1
INPUT ?=
PAK ?=
BIN := $(OUT)/$(if $(filter 1,$(P1_OS)),eltale-p1,eltale-p0)
GAME_SRC := $(filter-out work/src/game/audio/audio.c,$(shell find work/src/game -name '*.c')) \
  $(shell find work/src/overlays/intro -name '*.c')
HOST_SRC := port/p0/boot_probe.c port/p0/host.c port/p0/stubs.c port/p0/unavailable.c
OS_SRC :=
GU_SRC :=
ifeq ($(P1_OS),1)
GAME_SRC += $(shell find work/src/overlays/title -name '*.c')
OS_SRC += port/os/fiber.c port/os/kernel.c port/os/controller.c port/os/pak.c port/os/math_glue.c port/data/map_area.c port/data/anim.c port/gfx/frame_image.c port/data/common.c port/data/enemy.c port/gfx/vi.c port/gfx/rt64_bridge.c
GU_SRC += $(wildcard decomp/src/libultra/gu/*.c)
VI_SRC := decomp/src/libultra/io/vitbl.c
endif
AUDIO_SRC :=
ifeq ($(P1_OS)$(AUDIO),11)
GAME_SRC += work/src/game/audio/audio.c
OS_SRC += port/audio/bank.c port/audio/aspmain.c
AUDIO_SRC += $(wildcard work/src/libultra/audio/*.c)
CPPFLAGS_AUDIO := -DELTALE_P1_AUDIO
endif
GAME_OBJ := $(patsubst work/src/%.c,$(OUT)/obj/game/%.o,$(GAME_SRC))
HOST_OBJ := $(patsubst port/p0/%.c,$(OUT)/obj/host/%.o,$(HOST_SRC))
OS_OBJ := $(patsubst port/%.c,$(OUT)/obj/port/%.o,$(OS_SRC))
GU_OBJ := $(patsubst decomp/src/libultra/gu/%.c,$(OUT)/obj/gu/%.o,$(GU_SRC))
VI_OBJ := $(patsubst decomp/src/libultra/io/%.c,$(OUT)/obj/io/%.o,$(VI_SRC))
AUDIO_OBJ := $(patsubst work/src/libultra/audio/%.c,$(OUT)/obj/audio/%.o,$(AUDIO_SRC))
BASE_OBJ := $(GAME_OBJ) $(HOST_OBJ) $(OS_OBJ) $(GU_OBJ) $(VI_OBJ) $(AUDIO_OBJ)
ifeq ($(P1_OS),1)
MAP_OBJ := $(OUT)/host_address_map.o
endif
OBJ := $(BASE_OBJ) $(MAP_OBJ)

CPPFLAGS := -D_LANGUAGE_C -D_MIPS_SZLONG=32 -DF3DEX_GBI -DNDEBUG -D_FINALROM \
  -DBUILD_VERSION=VERSION_J -DNON_MATCHING -DELTALE_P0_BOOT_PROBE \
  -I$(OUT)/include -I$(OUT) -Iport/include -I. -Iwork/src -Idecomp/include -Idecomp/src \
  -idirafter decomp/include/ultra64 -idirafter decomp/include/ultra64/PR \
  -include port/addresses.h -include game/actor/actor.h
ifeq ($(P1_OS),1)
CPPFLAGS += -DELTALE_P1_OS $(CPPFLAGS_AUDIO)
endif
CFLAGS := -std=gnu11 -O2 -g -mcmodel=large -ffp-contract=off -fno-strict-aliasing -fwrapv -MMD -MP -ffunction-sections -fdata-sections -fno-pie \
  -fno-stack-protector -Wno-builtin-declaration-mismatch \
  -Werror=int-to-pointer-cast -Werror=pointer-to-int-cast -Werror=int-conversion
LDFLAGS := -no-pie -Wl,-Ttext-segment=0x10000000 -Wl,--gc-sections -lm -ldl

.PHONY: p0 p0-test p1-os-run p1-os-test endian-test abi-test census assets clean
p0: assets $(BIN)

assets:
	@test -n "$(ROM)" || (echo 'Set ROM=/path/to/baserom.jp.z64 or ELTALE_ROM' && exit 2)
	$(PYTHON) tools/check_rom.py "$(ROM)"
	$(PYTHON) decomp/tools/extract_assets.py decomp/assets.yaml "$(ROM)" $(OUT)/assets

$(OUT)/include/PR/gbi.h: decomp/include/ultra64/PR/gbi.h tools/gen_host_gbi.py
	$(PYTHON) tools/gen_host_gbi.py $@

$(OUT)/include/port/addresses.h: decomp/symbol_addrs.txt tools/gen_addresses.py
	$(PYTHON) tools/gen_addresses.py $(OUT)/include/port/addresses.h

$(OUT)/obj/game/%.o: work/src/%.c $(OUT)/include/PR/gbi.h $(OUT)/include/port/addresses.h | assets
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/obj/host/unavailable.o: port/p0/unavailable.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 $(if $(filter 1,$(P1_OS)),-DELTALE_P1_OS,) -c $< -o $@

$(OUT)/obj/host/%.o: port/p0/%.c $(OUT)/include/PR/gbi.h $(OUT)/include/port/addresses.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/obj/port/%.o: port/%.c $(OUT)/include/PR/gbi.h $(OUT)/include/port/addresses.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/obj/audio/%.o: work/src/libultra/audio/%.c $(OUT)/include/PR/gbi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -D_LANGUAGE_C -D_MIPS_SZLONG=32 -DF3DEX_GBI -I$(OUT)/include -Iport/include -Idecomp/include -Iwork/src 		-idirafter decomp/include/ultra64 -idirafter decomp/include/ultra64/PR 		-include port/include/PR/ultratypes.h -c $< -o $@

$(OUT)/obj/io/%.o: decomp/src/libultra/io/%.c $(OUT)/include/PR/gbi.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -D_LANGUAGE_C -D_MIPS_SZLONG=32 -I$(OUT)/include -Iport/include -Idecomp/include -Idecomp/src 		-idirafter decomp/include/ultra64 -idirafter decomp/include/ultra64/PR 		-include port/include/PR/ultratypes.h -c $< -o $@

$(OUT)/obj/gu/%.o: decomp/src/libultra/gu/%.c $(OUT)/include/PR/gbi.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -ffp-contract=off -D_LANGUAGE_C -D_MIPS_SZLONG=32 -DF3DEX_GBI \
		-I$(OUT)/include -Iport/include -Idecomp/include -Idecomp/src \
		-idirafter decomp/include/ultra64 -idirafter decomp/include/ultra64/PR \
		-include port/include/PR/ultratypes.h -c $< -o $@

$(OUT)/host_address_map.c: $(GAME_OBJ) tools/gen_host_address_map.py
	$(PYTHON) tools/gen_host_address_map.py $@ $(GAME_OBJ)

$(OUT)/host_address_map.o: $(OUT)/host_address_map.c
	$(CC) -std=gnu11 -O2 -c $< -o $@

$(OUT)/abs.rsp: $(OBJ) tools/gen_abs_symbols.py
	$(PYTHON) tools/gen_abs_symbols.py $@ $(OBJ)

$(BIN): $(OBJ) $(OUT)/abs.rsp
	$(CC) $(OBJ) @$(OUT)/abs.rsp $(LDFLAGS) -o $@

p0-test: p0 abi-test endian-test
	$(BIN) --rom "$(ROM)" --headless --fast

p1-os-run: p0 abi-test endian-test p1-os-test
	$(BIN) --rom "$(ROM)" --headless --fast --frames $(FRAMES) $(if $(INPUT),--input "$(INPUT)",) $(if $(PAK),--pak "$(PAK)",)

endian-test: tests/endian_test.c port/include/port/endian.h
	@mkdir -p $(OUT)
	$(CC) -std=gnu11 -O2 -Iport/include tests/endian_test.c -o $(OUT)/endian-test
	$(OUT)/endian-test

abi-test: tests/abi_test.c $(OUT)/include/PR/gbi.h $(OUT)/include/port/addresses.h
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/abi_test.c -o $(OUT)/abi-test
	$(OUT)/abi-test

p1-os-test: $(OUT)/include/PR/gbi.h $(OUT)/include/port/addresses.h
	$(CC) -std=gnu11 -O2 -Iport/include -I. tests/fiber_test.c port/os/fiber.c -o $(OUT)/fiber-test
	$(OUT)/fiber-test
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/os_scheduler_test.c port/os/kernel.c port/os/fiber.c -o $(OUT)/os-scheduler-test
	$(OUT)/os-scheduler-test
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/os_events_test.c port/os/kernel.c port/os/fiber.c -o $(OUT)/os-events-test
	$(OUT)/os-events-test
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/gu_test.c decomp/src/libultra/gu/mtxutil.c decomp/src/libultra/gu/sins.c -o $(OUT)/gu-test
	$(OUT)/gu-test
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/pak_test.c port/os/pak.c -o $(OUT)/pak-test
	$(OUT)/pak-test $(OUT)/test.mpk

census:
	@mkdir -p $(OUT)
	$(PYTHON) tools/port_census.py > $(OUT)/census.json

clean:
	rm -rf $(OUT)

-include $(OBJ:.o=.d)
