# YouTube

YouTube implements the AZ-MODS source-provider callbacks for catalog browsing, keyword search, and preparing a track for load. The core owns the deck’s screens, controls, load flow, and analysis. This mod requires a core that exposes `register_provider` and `set_storage_mode` through `source_service(AZ_SOURCE_ABI)` on XDJ-AZ firmware 2.00.

**Downloads** lists files cached by previous versions on the mod’s USB stick. Keyword searches return up to 24 entries per query. The bundled downloader supports keyword search and individual videos. YouTube account and remote playlists are unavailable in this implementation.

During load, the mod stages a copy of a legacy download or downloads a compatible AAC-LC stream and remuxes it to M4A without re-encoding. The completed file goes in the core’s staging directory.

**YouTube storage** in the Mod Menu applies to subsequent loads. **Streaming** is the default and keeps the audio for playback. **Import** saves it in the USB’s rekordbox library.

## Build

Run `make` with the AZ-MODS ARM64 toolchain and the SDK headers. Run `make unit` on the host for provider and catalog tests, and `make unit-helper` on Linux for the helper tests. Set `SAN_FLAGS=-fsanitize=address,undefined` and use a fresh `OUT` to run the tests with sanitizers.

Build the ARM64 downloader with `make downloader YTCUI_SOURCE=/path/to/ytcui-dl-0.6.1` in an ARM64 Linux environment with CMake, Ninja, Python, and static OpenSSL and zlib. This links the mod’s USB publication code into the downloader so both stages work on the deck’s FUSE exFAT driver. Use the same `OUT` for the next step, or pass the stripped binary as `YTCUI_DL`.

Run `make stage HELPER_BUNDLE=/path/to/ytcui-dl-xdj-az-arm64.tar.gz` with the AZ-MODS ARM64 toolchain. The archive supplies FFmpeg and the component licenses. On the host, run `../../az-mods-sdk/tools/azmod-pack package out/package --out dist`. The bundled cMCP JSON reader matches the copy used by Deck MCP, including its duplicate-key rejection patch. [Deck MCP dependencies](../deck-mcp/vendor/dependencies.mk)
