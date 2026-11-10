# SPDX-License-Identifier: GPL-3.0-only
AZ_SDK ?= ../../az-mods-sdk
AZ_SDK_INCLUDE ?= $(AZ_SDK)/include
OUT ?= out
SRCS := track-analysis.c inflate.c vendor/miniz/miniz_tinfl.c
CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -fPIC -fvisibility=hidden \
          -fno-omit-frame-pointer -fexceptions -fasynchronous-unwind-tables -I$(AZ_SDK_INCLUDE)
.PHONY: all test worker clean
all: $(OUT)/track-analysis.so
$(OUT)/track-analysis.so: $(SRCS) $(AZ_SDK_INCLUDE)/azmod-mod.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -static-libgcc -Wl,--no-undefined -Wl,--hash-style=gnu -o $@ $(SRCS)
test:
	TRACK_ANALYSIS_MODELS="$(CURDIR)/worker/Sources/Analysis/Resources/NativeBeatModels" swift test -c release --package-path worker
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -I$(AZ_SDK_INCLUDE) tests/test_runtime.c inflate.c vendor/miniz/miniz_tinfl.c -o /tmp/track-analysis-test
	/tmp/track-analysis-test
	python3 tests/test_inflate.py
HOST_CC ?= clang
worker:
	sh tools/build-worker.sh
clean:
	rm -rf $(OUT)
