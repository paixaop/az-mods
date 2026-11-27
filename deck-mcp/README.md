# Deck MCP

Deck MCP serves `https://<deck-address>:8765/mcp` from an SDK worker inside EP147. It uses cMCP for MCP, JSON, JSON-RPC and schema validation, with Libevent and Mbed TLS for HTTPS. It supports stateless Streamable HTTP with the 2026-07-28 and 2025-11-25 protocol revisions. Each POST returns JSON or an empty notification acknowledgement. [Streamable HTTP specification](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)

## First run

1. Enable Deck MCP with the installation USB inserted. Put the deck and the computer running Codex on the same LAN.
2. Open **Mod Menu → Community Mods → Installed → Deck MCP → Settings → Pair with Codex**.
3. Open the displayed HTTPS address in the laptop browser. If the browser warns about the deck's certificate, view the server certificate and compare its SHA-256 fingerprint with the one on the deck before entering the code. Ignore capitalization, colons and line breaks when comparing.
4. Enter the six-digit code shown on the deck, then select **Download connection file**. Keep the deck's pairing screen open until the file is sent. **New setup code** replaces an expired code. Each code lasts two minutes, allows five attempts, and can download the file once.
5. Ask local Codex on that laptop to set up and verify Deck MCP using `deck-mcp-connection.json` in Downloads. Tell it to follow the Deck MCP README and keep the token private. Reopen Codex when setup finishes.

The downloaded file contains the MCP URL, a bearer token and the public CA for the deck. **Back** cancels the code. **Reset token** disarms live controls and saved edits, saves a new token and displays a new setup code. Re-pair each client after a reset. A failed save leaves the previous token active. The local CA stays on the installation USB and continues to work if the deck's IP changes. Reload the mod and pair again to update the URL after an address change.

### Configuration instructions for a local coding assistant

Read the connection file locally. Keep its token out of chat, terminal output and shell arguments. Validate that `version` is 1, `transport` is `streamable-http`, and `url` uses HTTPS with the deck's address, port and the path `/mcp`. The `bearerToken` must be 64 hexadecimal characters.

Store the authorization header as a private JSON file containing the `Authorization` key with the value `Bearer <bearerToken>`. Save the public `caCertificatePem` separately if it is present. Use absolute paths in the user's Codex settings, updating just the Deck MCP entry. On macOS and Linux, a header helper can read the JSON using `/bin/cat`.

```toml
[mcp_servers.deck-mcp]
url = "https://<deck-address>:8765/mcp"
http_headers_helper = "/bin/cat '/absolute/path/to/deck-mcp-authorization.json'"
```

Configure Codex to use the saved public PEM. When the local Codex CLI or app-server loads its `~/.codex/.env` and has no CA override, set `SSL_CERT_FILE` in that file to the PEM's absolute path. If the process already uses `CODEX_CA_CERTIFICATE` or `SSL_CERT_FILE`, preserve the existing certificates and include the deck CA in the bundle it uses. When launching Codex directly, you can pass `CODEX_CA_CERTIFICATE` in its environment. A custom certificate deployment can return a null CA, in which case Codex uses its existing trust configuration. Restart any running Codex process so it reads the change, verify that the server lists tools with full TLS verification, and delete the downloaded connection file. [Codex MCP configuration](https://developers.openai.com/codex/mcp)

## Custom network or certificate

For an existing or managed setup, provide `server.json` and your PEM certificate chain and unencrypted private key as `server.crt` and `server.key` in the mod's data directory. Use an IPv4 bind address covered by the certificate or use `"bind": "auto"` for automatic private or link-local IPv4 selection. An explicit configuration without a `bind` value defaults to `127.0.0.1`. Set `origin` to the exact allowed origin if your client sends an `Origin` header. Disable and re-enable the mod when replacing these files.

```json
{
  "bind": "127.0.0.1",
  "port": 8765,
  "token": "<64 hexadecimal characters>"
}
```

For clients that require a legacy handshake, use 2025-11-25. Modern clients send the version and method headers on each POST and mirror the tool name on `tools/call`. The mod uses a single provisioned bearer credential.

## Tools

| Tool | Behavior |
| --- | --- |
| `deck_capabilities` | Returns the listener session ID, firmware, requested observation topics and current local arm generation. |
| `deck_read_state` | Reads logical deck 1–4. Reports field receipts for load, track metadata, key, transport, audible and Slip positions, tempo, loop, Sync, preferences, phase and stems. |
| `deck_read_cues` | Pages the main, live hot, live memory, saved hot and saved memory banks. Pages after offset zero require the previous page's load epoch, revision and provider ID. |
| `deck_loop` | Marks IN or OUT, enters, exits or re-enters a loop, clears retained points, changes the active length, moves a loop, or adjusts an individual boundary. |
| `deck_cue` | Main, hot and memory cue actions, saved-loop actions, color and comment edits, and explicit point or loop creation. |
| `deck_native_cues` | Reads the native bank's target IDs and exact editing revision. |
| `deck_apply_cue_bank` | Applies a complete bank on a compatible core. Replacement asks for confirmation on the deck. |
| `deck_command_status`, `deck_cancel_command` | Read a receipt by key or request cancellation of one pending operation. |
| `deck_library` | Queues USB playlist enumeration, track listing, text search and track-reference resolution. Poll `status` before starting the next lookup. |
| `deck_library_edit` | Reads exact occurrences in a USB playlist and moves one entry to a final one-based position. Requires the playlist editing SDK extension. |
| `deck_read_live_analysis` | Reads the current or saved beatgrid or phrase map of a loaded deck. Use the load epoch and carry the revision and provider ID to later pages. |
| `deck_read_track_analysis`, `deck_read_waveform` | Request, read and release USB tickets for stored metadata, cues, grid, phrases, overview and waveform windows. |
| `deck_read_vocals`, `deck_analysis` | Read vocal intervals and manage native analysis jobs. |
| `deck_ui_state`, `deck_input` | Read the current screen and send navigation or logical-layer panel buttons. The receipt proves delivery. |
| `deck_azmods_settings` | Reads official Mod Menu settings. On the extended core, queues compare-and-set writes and reports status. |
| `deck_provider_status` | Polls deferred tool calls from an approved community mod. |

In **Deck MCP → Settings**, tap **Arm live controls** for live actions or **Arm saved edits** for storage actions. The two grants last up to 15 minutes and can be disarmed from the same page. For cue and loop submissions, pass the current `sessionId`, deck and `loadEpoch`, plus a unique `key`. Set `armGeneration` from `deck_capabilities.armGeneration` for live actions and from `deck_capabilities.savedArmGeneration` for edits. Use the exact revision from `deck_native_cues` for saved targets. Reuse the key and arguments together when retrying. Query `deck_command_status` after queueing to distinguish acceptance from application.

Command, panel, official-setting and provider receipts remain available for 60 seconds after completion. During that window, retrying the same key with the same arguments returns the existing receipt. After the window, submitting the key again can start a fresh operation. Check the current state before resubmitting an action whose receipt has expired.

## Reorder a USB playlist

Enable **Playlist Editing** on the deck and tap **Arm saved edits** in Deck MCP Settings before submitting a move.

1. Call `deck_library_edit` with `action: "getOrder"`, `usb` (1 or 2), and the `playlistId` from `deck_library`. Poll `action: "status"` with the returned `sessionId` and `id`. For later order pages, carry over `revision`, `sourceGeneration` and `libraryId` with `nextOffset`.
2. Call `action: "moveTrack"` with the session, USB, playlist, the selected row's `entryId` and `fromPosition`, the desired final `toPosition`, the three order fields, and `armGeneration` from `deck_capabilities.savedArmGeneration`. Positions start at 1. Duplicate tracks have separate entry IDs.
3. Poll `status` for the move result. `outcome: "moved"` confirms the occurrence at the new position. `deviceLibrary: "failed"` means OneLibrary was edited but the separate `export.pdb` sync failed. Read the order again after an uncertain result or a stale revision. The library worker retains the most recent operation.

## Community mod tools

Allow providers individually in `server.json`. The values `read`, `live`, and `saved` set the highest scope each provider can publish. Local arming applies to each write.

```json
"providers": {
  "cues": "saved",
  "anchor": "live",
  "beatport-remember": "read"
}
```

- **Cues** publishes `cues.preview`, `cues.apply_to_deck`, rule and group tools, global settings, and configuration validation, apply and save tools. Rule and group inputs use the [Cues JSON format](../cues/examples/cue-rules.json). Read a fresh `configVersion` before editing. A local editing draft takes precedence and causes the remote edit to return `AZ_BUSY`. Track application reports that it entered the Cues workflow. Observe the native bank to verify the result.
- **Anchor** publishes `anchor.read_guide`, `anchor.list_targets`, `anchor.set_target`, `anchor.get_settings`, and `anchor.set_settings`.
- **Beatport Remember** publishes `beatport-remember.status`, `beatport-remember.set_options`, and `beatport-remember.forget`. The last asks for confirmation on the deck.

Each provider call returns a `sessionId` and `operationId`. Use both with `deck_provider_status` to read the result. Provider writes also require a client-generated `key`.

## Build for ARM64

From the mod directory, with the public SDK build image installed:

```sh
deps="$(cd ../.. && pwd)/temp/deck-mcp-deps"
sdk="$(cd ../../az-mods-sdk && pwd)"
make -f vendor/dependencies.mk arm-libraries DEPS="$deps"
docker run --rm --platform linux/arm64 \
  -v "$PWD:/mod:ro" -v "$sdk/include:/sdk/include:ro" -v "$deps:/deps" \
  -w /mod az-mods-sdk:1 make AZ_SDK_INCLUDE=/sdk/include OUT=/deps/out
mkdir -p out
cp "$deps/out/deck-mcp.so" out/
"$sdk/tools/azmod-pack" validate .
"$sdk/tools/azmod-pack" package .
```

`vendor/dependencies.mk` pins the archives and checks their SHA-256 hashes. The build uses the Mbed TLS backend from Libevent 2.2.2-alpha. The cMCP patch rejects duplicate JSON keys. The Libevent patch closes a connection when creation of its TLS context fails or all 12 connection slots are occupied. The package carries dependency notices under `assets/licenses/`.

To build a provider-enabled Cues, Anchor or Beatport Remember mod, mount this directory's `provider/` at `/provider` alongside the pinned dependency mount, and run that mod's Makefile with `MCP_PROVIDER_DIR=/provider`. Include the cMCP license notice from `assets/licenses/cMCP.txt` under the provider package's `assets/licenses/`. The provider SDK links per mod and serves registrations only within the same EP147 process. See [az-mcp-provider.h](provider/az-mcp-provider.h) for the C interface.

For host testing, run `make -f vendor/dependencies.mk host-libraries DEPS="$deps"` with CMake 3.20.2 or later. Run `make unit` with `DEPS="$deps"`, `TLS_PREFIX="$deps/host"`, `OUT`, and `FIXTURE` set. `EVENT_PREFIX` defaults to `TLS_PREFIX`. The fixture directory contains a certificate with IP SAN `127.0.0.1`, its key, and the test listener configuration. The C integration test defines the fixture token and port. It runs the production listener and SDK adapter against a fake SDK. `TEST_CURL_FLAGS` selects the test libcurl and `TEST_FLAGS` accepts sanitizer flags.
