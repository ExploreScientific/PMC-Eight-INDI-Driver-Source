# PMC-Eight INDI Driver What's New

## PMC8-INDI-2.2.1-es1

Status: Beta release
Base INDI version: 2.2.1  
Date: 2026-05-10
Maintainer: Explore Scientific

Formal beta release:

```text
https://github.com/ExploreScientific/PMC-Eight-INDI-Driver-Source/releases/tag/PMC8-INDI-2.2.1-es1
```

### Executive Summary

This Explore Scientific maintenance update brings the INDI PMC-Eight telescope driver much closer to the current authoritative PMC-Eight ASCOM driver behavior, especially the ASCOM `wifi-fix` branch.

The update focuses on goto accuracy, Rev2 firmware command support, mount-model coverage, WiFi tolerance, configurable Park, fixed Home behavior, and safer manual motion rates. The most visible improvements are ASCOM-style RA slew compensation, a post-goto correction pass, improved Rev2 slew detection using `ESV!`, firmware-timed pulse guiding using `ESSq`, expanded mount selection, a fixed motor-zero Home command, configurable Park storage, and manual motion max rates derived from the selected mount rather than a fixed `833x` value.

SkySafari-specific ASCOM variable-rate suppression was intentionally not ported in this update.

### User-Visible Changes

- Added an `ASCOM Slew Compensation` switch in the INDI Motion Control tab.
- Added INDI Home `Find` and `Go` support. PMC-Eight Home is fixed at motor position `(0,0)`.
- Enabled configurable Park positions independent of Home.
- Changed the fastest manual slew-rate label from `833x` to `Max` because the maximum rate is now calculated from the selected mount.
- Added mount selector entries for Titan, iEXOS-200, iEXOS-300, MSROEQ, and ASKO SX260S.
- Removed the Scotty mount selection from the Explore Scientific INDI driver.
- Improved WiFi/Ethernet connection tolerance for PMC-Eight command responses.
- Improved behavior when Park or Home motion stops before the expected motor target is reached.

### Beta Binary Artifacts

The beta release provides compiled server/driver artifacts for:

- Linux PC/notebook x86_64: `pmc8-indi-beta-PMC8-INDI-2.2.1-es1-linux-x86_64.tar.gz`
- Raspberry Pi 64-bit Linux arm64: `pmc8-indi-beta-PMC8-INDI-2.2.1-es1-linux-arm64.tar.gz`
- macOS Apple Silicon arm64: `pmc8-indi-beta-PMC8-INDI-2.2.1-es1-macos-arm64.zip`
- macOS Intel x86_64: `pmc8-indi-beta-PMC8-INDI-2.2.1-es1-macos-x86_64.zip`

Each artifact includes a matching beta stack built from this source tree:

- `bin/indi_pmc8_telescope`
- `bin/indiserver`
- matching INDI shared libraries
- `README-INSTALL.md`
- `MANIFEST.txt`
- this `pmc8-whats-new.md` release note

These artifacts are not complete KStars distributions. They are intended for beta testers who already have a working INDI/KStars/Ekos environment and are comfortable replacing/restoring INDI server/driver binaries.

### Goto And Slew Accuracy

- Updated RA slew compensation to match the current ASCOM compensation model.
- Updated the short-move RA compensation threshold from the older fixed `2000` count behavior to the ASCOM-derived `5000 - 2 * MountRACounts / 86400` threshold.
- Updated long-move RA compensation defaults to match ASCOM:
  - east/future move offset: `8`
  - west/past move offset: `-4`
- Preserved ramp-only compensation at `4 / 4`, matching the corrected ASCOM source and the ASCOM setup dialog behavior.
- Added southern-hemisphere compensation sign handling based on configured site latitude.
- Added DEC-only slew handling so the driver skips the RA target command when the RA motor is already within one motor count of the requested target.
- Added an ASCOM-style post-goto correction pass.
- The correction pass rereads the actual motor positions, recomputes the target motor counts using the current sidereal time, and sends one additional correction slew if RA or DEC is outside the ASCOM-style correction threshold.
- Set the correction threshold to `10` motor counts.
- Kept the INDI driver in the slewing state until correction evaluation completes, so clients do not see goto completion between the initial slew and the correction slew.
- Updated `set_pmc8_radec()` so DEC motor-count conversion receives the DEC value instead of the RA value.

### Slew Detection

- Added Rev2-compliant `ESV!` state-vector slew detection when ASCOM slew compensation is enabled.
- The `ESV!` path reads RA motor rate, DEC motor rate, and pulse-guide state together instead of relying only on separate RA/DEC rate queries.
- Added fallback to the legacy INDI rate-query slew check if the `ESV!` state-vector read fails.
- Updated DEC slew detection to compare DEC motor rate against the expected custom DEC tracking rate.

### Pulse Guiding

- Added firmware-timed pulse guiding for Rev2-compliant firmware using `ESSqAdDDDD!`.
- Added `ESGq!` confirmation handling for firmware-timed guide pulses.
- Kept the existing INDI timer/rate guide method as a fallback if the firmware-timed command is not accepted.
- Left INDI guide timers in place for client/UI state while allowing firmware to control the actual pulse duration when `ESSq` is used.

### Tracking And Rate Control

- Added custom DEC tracking-rate support for Rev2-compliant firmware.
- DEC custom tracking uses `ESSd1d!` for direction and `ESTe1XXXX!` for high-precision DEC motor rate.
- Updated manual RA/DEC motion direction handling for Titan RA preferred direction and MSROEQ DEC preferred-direction geometry.
- Updated manual motion axis rates to follow the ASCOM `AxisRates` model.
- The INDI `Max` manual move-rate button now derives RA and DEC limits from the selected mount's max motor speed and axis motor-count constants.
- Removed the fixed `833x` max-rate assumption for all mounts.
- Updated manual move-rate motor conversion to use the active RA or DEC motor-count scale. DEC manual motion is no longer converted through the RA axis scale.
- Did not add the ASCOM SkySafari-specific variable-rate suppression behavior.

### Mount Model Support

- Added Explore Scientific PMC-Eight mount constants for:
  - G11
  - Titan
  - EXOS II
  - iEXOS-100
  - iEXOS-200
  - iEXOS-300
  - MSROEQ
  - ASKO SX260S
- Added ASCOM-derived constants for RA motor counts, DEC motor counts, mount max slew speed, long-move offsets, ramp-only offsets, and special MSROEQ geometry behavior.
- Removed Scotty as a selectable mount model.
- Updated firmware P9 mount-code detection to parse the mount code as hexadecimal.
- Mapped firmware P9 codes:
  - `0xD` to Titan
  - `0xE` to MSROEQ
  - `0xF` to ASKO SX260S
- Treated P9 code `0xC` as EXOS2 and did not expose it as a separate selectable Scotty model.

### Coordinate Conversion

- Updated RA motor-count conversion to use ASCOM's hour-angle sign method rather than relying on the caller-supplied pier side.
- Updated southern-hemisphere destination-side handling to follow ASCOM convention while preserving the flipped DEC motor-count geometry used for slew and sync target calculations.
- Updated DEC motor-count conversion for MSROEQ geometry.
- Updated motor-count-to-coordinate conversion for MSROEQ geometry.
- Added a Python conversion fixture at `explorescientific/pmc8_conversion_fixture.py`.
- The fixture validates P9 mapping, destination-side behavior, known motor-count vectors, and coordinate/motor-count round trips.

### Park And Home

- Enabled INDI Park data storage using PMC-Eight motor/encoder counts.
- Default Park remains motor position `(0,0)`.
- Park can now be changed independently of Home for observatory clearance, including roll-off roof constraints.
- Updated `Set Current Park` to save the actual current PMC-Eight motor position, matching ASCOM `SetPark()`.
- Updated Park motion to follow the ASCOM sequence:
  - send the configured RA park target first
  - use the Rev2 stop-at-target axis command where available
  - wait 3 seconds
  - send the configured DEC park target
- Added ASCOM-style Park completion verification.
- Park now checks that RA and DEC motor positions are within `250` counts of the configured park target before reporting the mount parked.
- If parking motion stops before the mount reaches the target, the driver leaves the mount unparked and attempts to restore tracking.
- Added INDI Home `Find` and `Go` support.
- Home always commands motor position `(0,0)` and does not allow redefining the Home position.
- Added Home completion verification using a `100` count tolerance.
- Home stops tracking before commanding the home slew and leaves the mount idle when Home is reached.
- If Home motion stops before the mount reaches `(0,0)`, the driver reports the Home action as failed and attempts to restore tracking.

### WiFi And Ethernet Communication

- Added cleanup for noisy WiFi responses before command matching.
- The cleanup handles leading control characters, `*HELLO*` greetings, `AT` reconnect echoes, and valid PMC-Eight responses that arrive after leading noise.
- Changed Ethernet read timeout handling so the driver retries reads before requesting a disconnect/reconnect cycle.
- Added a 50 ms WiFi refraction interval after successful Ethernet command responses, matching the breathing interval used by ASCOM.
- Increased small response buffers used by command handlers that can receive WiFi-prefixed responses.
- Added last-known-good axis position fallback for Ethernet position reads, so transient WiFi read glitches do not immediately break coordinate or status reporting.
- Limited WiFi response cleanup to Ethernet connections so serial behavior remains unchanged.

### Versioning And Release Notes

- The upstream INDI driver version remains `2.2.1`.
- The Explore Scientific maintenance label for this work is `PMC8-INDI-2.2.1-es1`.
- This file, `explorescientific/pmc8-whats-new.md`, is intended to be the GitHub-facing release note for the Explore Scientific maintenance branch.
- The source comments in `drivers/telescope/pmc8driver.cpp` identify ASCOM parity constants and the DEC-only RA skip behavior.
- The correction slew is implemented inside the normal INDI `ReadScopeStatus()` polling path rather than as a separate background thread.

### Verification Completed

- Focused WSL build passed for the PMC-Eight driver target:
  - `indi_pmc8_telescope`
- GitHub Actions beta artifact workflow passed for:
  - Linux PC/notebook x86_64
  - Raspberry Pi Linux arm64
  - macOS Apple Silicon arm64
  - macOS Intel x86_64
- Python conversion fixture passed:
  - P9 mapping checks
  - destination-side checks
  - known vector checks
  - round-trip checks
- The fixture reports expected ASCOM southern MSROEQ positive-DEC inverse ambiguities separately as expected failures.
- `git diff --check` reported only line-ending conversion warnings from the Windows checkout.
- The updated driver binary was built locally in the existing WSL build tree.

### Remaining Follow-Up Items

- Bench-test Titan, iEXOS-200, iEXOS-300, MSROEQ, and ASKO SX260S selections on physical hardware before upstream release.
- Exercise serial, WiFi, Park, Home, goto correction, and firmware-timed guiding on real PMC-Eight hardware before submitting the INDI upstream pull request.
- Decide whether Explore Scientific wants the ASCOM SkySafari-specific variable-rate suppression behavior ported later. It was intentionally excluded from this update.
