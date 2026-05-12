Subject: PMC-Eight INDI Driver Beta Binary Builds Available for Linux PC, Raspberry Pi, and macOS

Hello PMC-Eight users,

I want to announce the availability of updated **Explore Scientific PMC-Eight INDI server/driver beta builds** for testing on Linux PC/notebook systems, Raspberry Pi 64-bit Linux systems, and macOS systems running KStars/Ekos/INDI.

My longer-term plan is to provide an **Explore Scientific maintained PMC-Eight INDI driver source branch and beta build process** so the INDI driver can stay in closer parity with the latest Explore Scientific ASCOM driver. The ASCOM driver has historically been the authoritative implementation for PMC-Eight mount behavior, and this INDI update begins bringing the INDI driver into alignment with that code path.

This beta branch and these beta artifacts are the first step in that process. The plan is to make the updated Explore Scientific INDI driver available to beta testers, collect feedback from PMC-Eight users, correct issues found during testing, and then submit the validated changes as a pull request to the official INDI project repository.

Once the changes are submitted to the INDI project, Explore Scientific will work through the INDI maintainer review process so the update can be accepted into the official INDI code base. After that, the updated PMC-Eight driver can flow into normal INDI releases and downstream distributions.

That upstream step is important. Products and platforms that use INDI, including ASIAIR and other astronomy software or appliance distributions, generally incorporate INDI driver updates from the official INDI project repository or from official INDI release packages. Explore Scientific cannot directly control when each downstream product vendor incorporates an INDI update, but getting the PMC-Eight driver changes accepted into the official INDI repository is the correct path for making the update broadly available to general product customers.

This beta release is therefore intended to validate the updated driver before it is submitted upstream. It is still beta software. Please test only if you are comfortable installing beta INDI packages, restoring your previous INDI packages if needed, and collecting logs.

Repository:

```text
https://github.com/ExploreScientific/PMC-Eight-INDI-Driver-Source
```

Branch:

```text
explorescientific/pmc8-maintenance
```

Formal beta release:

```text
https://github.com/ExploreScientific/PMC-Eight-INDI-Driver-Source/releases/tag/PMC8-INDI-2.2.1-es1
```

Download the beta artifacts from the **Assets** section of that GitHub Release.

## Available Beta Artifacts

The beta driver executable is:

```text
indi_pmc8_telescope
```

This executable is used by `indiserver` when KStars/Ekos starts the Explore Scientific PMC-Eight telescope driver.

Linux beta artifacts are now provided as INDI-style Debian packages instead of loose executable tarballs. This follows the same packaging model used by the INDI project and avoids shared-library version failures between Ubuntu releases. The package version includes the Ubuntu codename, such as `2.2.1+es1~jammy` or `2.2.1+es1~noble`, so the package files for Ubuntu 22.04 and Ubuntu 24.04 remain distinct.

Each Linux package set includes:

- `libindi-data_<version>~<distro>_all.deb`
- `libindi1_<version>~<distro>_<arch>.deb`
- `indi-bin_<version>~<distro>_<arch>.deb`
- checksums
- installation notes and release notes

The beta artifacts are:

```text
Linux PC / notebook Ubuntu 22.04 x86_64:
ubuntu-22.04-x86_64-deb package set

Linux PC / notebook Ubuntu 24.04 x86_64:
ubuntu-24.04-x86_64-deb package set

Raspberry Pi 64-bit Ubuntu 22.04 arm64:
ubuntu-22.04-arm64-deb package set

Raspberry Pi 64-bit Ubuntu 24.04 arm64:
ubuntu-24.04-arm64-deb package set

macOS Apple Silicon:
pmc8-indi-beta-PMC8-INDI-2.2.1-es1-macos-arm64.zip

macOS Intel:
pmc8-indi-beta-PMC8-INDI-2.2.1-es1-macos-x86_64.zip
```

The macOS build is split into Apple Silicon and Intel builds because macOS INDI/Homebrew dependencies are architecture-specific.

## What Has Changed

This beta update includes a substantial PMC-Eight driver maintenance update.

Major changes include:

- ASCOM-style PMC-Eight slew compensation logic.
- Improved goto accuracy behavior.
- A post-goto correction pass after the initial slew completes.
- Improved Rev2 firmware support using the `ESV!` state-vector command.
- Firmware-timed pulse guiding using the PMC-Eight `ESSq` command where supported.
- Custom DEC tracking-rate support using high-precision Rev2 firmware commands.
- Improved WiFi/Ethernet response handling.
- Better tolerance of WiFi stream artifacts such as greetings, reconnect echoes, and noisy leading characters.
- Configurable INDI Park support using PMC-Eight motor/encoder counts.
- INDI Home support, with Home defined as fixed motor position `(0,0)`.
- Expanded mount model support.
- Removal of obsolete/non-needed Scotty mount selection.
- Updated manual axis-rate handling based on the selected mount's actual motor-count constants.
- Updated RA/DEC motor-count conversion to better match the ASCOM implementation.
- Added Explore Scientific maintenance documentation and a conversion test fixture.

## Important Beta Notes

These artifacts are not complete KStars distributions. The Linux package sets provide beta INDI runtime packages containing the updated PMC-Eight telescope driver.

You must already have a working KStars/Ekos/INDI installation before installing one of these beta artifacts.

If you are using your PMC-Eight system for an important observing session, outreach event, or imaging run, please use the current released INDI driver unless you are specifically prepared to test and troubleshoot.

## Linux PC / Notebook Install Instructions

Use the package set that matches your Ubuntu release and processor architecture.

Examples:

```text
Ubuntu 22.04 x86_64:
ubuntu-22.04-x86_64-deb

Ubuntu 24.04 x86_64:
ubuntu-24.04-x86_64-deb
```

This file is available from the GitHub Release assets:

```text
https://github.com/ExploreScientific/PMC-Eight-INDI-Driver-Source/releases/tag/PMC8-INDI-2.2.1-es1
```

Install:

```bash
sudo apt update
sudo apt install ./libindi-data_*.deb ./libindi1_*.deb ./indi-bin_*.deb
```

Verify:

```bash
which indi_pmc8_telescope
which indiserver
ldd $(which indi_pmc8_telescope)
```

Then restart KStars/Ekos and test your PMC-Eight profile.

## Raspberry Pi 64-bit Linux Install Instructions

Use this artifact on a Raspberry Pi running a 64-bit Linux operating system.

Use the package set that matches your Raspberry Pi operating system release:

```text
Ubuntu 22.04 arm64:
ubuntu-22.04-arm64-deb

Ubuntu 24.04 arm64:
ubuntu-24.04-arm64-deb
```

This file is available from the GitHub Release assets:

```text
https://github.com/ExploreScientific/PMC-Eight-INDI-Driver-Source/releases/tag/PMC8-INDI-2.2.1-es1
```

Install:

```bash
sudo apt update
sudo apt install ./libindi-data_*.deb ./libindi1_*.deb ./indi-bin_*.deb
```

Verify:

```bash
which indi_pmc8_telescope
which indiserver
ldd $(which indi_pmc8_telescope)
```

Then restart KStars/Ekos or restart the INDI server software you use on the Raspberry Pi.

Raspberry Pi note: these beta package sets are intended for 64-bit Raspberry Pi systems. They are not intended for older 32-bit Raspberry Pi OS installations.

If you see a shared-library error such as `libcfitsio.so.9: cannot open shared object file`, you probably installed a package built for a different Ubuntu release. Use the package set that matches the OS release reported by:

```bash
lsb_release -a
uname -m
```

## macOS Install Instructions

macOS beta testing is possible, but it should be considered **advanced testing at this stage**.

The main caveat is that KStars/Ekos on macOS may use bundled INDI components depending on how KStars was installed. A driver installed into `/usr/local/bin` or `/opt/homebrew/bin` may not automatically be used by a bundled KStars application unless that KStars installation is configured to use the same `indiserver` and driver path.

Mac users who want to test should be comfortable with Homebrew, command-line tools, and confirming which `indiserver` their KStars/Ekos installation is using.

Download the artifact that matches your Mac:

```text
Apple Silicon:
pmc8-indi-beta-PMC8-INDI-2.2.1-es1-macos-arm64.zip

Intel:
pmc8-indi-beta-PMC8-INDI-2.2.1-es1-macos-x86_64.zip
```

Both macOS files are available from the GitHub Release assets:

```text
https://github.com/ExploreScientific/PMC-Eight-INDI-Driver-Source/releases/tag/PMC8-INDI-2.2.1-es1
```

Install:

```bash
unzip pmc8-indi-beta-PMC8-INDI-2.2.1-es1-macos-arm64.zip
cd pmc8-indi-beta-PMC8-INDI-2.2.1-es1-macos-arm64
sudo ./install.sh "$(brew --prefix)"
```

For Intel Macs, use the `macos-x86_64` artifact and directory name instead.

Verify:

```bash
which indi_pmc8_telescope
which indiserver
```

On Apple Silicon Macs, the driver will usually be installed under:

```text
/opt/homebrew/bin
```

On Intel Macs, the driver will usually be installed under:

```text
/usr/local/bin
```

Before testing with KStars/Ekos on macOS, please confirm that KStars is using the same INDI installation where the beta `indi_pmc8_telescope` driver was installed.

## Testing Requested

Please test only if you are comfortable restoring your previous INDI driver if needed.

I am especially interested in feedback on:

- USB serial connection.
- WiFi connection.
- Goto accuracy.
- Park behavior.
- Home behavior.
- Manual motion controls.
- Pulse guiding behavior.
- Mount model selection.
- Behavior with different PMC-Eight firmware versions.
- Raspberry Pi 64-bit compatibility.
- macOS KStars/Ekos driver discovery and launch behavior.

Please include the following information when reporting results:

```text
Mount model:
PMC-Eight firmware version:
Connection type: USB serial / WiFi / Bluetooth serial:
Operating system and version:
Hardware platform: Linux PC / Raspberry Pi / Mac Apple Silicon / Mac Intel:
KStars version:
INDI version:
How KStars/Ekos was installed:
Artifact downloaded:
Install result:
What worked:
What failed:
Steps to reproduce:
INDI driver log:
Ekos log:
```

## Restoring The Previous Driver

The installer backs up an existing driver in the same install directory with a timestamped `.bak` suffix before installing the beta driver.

For example:

```text
indi_pmc8_telescope.bak-YYYYMMDD-HHMMSS
```

To restore manually, stop KStars/Ekos/INDI, copy the backup file back to `indi_pmc8_telescope`, and restart KStars/Ekos.

## Reporting Issues

For beta feedback, please include as much detail as possible. Logs are extremely helpful, especially INDI driver logs and Ekos logs.

The goal is to validate this Explore Scientific maintained branch, correct any issues found by beta testers, and then submit the driver changes upstream to the INDI project so the PMC-Eight driver can eventually be distributed through the normal INDI/KStars release process.

Once accepted into the official INDI project, the updated driver will be available for downstream distributors and product vendors to incorporate according to their own update schedules.

Thank you to anyone willing to help test and improve PMC-Eight support for the Linux, Raspberry Pi, and macOS astronomy community.

Clear skies,

Jerry Hubbell  
Explore Scientific
