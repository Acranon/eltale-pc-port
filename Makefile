CC ?= gcc
PYTHON ?= python3
ROM ?= $(ELTALE_ROM)

OUT := out/p0
GAME_SRC := $(filter-out work/src/game/audio/audio.c,$(shell find work/src/game -name '*.c')) \
  $(shell find work/src/overlays/intro -name '*.c')
HOST_SRC := port/p0/boot_probe.c port/p0/host.c port/p0/stubs.c port/p0/unavailable.c
GAME_OBJ := $(patsubst work/src/%.c,$(OUT)/obj/game/%.o,$(GAME_SRC))
HOST_OBJ := $(patsubst port/p0/%.c,$(OUT)/obj/host/%.o,$(HOST_SRC))
OBJ := $(GAME_OBJ) $(HOST_OBJ)

CPPFLAGS := -D_LANGUAGE_C -D_MIPS_SZLONG=32 -DF3DEX_GBI -DNDEBUG -D_FINALROM \
  -DBUILD_VERSION=VERSION_J -DNON_MATCHING -DELTALE_P0_BOOT_PROBE \
  -I$(OUT)/include -I$(OUT) -Iport/include -I. -Iwork/src -Idecomp/include -Idecomp/src \
  -idirafter decomp/include/ultra64 -idirafter decomp/include/ultra64/PR \
  -include port/addresses.h -include game/actor/actor.h
CFLAGS := -std=gnu11 -O2 -g -mcmodel=large -ffp-contract=off -MMD -MP -ffunction-sections -fdata-sections -fno-pie \
  -fno-stack-protector -Wno-builtin-declaration-mismatch \
  -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
LDFLAGS := -no-pie -Wl,--gc-sections -lm

.PHONY: p0 p0-test endian-test abi-test census assets clean
p0: assets $(OUT)/eltale-p0

assets:
	@test -n "$(ROM)" || (echo 'Set ROM=/path/to/baserom.jp.z64 or ELTALE_ROM' && exit 2)
	$(PYTHON) tools/check_rom.py "$(ROM)"
	$(PYTHON) decomp/tools/extract_assets.py decomp/assets.yaml "$(ROM)" $(OUT)/assets

$(OUT)/include/PR/gbi.h: decomp/include/ultra64/PR/gbi.h tools/gen_host_gbi.py
	$(PYTHON) tools/gen_host_gbi.py

$(OUT)/include/port/addresses.h: decomp/symbol_addrs.txt tools/gen_addresses.py
	$(PYTHON) tools/gen_addresses.py $(OUT)/include/port/addresses.h

$(OUT)/obj/game/%.o: work/src/%.c $(OUT)/include/PR/gbi.h $(OUT)/include/port/addresses.h | assets
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/obj/host/unavailable.o: port/p0/unavailable.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -c $< -o $@

$(OUT)/obj/host/%.o: port/p0/%.c $(OUT)/include/PR/gbi.h $(OUT)/include/port/addresses.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/abs.rsp: $(OBJ) tools/gen_abs_symbols.py
	$(PYTHON) tools/gen_abs_symbols.py $@ $(OBJ)

$(OUT)/eltale-p0: $(OBJ) $(OUT)/abs.rsp
	$(CC) $(OBJ) @$(OUT)/abs.rsp $(LDFLAGS) -o $@

p0-test: p0 abi-test endian-test
	$(OUT)/eltale-p0 --rom "$(ROM)" --headless --fast

endian-test: tests/endian_test.c port/include/port/endian.h
	@mkdir -p $(OUT)
	$(CC) -std=gnu11 -O2 -Iport/include tests/endian_test.c -o $(OUT)/endian-test
	$(OUT)/endian-test

abi-test: tests/abi_test.c $(OUT)/include/PR/gbi.h $(OUT)/include/port/addresses.h
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/abi_test.c -o $(OUT)/abi-test
	$(OUT)/abi-test

census:
	@mkdir -p $(OUT)
	$(PYTHON) tools/port_census.py > $(OUT)/census.json

clean:
	rm -rf $(OUT)

-include $(OBJ:.o=.d)
