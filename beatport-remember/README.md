# Beatport Remember — an AZ-MODS community mod

Keeps the **Beatport Streaming** login on the XDJ-AZ. The deck asks for the
Beatport user name and password on its own login screen and forgets them when
it is powered off; this mod remembers them once and fills the screen for you
the next time.

Built with the [AZ-MODS SDK](https://github.com/Kyle-Hosman/az-mods-sdk), the
way the [xdj-az-mods](https://github.com/Kyle-Hosman/xdj-az-mods) repository
describes: a shared library, a manifest and an icon, packaged as one `.azmod`.

## What it does

1. **Remembers.** When you log in to Beatport on the deck and press **LOG IN**,
   the mod takes the two values from EP147's own login widget and stores them
   on the deck.
2. **Fills and signs in.** The next time the Beatport login screen is created,
   it writes the saved values into the user name and password fields (through
   EP147's own `juce::TextEditor::setText`), then presses **LOG IN** the way a
   finger would: by entering EP147's own button-dispatch function with the name
   of the LOG IN widget. Nothing is typed and nothing is tapped.
3. **Forgets.** The mod's page in **Mod Menu → Community Mods → Installed →
   Beatport Remember** has **Forget saved login**, plus switches for the two
   behaviours.

The automatic press happens only while both fields still hold exactly the saved
values, and only when the button is in its LOG IN state (state 1 is LOG OUT, so
pressing then would sign the DJ out). One attempt is made per login screen; the
manual button remains available if it does not take.

## Install

The mod targets **XDJ-AZ firmware 2.00** (EP147 tag
`ep147-9e9454e357389ece`) because it hooks two Beatport login functions in
EP147. AZ-MODS shows *Not made for this firmware* on any other build.

- On the deck: **Mod Menu → Community Mods → Repositories → Add**, with the
  repository that publishes this mod, then install and allow it.
- By hand: copy `beatport-remember/` with `azmod.json`, `beatport-remember.so`
  and `icon.png` into `<USB>/MODS/community-mods/beatport-remember/`, set
  `community-mods.beatport-remember = on` in `<USB>/MODS/azmods.cfg`, reload,
  then allow the new mod on the deck. Approval records the SHA-256 of the
  `.so`; a different build asks again.

Build and package the mod with the AZ-MODS SDK to create an installable package.

## How it works

Two hooks, claimed from `init()` with `claim_hook`, both in EP147's Beatport
login screen:

| EP147 2.00 address | What it is | Why the mod needs it |
|---|---|---|
| `0x1b8e3a8` | `gui::BeatportLoginWidgetController::BeatportLoginWidgetController(BeatportLoginWidget*)` | Runs when the login screen is built; gives the mod the controller and the widget pointers. |
| `0x1b8dfa8` | the controller's widget-dispatch callback (vtable slot 11) | Every button press on that screen passes through here; a press on **LOG IN** is when the typed values are read. |

The widget is a `BeatportLoginWidget` with a vtable at `0x2d8d5e8`. Its two
text editors are at `+0xf0` (user name) and `+0xf8` (password); the LOG IN
button is at `+0x118`. The controller keeps the widget at `+0x38`.

The mod calls three EP147 functions, found by following the widget's own
constructor and its LOG IN handler:

| EP147 address | Function |
|---|---|
| `0x2647670` | `juce::TextEditor::setText(const juce::String&, bool)` |
| `0x24a1180` | `juce::String::String(const char*)` |
| `0x2469aa0` | `juce::String::~String()` |

Both fields are read for capture through EP147's own
`juce::TextEditor::getText` (`0x263f4e0`), which returns a `juce::String` — one
pointer to its UTF-8 text in this build. The bytes are copied with
`process_vm_readv` on the mod's own process, so a wrong guess returns `EFAULT`
instead of crashing the deck, and the `String` is destroyed by EP147's own
destructor.

### Pressing LOG IN

The second hook is not a `juce::Button::Listener`, so it does not take the
pressed button. It takes a pointer to a `juce::String` holding the pressed
widget's **name** and pointer-compares that name against four `juce::String`
globals that EP147's static initialiser (`_INIT_1546`, `0x5716d0`) builds in
`.bss`:

| Address | Name | Effect |
|---|---|---|
| `0x3feee80` | `userNameDelBtn` | clears the user name field |
| `0x3feee88` | `passwordDelBtn` | clears the password field |
| `0x3feee90` | `passwordMaskBtn` | toggles the password mask |
| `0x3feee98` | `loginBtn` | reads both editors and starts the Beatport session |

So the automatic press passes the address of EP147's own `loginBtn` String
(`0x3feee98`): the callee's first instruction loads that word, the comparison
is then true by construction, and the `loginBtn` branch runs with the two
fields' text exactly as a real press would send it. The callee only reads that
word — it neither stores nor destroys the String.

Passing the `Button*` instead, as 0.1.4/0.1.5 did, loads the button's vtable
pointer where the name is expected, matches none of the four, and returns
immediately. That is why those builds filled the fields but never signed in.

Before pressing, the mod reads the button's state at `+0x1e0`: `0` is LOG IN,
`1` is LOG OUT/abort. It presses only in state `0`, so an already signed-in
deck is never signed out, and it checks that EP147's `loginBtn` String is
initialised rather than routing to the wrong branch.

### Storage

```
/home/root/settings/azmod-community/beatport-remember.dat
```

Two hex-encoded lines (`user=…`, `pass=…`, hex so spaces, `=` and newlines
round-trip), mode `0600`, written by the mod and removed by **Forget saved
login**. The clear-text value never leaves the deck. Use **Forget saved login**
before selling or handing on the deck: the settings partition is not cleared by
an AZ-MODS uninstall.

This is a convenience store, not a keychain: anyone with root on the deck can
read it. The mod is GPL-3.0-only and the `.so` is built from this source with
the SDK's pinned container, so the package can be rebuilt byte for byte.

## Source layout

```
beatport-remember.c   the mod (C, one translation unit)
azmod.json            manifest: id, version, firmware tag, hooks: true
icon.png              256x256
Makefile              SDK build rules (unchanged from the template)
dist/                 output of `azmod-pack package`
```

Build and check:

```sh
../sdk/tools/azmod-pack build .
../sdk/tools/azmod-pack package .
```

`make test` (the SDK harness) refuses hook mods by design — its stand-in core
only accepts `"firmware": ["any"]` — so testing is on a deck (or the
[xdj-az-emu](https://github.com/Kyle-Hosman/xdj-az-emu) emulator). See
`TESTING.md` for what was verified and what still needs a real Beatport
account.

## License

GPL-3.0-only, like every mod that runs inside the GPL AZ-MODS core.
Not affiliated with AlphaTheta, Pioneer DJ or Beatport; trademarks belong to
their owners.
