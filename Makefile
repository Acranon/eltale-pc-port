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
ifeq ($(SANITIZE),1)
HOST_SRC += port/p0/asan_rdram.c
endif
OS_SRC :=
GU_SRC :=
ifeq ($(P1_OS),1)
GAME_SRC += $(shell find work/src/overlays/title -name '*.c')
GAME_SRC += $(shell find work/src/overlays/ending -name '*.c')
GAME_SRC += $(shell find work/src/overlays/pakmenu -name '*.c')
OS_SRC += port/os/fiber.c port/os/kernel.c port/os/controller.c port/os/pak.c port/os/math_glue.c port/data/map_area.c port/data/anim.c port/gfx/frame_image.c port/data/common.c port/data/enemy.c port/data/magic.c port/gfx/vi.c port/gfx/host_bridge.c port/p0/quicksave.c port/p0/mouse_camera.c port/p0/mouse_menu.c port/p0/spell_ui.c port/text/en_text.c port/text/en_images.c
# the native renderer's game side: one file per kind of draw (docs/renderer/NATIVE_DRAW.md)
ND_SRC := $(wildcard game/native_draw/*.c)
# libultra's sinf/cosf keep their double constants as big-endian word pairs: the host libm's instead
GU_SRC += $(filter-out %/sinf.c %/cosf.c,$(wildcard decomp/src/libultra/gu/*.c))
VI_SRC := decomp/src/libultra/io/vitbl.c
endif
AUDIO_SRC :=
ifeq ($(P1_OS)$(AUDIO),11)
GAME_SRC += work/src/game/audio/audio.c
OS_SRC += port/audio/bank.c port/audio/aspmain.c port/audio/rsp_lle.c port/rsp/rsp.c
AUDIO_SRC += $(wildcard work/src/libultra/audio/*.c)
CPPFLAGS_AUDIO := -DELTALE_P1_AUDIO
endif
GAME_OBJ := $(patsubst work/src/%.c,$(OUT)/obj/game/%.o,$(GAME_SRC))
HOST_OBJ := $(patsubst port/p0/%.c,$(OUT)/obj/host/%.o,$(HOST_SRC))
OS_OBJ := $(patsubst port/%.c,$(OUT)/obj/port/%.o,$(OS_SRC))
ND_OBJ := $(patsubst game/native_draw/%.c,$(OUT)/obj/native_draw/%.o,$(ND_SRC))
GU_OBJ := $(patsubst decomp/src/libultra/gu/%.c,$(OUT)/obj/gu/%.o,$(GU_SRC))
VI_OBJ := $(patsubst decomp/src/libultra/io/%.c,$(OUT)/obj/io/%.o,$(VI_SRC))
AUDIO_OBJ := $(patsubst work/src/libultra/audio/%.c,$(OUT)/obj/audio/%.o,$(AUDIO_SRC))
BASE_OBJ := $(GAME_OBJ) $(HOST_OBJ) $(OS_OBJ) $(ND_OBJ) $(GU_OBJ) $(VI_OBJ) $(AUDIO_OBJ)
ifeq ($(P1_OS),1)
MAP_OBJ := $(OUT)/host_address_map.o
# English text and pictures, made from the ROM (tools/build_windows.py does the same)
P1_DATA := $(OUT)/text/en.tsv $(OUT)/en_images.inc
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

# The memory-safety build (docs/MEMORY_SAFETY.md): AddressSanitizer with UBSan's bound and pointer checks,
# clang on Linux. `make asan ROM=...` -> out/asan/eltale-p1 (or tools/asan_run.sh). Reports go on; none stops
# the game. Instrumented mem* calls go through port/p0/asan_rdram.c, which knows RDRAM.
SANITIZE ?= 0
SAN_CFLAGS :=
ifeq ($(SANITIZE),1)
SAN_FLAGS := -fsanitize=address,array-bounds,pointer-overflow,object-size,vla-bound,shift-exponent,float-cast-overflow,builtin,return,unreachable,bool,enum,nonnull-attribute,returns-nonnull-attribute
SAN_CFLAGS := $(SAN_FLAGS) -fsanitize-recover=all -fno-omit-frame-pointer   -mllvm -asan-memory-access-callback-prefix=eltale_asan_ -mllvm -asan-instrumentation-with-call-threshold=1000000000
CFLAGS += $(SAN_CFLAGS)
LDFLAGS += $(SAN_FLAGS)
endif

.PHONY: asan p0 p0-test p1-os-run p1-os-test endian-test abi-test census assets clean linux
p0: assets $(BIN)

assets:
	@test -n "$(ROM)" || (echo 'Set ROM=/path/to/baserom.jp.z64 or ELTALE_ROM' && exit 2)
	$(PYTHON) tools/check_rom.py "$(ROM)"
	$(PYTHON) decomp/tools/extract_assets.py decomp/assets.yaml "$(ROM)" $(OUT)/assets

$(OUT)/text/en.tsv: translations/en.v2.tsv decomp/assets.yaml tools/text/runtime_text.py
	$(PYTHON) tools/text/runtime_text.py translations/en.v2.tsv decomp/assets.yaml $@

$(OUT)/en_images.inc: $(wildcard tools/images/*.py tools/text/*.py) | assets
	@mkdir -p $(dir $@)
	$(PYTHON) tools/images/build_en_images.py "$(ROM)" $@

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

$(OUT)/obj/host/asan_rdram.o: port/p0/asan_rdram.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -g -fno-builtin -c $< -o $@

$(OUT)/obj/host/%.o: port/p0/%.c $(OUT)/include/PR/gbi.h $(OUT)/include/port/addresses.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/obj/port/%.o: port/%.c $(OUT)/include/PR/gbi.h $(OUT)/include/port/addresses.h $(P1_DATA)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/obj/native_draw/%.o: game/native_draw/%.c $(OUT)/include/PR/gbi.h $(OUT)/include/port/addresses.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/obj/audio/%.o: work/src/libultra/audio/%.c $(OUT)/include/PR/gbi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -D_LANGUAGE_C -D_MIPS_SZLONG=32 -DF3DEX_GBI -I$(OUT)/include -Iport/include -Idecomp/include -Iwork/src 		-idirafter decomp/include/ultra64 -idirafter decomp/include/ultra64/PR 		-include port/include/PR/ultratypes.h -c $< -o $@

$(OUT)/obj/io/%.o: decomp/src/libultra/io/%.c $(OUT)/include/PR/gbi.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -D_LANGUAGE_C -D_MIPS_SZLONG=32 -I$(OUT)/include -Iport/include -Idecomp/include -Idecomp/src 		-idirafter decomp/include/ultra64 -idirafter decomp/include/ultra64/PR 		-include port/include/PR/ultratypes.h $(SAN_CFLAGS) -c $< -o $@

$(OUT)/obj/gu/%.o: decomp/src/libultra/gu/%.c $(OUT)/include/PR/gbi.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -ffp-contract=off -D_LANGUAGE_C -D_MIPS_SZLONG=32 -DF3DEX_GBI \
		-I$(OUT)/include -Iport/include -Idecomp/include -Idecomp/src \
		-idirafter decomp/include/ultra64 -idirafter decomp/include/ultra64/PR \
		-include port/include/PR/ultratypes.h $(SAN_CFLAGS) -c $< -o $@

$(OUT)/host_address_map.c: $(GAME_OBJ) tools/gen_host_address_map.py
	$(PYTHON) tools/gen_host_address_map.py $@ $(GAME_OBJ)

$(OUT)/host_address_map.o: $(OUT)/host_address_map.c
	$(CC) -std=gnu11 -O2 $(SAN_CFLAGS) -c $< -o $@

$(OUT)/abs.rsp: $(OBJ) tools/gen_abs_symbols.py
	$(PYTHON) tools/gen_abs_symbols.py $@ $(OBJ)

$(BIN): $(OBJ) $(OUT)/abs.rsp $(P1_DATA)
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

# the memory-safety build (docs/MEMORY_SAFETY.md)
# (and tools/asan_host_stub.c as libeltale_host.so: --dump-frames runs the game's native draw side without a GPU)
asan:
	$(MAKE) P1_OS=1 SANITIZE=1 CC=$(or $(ASAN_CC),clang-18) OUT=$(or $(ASAN_OUT),out/asan) ROM="$(ROM)" p0 		$(or $(ASAN_OUT),out/asan)/libeltale_host.so

$(OUT)/libeltale_host.so: tools/asan_host_stub.c port/include/port/render.h port/include/port/automap.h
	$(CC) -std=gnu11 -O1 -g -fPIC -shared -fsanitize=address -fno-omit-frame-pointer -Iport/include $< -o $@

# Linux: out/p1-linux/eltale-p1 and libeltale_host.so (+ libSDL3.so.0) beside it (tools/build_linux.sh)
linux:
	bash tools/build_linux.sh --rom "$(ROM)"

census:
	@mkdir -p $(OUT)
	$(PYTHON) tools/port_census.py > $(OUT)/census.json

clean:
	rm -rf $(OUT)

-include $(OBJ:.o=.d)
