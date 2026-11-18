# SPDX-License-Identifier: GPL-3.0-only
.DEFAULT_GOAL := all
AZ_SDK ?= ../../az-mods-sdk
AZ_SDK_INCLUDE ?= $(AZ_SDK)/include
OUT ?= out
CC = cc
AR ?= ar
PYTHON ?= python3
SANITIZE ?=
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -I$(AZ_SDK_INCLUDE) \
            -Ianalysis -Ianalysis/dsp/include -Ivendor/faad/include -Ivendor/faad/libfaad -Ivendor/mimalloc/include
CFLAGS += -std=c11 -O2 -Wall -Wextra -Werror -fPIC -fvisibility=hidden \
          -ffp-contract=off -fno-omit-frame-pointer -fexceptions -fasynchronous-unwind-tables -pthread
LDLIBS += -lm -pthread
ifneq ($(SANITIZE),)
CFLAGS += -g -fsanitize=$(SANITIZE) -fno-sanitize-recover=all
LDFLAGS += -fsanitize=$(SANITIZE)
endif
FAAD_FLAGS := -DLC_ONLY_DECODER -DHAVE_INTTYPES_H=1 -DHAVE_MEMCPY=1 -DHAVE_STRING_H=1 \
              -DHAVE_STRINGS_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_LRINTF=1 \
              -DPACKAGE_VERSION=\"2.11.4\"
ENGINE_SRC := $(wildcard analysis/*.c) analysis/dsp/decode.c analysis/dsp/aac.c \
              analysis/dsp/dsp.c analysis/dsp/phrase_filters.c $(wildcard vendor/faad/libfaad/*.c) \
              vendor/mimalloc/src/static.c $(wildcard vendor/musl-math/*.c)
ENGINE_OBJ := $(patsubst %.c,$(OUT)/%.o,$(ENGINE_SRC))
# Retain the original worker's release optimization for its DSP/codec kernels.
$(OUT)/analysis/vocals.o $(filter $(OUT)/analysis/dsp/% $(OUT)/vendor/faad/%,$(ENGINE_OBJ)): CFLAGS += -O3
ifeq ($(shell $(CC) -dumpmachine | cut -d- -f1),aarch64)
CFLAGS += -march=armv8-a -mtune=cortex-a53
endif
.PHONY: all test clean
all: $(OUT)/track-analysis.so
$(OUT)/track-analysis.so: $(OUT)/track-analysis.o $(ENGINE_OBJ) exports.map
	$(CC) -shared $(LDFLAGS) -static-libgcc -Wl,--no-undefined -Wl,--hash-style=gnu -Wl,--version-script=exports.map -o $@ $(filter %.o,$^) $(LDLIBS)
$(OUT)/vendor/faad/%.o: vendor/faad/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(FAAD_FLAGS) $(filter-out -Wall -Wextra -Werror,$(CFLAGS)) -MMD -MP -c $< -o $@
$(OUT)/vendor/mimalloc/src/static.o: vendor/mimalloc/src/static.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(filter-out -Wall -Wextra -Werror,$(CFLAGS)) -DNDEBUG -DMI_STATIC_LIB \
	  $(if $(findstring address,$(SANITIZE)),-DMI_TRACK_ASAN=1) -ftls-model=local-dynamic -MMD -MP -c $< -o $@
$(OUT)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(OUT)/analysis/dsp/decode.o $(OUT)/analysis/dsp/aac.o: CFLAGS += -Wno-error
$(OUT)/fixtures.c: tests/generate_fixtures.py $(wildcard tests/fixtures/*.json)
	$(PYTHON) $< $@
$(OUT)/test_phrases: $(OUT)/fixtures.c $(ENGINE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O0 $(LDFLAGS) $^ $(LDLIBS) -o $@
$(OUT)/test_runtime: tests/test_runtime.c track-analysis.c analysis/analysis.h $(AZ_SDK_INCLUDE)/azmod-mod.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@
$(OUT)/test_analysis: tests/test_analysis.c $(ENGINE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
$(OUT)/test_execution: tests/test_execution.c analysis/execution.c $(OUT)/analysis/memory.o $(OUT)/vendor/mimalloc/src/static.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(OUT)/analysis/memory.o $(OUT)/vendor/mimalloc/src/static.o $(LDLIBS) -o $@
$(OUT)/test_vocal_kernels: tests/test_vocal_kernels.c analysis/vocals.c $(ENGINE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(LDFLAGS) $< $(filter-out $(OUT)/analysis/vocals.o,$(ENGINE_OBJ)) $(LDLIBS) -o $@
$(OUT)/test_vocal_frontend: tests/test_vocal_frontend.c analysis/vocals.c $(ENGINE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(LDFLAGS) $< $(filter-out $(OUT)/analysis/vocals.o,$(ENGINE_OBJ)) $(LDLIBS) -o $@
$(OUT)/test_parallel_analysis: tests/test_parallel_analysis.c $(ENGINE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
$(OUT)/analysis_probe: tests/analysis_probe.c $(ENGINE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
$(OUT)/test_activation_math: tests/test_activation_math.c tests/fixtures/musl-activation-bits.inc $(filter $(OUT)/vendor/musl-math/%,$(ENGINE_OBJ))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(filter %.o,$^) $(LDLIBS) -o $@
test: $(OUT)/test_phrases $(OUT)/test_runtime $(OUT)/test_execution $(OUT)/test_vocal_kernels $(OUT)/test_vocal_frontend $(OUT)/test_analysis $(OUT)/test_parallel_analysis $(OUT)/test_activation_math
	$(OUT)/test_phrases
	$(OUT)/test_runtime
	$(OUT)/test_execution
	$(OUT)/test_vocal_kernels
	$(OUT)/test_vocal_frontend
	$(OUT)/test_activation_math
	$(OUT)/test_analysis tests/fixtures assets
	$(OUT)/test_parallel_analysis tests/fixtures assets
clean:
	rm -rf $(OUT)
-include $(ENGINE_OBJ:.o=.d) $(OUT)/track-analysis.d
