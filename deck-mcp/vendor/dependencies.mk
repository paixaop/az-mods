# SPDX-License-Identifier: GPL-3.0-only
DEPS ?= $(abspath ../../temp/deck-mcp-deps)
SDK_IMAGE ?= az-mods-sdk:1
SHA256 ?= shasum -a 256
CMAKE ?= cmake
JOBS ?= 8

CMCP_REV := af886a13d8ed21cb3db75b9716d87bdfd6c84c8f
EVENT_VERSION := 2.2.2-alpha
MBED_VERSION := 4.1.1
CMAKE_VERSION := 3.31.6
CMCP_SHA := 1c094b6a98e3336844adc54d4d236a8ba7af29bf4f6e0974089d87d888f1cccc
EVENT_SHA := 4ab1b369bcb5af0c5971b8ade4e95a2c1326f6d0dc1ba75d620bf0331c3184a8
MBED_SHA := 3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c
CMAKE_SHA := b4cc788d63112b2749b40627e719eb5d3b8ed8f00c36d77189f4019cfe64bc9e
EVENT := libevent-$(EVENT_VERSION)
MBED := mbedtls-$(MBED_VERSION)
ARM_CMAKE := cmake-$(CMAKE_VERSION)-linux-aarch64
COMMON_FLAGS := -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_LIBDIR=lib
MBED_FLAGS := -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF
EVENT_FLAGS := -DEVENT__LIBRARY_TYPE=STATIC -DEVENT__DISABLE_OPENSSL=ON -DEVENT__DISABLE_MBEDTLS=OFF \
               -DEVENT__DISABLE_TESTS=ON -DEVENT__DISABLE_BENCHMARK=ON -DEVENT__DISABLE_SAMPLES=ON

.DELETE_ON_ERROR:
.PHONY: fetch arm-libraries host-libraries
fetch: $(DEPS)/cmcp/.patched $(DEPS)/$(EVENT)/.patched $(DEPS)/$(MBED)/CMakeLists.txt

$(DEPS):
	mkdir -p "$@"

$(DEPS)/cmcp.tar.gz: | $(DEPS)
	curl -fLsS "https://codeload.github.com/Simon-oid/cMCP/tar.gz/$(CMCP_REV)" -o "$@"
	printf '%s  %s\n' "$(CMCP_SHA)" "$@" | $(SHA256) -c -

$(DEPS)/$(EVENT).tar.gz: | $(DEPS)
	curl -fLsS "https://github.com/libevent/libevent/releases/download/release-$(EVENT_VERSION)/$(EVENT).tar.gz" -o "$@"
	printf '%s  %s\n' "$(EVENT_SHA)" "$@" | $(SHA256) -c -

$(DEPS)/$(MBED).tar.bz2: | $(DEPS)
	curl -fLsS "https://github.com/Mbed-TLS/mbedtls/releases/download/$(MBED)/$(MBED).tar.bz2" -o "$@"
	printf '%s  %s\n' "$(MBED_SHA)" "$@" | $(SHA256) -c -

$(DEPS)/$(ARM_CMAKE).tar.gz: | $(DEPS)
	curl -fLsS "https://cmake.org/files/v3.31/$(ARM_CMAKE).tar.gz" -o "$@"
	printf '%s  %s\n' "$(CMAKE_SHA)" "$@" | $(SHA256) -c -

$(DEPS)/cmcp/.patched: $(DEPS)/cmcp.tar.gz vendor/cmcp.patch
	mkdir -p "$(@D)"
	tar -xf "$<" -C "$(@D)" --strip-components 1
	patch -s -d "$(@D)" -p1 -i "$(CURDIR)/vendor/cmcp.patch"
	touch "$@"

$(DEPS)/$(EVENT)/.patched: $(DEPS)/$(EVENT).tar.gz vendor/libevent.patch
	mkdir -p "$(@D)"
	tar -xf "$<" -C "$(@D)" --strip-components 1
	patch -s -d "$(@D)" -p1 -i "$(CURDIR)/vendor/libevent.patch"
	touch "$@"

$(DEPS)/$(MBED)/CMakeLists.txt: $(DEPS)/$(MBED).tar.bz2
	mkdir -p "$(@D)"
	tar -xf "$<" -C "$(@D)" --strip-components 1
	touch "$@"

$(DEPS)/$(ARM_CMAKE)/bin/cmake: $(DEPS)/$(ARM_CMAKE).tar.gz
	mkdir -p "$(DEPS)/$(ARM_CMAKE)"
	tar -xf "$<" -C "$(DEPS)/$(ARM_CMAKE)" --strip-components 1
	touch "$@"

arm-libraries: fetch $(DEPS)/$(ARM_CMAKE)/bin/cmake
	docker run --rm --platform linux/arm64 -v "$(DEPS):/deps" "$(SDK_IMAGE)" sh -ec '\
	    cmake=/deps/$(ARM_CMAKE)/bin/cmake; \
	    "$$cmake" -S /deps/$(MBED) -B /deps/build-mbed-arm $(COMMON_FLAGS) $(MBED_FLAGS) -DCMAKE_INSTALL_PREFIX=/deps/arm; \
	    "$$cmake" --build /deps/build-mbed-arm -j $(JOBS); "$$cmake" --install /deps/build-mbed-arm; \
	    "$$cmake" -S /deps/$(EVENT) -B /deps/build-event-arm $(COMMON_FLAGS) $(EVENT_FLAGS) \
	        -DCMAKE_INSTALL_PREFIX=/deps/arm -DMBEDTLS_ROOT_DIR=/deps/arm -DMBEDTLS_USE_STATIC_LIBS=ON; \
	    "$$cmake" --build /deps/build-event-arm -j $(JOBS); "$$cmake" --install /deps/build-event-arm'

host-libraries: fetch
	$(CMAKE) -S "$(DEPS)/$(MBED)" -B "$(DEPS)/build-mbed-host" $(COMMON_FLAGS) $(MBED_FLAGS) -DCMAKE_INSTALL_PREFIX="$(DEPS)/host"
	$(CMAKE) --build "$(DEPS)/build-mbed-host" -j $(JOBS)
	$(CMAKE) --install "$(DEPS)/build-mbed-host"
	$(CMAKE) -S "$(DEPS)/$(EVENT)" -B "$(DEPS)/build-event-host" $(COMMON_FLAGS) $(EVENT_FLAGS) \
	    -DCMAKE_INSTALL_PREFIX="$(DEPS)/host" -DMBEDTLS_ROOT_DIR="$(DEPS)/host" -DMBEDTLS_USE_STATIC_LIBS=ON
	$(CMAKE) --build "$(DEPS)/build-event-host" -j $(JOBS)
	$(CMAKE) --install "$(DEPS)/build-event-host"
