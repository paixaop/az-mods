# Beatport Remember

Remembers the Beatport Streaming login entered on the XDJ-AZ, then fills and
submits the login screen the next time it opens. Version 0.3.0 requires
**AZ-MODS 1.2.8 or newer** and firmware supported by its Beatport login service
(currently XDJ-AZ firmware 2.00, tag `ep147-9e9454e357389ece`).

## Use

1. Enable Beatport Remember and log in through the deck's Beatport screen.
   With **Remember a login typed on the deck** enabled, pressing **LOG IN**
   captures a complete username/password pair. This remembers the values you
   submitted; it does not prove that Beatport accepted them.
2. Open **Mod Menu → Community Mods → Installed → Beatport Remember → Settings** to
   check the result. **Updating saved login...** means storage work is still
   pending. **Saved login: ...** appears only for a pair committed to storage.
   A storage failure is shown on this page.
3. With **Auto login** enabled, the next empty Beatport login screen is filled.
   Submission waits 45 seconds by default so the streaming device can start.
   A field you edit is not automatically submitted, and a **LOG OUT** button
   is never pressed automatically.
4. Use **Forget saved login** to remove it. Deletion is asynchronous; check
   that the page reports **No saved login**. If storage fails, the page reports
   the error and leaves Forget available to retry.

The two switches are independent. Turning off Remember stops new capture;
turning off Auto login stops automatic fill and submission. Neither switch
removes the saved login. Advanced settings `delay-sec` (0–600) and `attempts`
(1–4) retain their existing defaults of 45 seconds and one attempt.

## Installation and upgrade

Install the `.azmod` package through AZ-MODS, or copy the package's
`beatport-remember/` directory to `<USB>/MODS/community-mods/` and enable
`community-mods.beatport-remember = on` in `<USB>/MODS/azmods.cfg`.
A changed library needs approval through the deck's normal mod controls.

Upgrade the core along with this mod. An older core cannot supply the service
and the mod refuses initialization with a clear log message. The manifest
uses `"firmware": ["any"]` because the mod uses only the public SDK;
the core still checks whether its native Beatport service supports the
running firmware. A missing or unsupported service does not activate the mod.

After upgrading to 0.3.0, ordinary core updates do not require rebuilding this
mod. The ABI v1 service is append-only: the core keeps its existing entries and
owns changes required by new firmware. A future core may extend its tables;
this mod reads only the supported prefix. Unknown ABI versions, shorter tables,
missing entries and unsupported firmware are refused before login is configured.

## Credential storage

The core stores the login at the existing location:

```
/home/root/settings/azmod-community/beatport-remember.dat
```

Existing valid saved credentials are read from this file, so the upgrade does
not require entering them again. The file uses the existing two hex-encoded
lines (`user=...`, `pass=...`) and is readable only by root (`0600`). Hex is an
encoding, not encryption; anyone with root access to the deck can read it.
Use Forget before handing the deck to another owner. Uninstalling AZ-MODS
does not erase the settings partition.

The mod never receives the password, reads firmware objects, installs hooks,
or performs file I/O. It configures and reads status from the public
`beatport_login_service` SDK interface. The core owns screen lifetime,
credential capture, native UI operations and persistence. Native UI access
runs on EP147's message thread; file operations run on a storage worker.

## Build and test

From this directory, with the SDK in the sibling repository:

```sh
../../az-mods-sdk/tools/azmod-pack build .
../../az-mods-sdk/tools/azmod-pack package .
make test
make test-harness SAN='plain asan'
```

`make test` exercises the shipped callbacks with a mocked public login
service, in plain, AddressSanitizer/UBSan and ThreadSanitizer builds.
`make test-harness` additionally runs the SDK callback and rendering checks.
See [TESTING.md](TESTING.md) for the regression cases and physical-deck checks.

## License

GPL-3.0-only. Not affiliated with AlphaTheta, Pioneer DJ or Beatport;
trademarks belong to their owners.
