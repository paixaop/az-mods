# Testing — what was verified, and on what

Target: a real XDJ-AZ on **AZ-MODS 1.2.4**, firmware tag
`ep147-9e9454e357389ece` (stock firmware 2.00, EP147 MD5
`8daa1de55d7328f458f6d855cdc3cdde`), reached over the root SSH access that
AZ-MODS enables from the USB. Mod build: `beatport-remember 0.1.0`,
`.so` built and validated by the SDK's `linux/arm64` container.

## Verified on the deck

| Check | Result |
|---|---|
| `azmod-pack build` / `validate` | `validate …: OK` (one export, aarch64, allowed libraries, ≤16 MB) |
| Load | `/tmp/azmod-community/loaded` shows `mod=beatport-remember|1|0.1.0|` (loaded, not suspended) |
| Firmware gate | `init` reads `fw_tag()` = `ep147-9e9454e357389ece` and matches the manifest |
| Hook 1 | `claim_hook 0x1b8e3a8 ok` (login controller constructor) |
| Hook 2 | `claim_hook 0x1b8dfa8 ok` (button dispatch / LOG IN) |
| Init | returns 0; mod log: `loaded on ep147-9e9454e357389ece (auto=1 remember=1 creds=0)` |
| Login-screen hook fires | mod log: `ctor self=… widget=…` when the Beatport login screen is created |
| Prefill runs | with a synthetic login on the deck, mod log: `filled the Beatport login with the saved credentials`; EP147 did not crash |
| Capture path | guarded by `process_vm_readv`; no crash observed on LOG IN presses during testing |
| Clean state | final deploy has no stored credentials (`creds=0` in the log) and the test login was removed |

Two crashes were found and fixed during this testing, both in the mod:

1. Reading a guessed `.bss` slot (`0x3fee0e98`, a cached button-vtable pointer)
   to identify the LOG IN button — the address was not mapped in the crashed
   process. Replaced with the known widget member `+0x118`.
2. Reusing `x16` across two calls to `TextEditor::getText` in inline assembly:
   the first call clobbers `x16`, so the second jumped to a garbage address.
   Replaced with a naked trampoline that loads the constant itself.

The first two short EP147 restarts after those crashes left the AZ-MODS
quarantine markers behind; they were cleared (`/home/root/settings/azmod-community/.starting`,
`.crashes`, `beatport-remember.crashed`), which is what the deck's **Resume** /
**Try again** buttons do.

## Not yet verified (needs a real Beatport account)

- The saved values actually appearing in the two fields **on screen** after
  re-opening the login screen (the synthetic test proved `setText` runs on a
  live login widget without a crash, but the screen could not be photographed
  in the right state at that point).
- One full round trip: type real credentials once → LOG IN → power-cycle →
  open Beatport → fields prefilled → LOG IN succeeds.
- Beatport authentication errors, wrong credentials, and a changed password.
- Two- and four-deck views, Day and dark themes on the mod's page.

## How to finish the test on the deck

```sh
# logs on (the mod's own lines need this file; the core's own lines always write)
ssh root@DECK 'touch /home/root/settings/azmod-logs.on'

# then on the deck: open Beatport, log in once. Read:
ssh root@DECK 'cat /tmp/azmod-community/beatport-remember.log'
# expect: loaded … creds=0 / ctor … / saved Beatport login for <user>

# power-cycle, open Beatport again:
# expect: loaded … creds=1 / ctor … / filled the Beatport login with the saved credentials
# and the two fields filled. Press LOG IN as usual.

# inspect / remove what was stored
ssh root@DECK 'cat /home/root/settings/azmod-community/beatport-remember.dat'   # hex
# or use Mod Menu → Community Mods → Installed → Beatport Remember → Forget saved login
```

If something goes wrong, the mod can be removed without touching firmware:

```sh
# on the USB (or from a computer): remove the folder and its cfg key
rm -rf  /media/usb/sda1/MODS/community-mods/beatport-remember
sed -i '/^community-mods\.beatport-remember/d' /media/usb/sda1/MODS/azmods.cfg
# and on the deck after a reboot:
rm -f /home/root/settings/azmod-community/beatport-remember.dat
```

A crashing mod is suspended by AZ-MODS on the next start (`Suspended: it
crashed the deck`); its page offers **Try again**. Two short restarts in a row
hold back all community mods until **Resume**; the mod's code never runs when
suspended.

## Reproducing the reverse engineering

All addresses were found in the EP147 copied from the deck
(`/home/root/pdj/EP147`, MD5 `8daa1de5…`). Useful starting points, if the
firmware changes:

- the widget name strings (`userNameText`, `passwordText`, `loginBtn`,
  `BeatportLoginWidgetController`) locate the login widget cluster around
  `0x1b8a000`–`0x1b8e400`;
- its constructor stores the primary vtable `0x2d8d5e8` and the editor member
  offsets `0xf0`/`0xf8`, and the controller constructor stores the widget at
  `+0x38`;
- the controller's vtable at `0x2d91bd0` slot 11 is the button callback; its
  LOG IN path calls `getText` on the two editors and passes the strings to the
  Beatport session.

The mod deliberately hooks only two functions and calls three known EP147
functions; anything else is plain C in the mod.

## Fixes in 0.1.2 / 0.1.3 (2026-10-20, from a real-deck report)

Three defects found after the first real use:

1. **Saved login was garbage (`b83e4f03`).** 0.1.0 read the text from a guessed
   `juce::TextEditor` member (`+0x288`); that member is not the field's text.
   Capture now calls EP147's own `juce::TextEditor::getText` (`0x263f4e0`), reads
   the returned `juce::String` (one pointer to UTF-8), destroys it with EP147's
   destructor, rejects control characters, and ignores malformed saved data.
2. **LOG IN press froze the deck.** The assembly trampoline that calls `getText`
   did `blr x16; ret`; `blr` overwrites `x30`, so `ret` branched to itself and
   spun the UI thread. The trampoline now saves/restores `x29`/`x30` (with CFI)
   around the call. Symptom was a hang on the first login-screen button press
   (eye/mask or LOG IN) after capture was switched to `getText`.
3. **Streaming tracks stayed on "downloading" forever.** EP147's
   `cipher_createSharedMemory` (`0xed0cc0`) does `ftok("/etc",0x14)` +
   `shmget(key, 0x340, IPC_CREAT|IPC_EXCL|0600)` and treats `EEXIST` as fatal,
   but EP147 leaves the segment behind on exit. Every later EP147 start (AZ-MODS
   Reload or restart) then fails to initialize the DRM cipher: login/browsing
   work, encrypted streaming never decrypts. The mod now removes an unattached
   stale 0x340-byte mode-0600 segment at load (and in its init callback).
   Verified: two consecutive EP147 restarts, no new
   `cipher_createSharedMemory` errors, and the segment is recreated cleanly.

The deck was left with no saved login and the corrected 0.1.3 build installed.

## 0.1.4 / 0.1.5: auto-login and the page menu

- **Auto login** (0.1.4): after filling the two fields, the mod schedules the
  LOG IN button's dispatch call a few ticks later. It presses only when both
  fields still hold exactly the saved values, so text the DJ has started
  typing is never submitted. One attempt per login screen; the manual button
  remains available if the automatic attempt fails.
- **One Mod Menu entry** (0.1.5): the three top-level rows were replaced with a
  single "Beatport Remember" row that opens the mod page. The page now has
  "Auto login" and "Remember a login typed on the deck" checkboxes (tap to
  toggle, saved with cfg_set) and a "Forget saved login" button that deletes
  the stored credentials immediately. BACK returns to the Mod Menu.
- The log showed `filled …` then `pressing LOG IN automatically`, EP147 stayed
  responsive, and no cipher error was added across restarts. **That was not
  proof of a working press**: see 0.1.6 below. Synthetic credentials were then
  removed.

## 0.1.6: the automatic press never reached the LOG IN branch

Reported from the deck: *"remember works but the autologin is not; I have to
press the login button manually every time I reboot."* The fields were filled,
so capture and `setText` were fine — only the press did nothing.

**Cause.** 0.1.4/0.1.5 called the dispatch function as
`orig_btn_dispatch(controller, button)` — a `juce::Button*` where EP147 expects
a pointer to a `juce::String` holding the pressed widget's **name**. The
function opens with `ldr x0, [x1]` (the name's first word) and pointer-compares
it against four `juce::String` globals built by `_INIT_1546` (`0x5716d0`) in
`.bss`: `0x3feee80` `userNameDelBtn`, `0x3feee88` `passwordDelBtn`, `0x3feee90`
`passwordMaskBtn`, `0x3feee98` `loginBtn`. Given a `Button*`, that word is the
button's vtable pointer, no comparison matches, and the function returns at the
first `ret` — a silent no-op. A real finger-press passes a name String whose
word does match, which is also why credential capture (hooked on the same
address) had always worked.

**Fix.** The press now passes the address of EP147's own `loginBtn` String
(`0x3feee98`). `*param_2` then equals `DAT_03feee98` by construction and the
`loginBtn` branch at `0x1b8e008` runs. Fine print, all checked in the
disassembly:

- the callee only reads that one word; it never stores, copies or destroys the
  String, so passing EP147's own object is safe and needs no refcount;
- the `loginBtn` branch takes nothing else from the argument: it reads the
  button state, the two editors and the controller members itself, so the
  press is the same call a finger makes;
- the branch is a toggle: `*(int *)(loginBtn + 0x1e0)` `0` = LOG IN, `1` =
  LOG OUT. The mod now reads it and presses only in state `0`, so a deck that
  is already signed in is never signed out;
- `g_ctrl` is the right `this`: the controller's vtable (`0x2d91bd0`) holds the
  dispatcher at slot 11 (`0x2d91c28`), which is the vtable the mod checks for
  liveness.

`.bss` covers `0x3feee98` (the rw segment ends at `0x40c9520`). The earlier
"unmapped `.bss`" crash came from a mistyped address (`0x3fee0e98`, far outside
the segment), not from this one.

**Not yet verified on the deck** (the deck was offline during this build): that
one press with real credentials signs in, and that it is stable across repeated
boots. The build logs the whole attempt so a single test decides it:

```
filled the Beatport login with the saved credentials
pressing LOG IN automatically (button state 0, name <holder>)
LOG IN dispatch returned
after auto login: login screen is gone        <- or "button state 1"
```

If the first and second lines appear but the fourth is missing or says
`state 0`, the press reached the branch but the session did not start, and the
next step is to read the login branch's own downstream calls rather than the
mod's plumbing.
