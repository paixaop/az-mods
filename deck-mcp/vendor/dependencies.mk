# SPDX-License-Identifier: GPL-3.0-only
DEPS ?= $(abspath ../../temp/deck-mcp-deps)
SDK_IMAGE ?= az-mods-sdk:1
SHA256 ?= shasum -a 256
JOBS ?= 8

CMCP_REV := af886a13d8ed21cb3db75b9716d87bdfd6c84c8f
EVENT_VERSION := 2.1.13-stable
SSL_VERSION := 3.5.9
CMCP_SHA := 1c094b6a98e3336844adc54d4d236a8ba7af29bf4f6e0974089d87d888f1cccc
EVENT_SHA := f7e9383b8c0baa81b687e5b5eecc01beefaf1b19b64151d95ed61647fe7a315c
SSL_SHA := 603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a

.DELETE_ON_ERROR:
.PHONY: fetch arm-libraries
fetch: $(DEPS)/cmcp/.patched $(DEPS)/libevent/.patched $(DEPS)/openssl/Configure

$(DEPS):
	mkdir -p "$@"

$(DEPS)/cmcp.tar.gz: | $(DEPS)
	curl -fLsS "https://codeload.github.com/Simon-oid/cMCP/tar.gz/$(CMCP_REV)" -o "$@"
	printf '%s  %s\n' "$(CMCP_SHA)" "$@" | $(SHA256) -c -

$(DEPS)/libevent.tar.gz: | $(DEPS)
	curl -fLsS "https://github.com/libevent/libevent/releases/download/release-$(EVENT_VERSION)/libevent-$(EVENT_VERSION).tar.gz" -o "$@"
	printf '%s  %s\n' "$(EVENT_SHA)" "$@" | $(SHA256) -c -

$(DEPS)/openssl.tar.gz: | $(DEPS)
	curl -fLsS "https://github.com/openssl/openssl/releases/download/openssl-$(SSL_VERSION)/openssl-$(SSL_VERSION).tar.gz" -o "$@"
	printf '%s  %s\n' "$(SSL_SHA)" "$@" | $(SHA256) -c -

$(DEPS)/cmcp/.patched: $(DEPS)/cmcp.tar.gz vendor/cmcp.patch
	mkdir -p "$(@D)"
	tar -xf "$<" -C "$(@D)" --strip-components 1
	patch -s -d "$(@D)" -p1 -i "$(CURDIR)/vendor/cmcp.patch"
	touch "$@"

$(DEPS)/libevent/.patched: $(DEPS)/libevent.tar.gz vendor/libevent.patch
	mkdir -p "$(@D)"
	tar -xf "$<" -C "$(@D)" --strip-components 1
	patch -s -d "$(@D)" -p1 -i "$(CURDIR)/vendor/libevent.patch"
	touch "$@"

$(DEPS)/openssl/Configure: $(DEPS)/openssl.tar.gz
	mkdir -p "$(@D)"
	tar -xf "$<" -C "$(@D)" --strip-components 1
	touch "$@"

arm-libraries: fetch
	docker run --rm --platform linux/arm64 --user root -v "$(DEPS):/deps" "$(SDK_IMAGE)" sh -ec '\
	    apt-get update && apt-get install -y --no-install-recommends perl; \
	    cd /deps/openssl; \
	    ./Configure linux-aarch64 no-shared no-tests no-module no-dso no-legacy no-asm \
	        -fPIC --prefix=/deps/arm --libdir=lib; \
	    make -j $(JOBS) build_libs; make install_dev; \
	    cd /deps/libevent; \
	    CPPFLAGS=-I/deps/arm/include LDFLAGS=-L/deps/arm/lib LIBS="-lssl -lcrypto -ldl -pthread" \
	        CFLAGS="-O2 -fPIC" ./configure --disable-shared --enable-static --disable-samples \
	        --disable-libevent-regress --prefix=/deps/arm; \
	    make -j $(JOBS); make install'
