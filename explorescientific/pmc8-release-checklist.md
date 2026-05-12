# PMC-Eight INDI Driver Release Checklist

Use this checklist before Explore Scientific submits a PMC-Eight INDI driver update upstream.

## Source Control

- [ ] Branch is based on current `indilib/indi:master`.
- [ ] Diff is limited to the PMC-Eight driver or justified in the PR.
- [ ] No unrelated formatting churn.
- [ ] No generated build artifacts committed.
- [ ] Commit messages are clear and upstream-friendly.

## Build

- [ ] Clean CMake configure succeeds.
- [ ] Clean build succeeds.
- [ ] `indi_pmc8_telescope` is built.
- [ ] INDI-style Debian package build succeeds for each supported Linux target.
- [ ] Linux package artifacts include `libindi-data`, `libindi1`, and `indi-bin` `.deb` files.
- [ ] Package metadata declares correct runtime shared-library dependencies through Debian packaging.
- [ ] `ldd $(which indi_pmc8_telescope)` on a test install resolves all libraries.
- [ ] Formatting follows INDI `astyle` rules.

## Hardware Test

| Mount | Firmware | Connection | Client | Pass/Fail | Notes |
|---|---|---|---|---|---|
|  |  |  |  |  |  |

## Functional Test

- [ ] Connect.
- [ ] Disconnect.
- [ ] Track on.
- [ ] Track off.
- [ ] RA slew.
- [ ] DEC slew.
- [ ] Stop/abort.
- [ ] Position readback.
- [ ] Rate readback.
- [ ] Firmware/version identification.
- [ ] Error recovery behavior if applicable.

## Documentation

- [ ] Firmware compatibility noted.
- [ ] User-visible behavior changes documented.
- [ ] Known limitations documented.
- [ ] Linux beta installation instructions use `.deb` packages rather than raw executable copy instructions.
- [ ] Restore/reinstall instructions are included for returning to distribution INDI packages.
- [ ] Upstream PR body includes test evidence.

## Approval

- [ ] Explore Scientific engineering review complete.
- [ ] Upstream PR prepared.
- [ ] Support team notified of expected behavior and tested firmware.
