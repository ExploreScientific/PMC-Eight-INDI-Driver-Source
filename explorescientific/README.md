# Explore Scientific PMC-Eight INDI Driver Notes

This folder contains Explore Scientific maintenance notes for the PMC-Eight INDI driver fork. These notes are intended to be visible in GitHub for engineering review, release preparation, and upstream pull request support.

## Documents

- [What's New](pmc8-whats-new.md)
- [Firmware Compatibility](pmc8-firmware-compatibility.md)
- [Maintenance Governance](pmc8-maintenance-governance.md)
- [Release Checklist](pmc8-release-checklist.md)
- [Conversion Fixture](pmc8_conversion_fixture.py)

## Version Labeling

Explore Scientific fork releases use this label format:

```text
PMC8-INDI-<upstream-indi-version>-es<N>
```

For example, `PMC8-INDI-2.2.1-es1` means the driver is based on upstream INDI `2.2.1` with Explore Scientific patch release `es1`.

The upstream INDI version in the root `CMakeLists.txt` should not be changed for Explore Scientific fork-only maintenance releases.

## Current Explore Scientific Driver Scope

This maintenance branch is aligning the INDI PMC-Eight telescope driver with the authoritative Explore Scientific ASCOM driver `wifi-fix` branch.

The current ASCOM parity work includes these mount models:

- Losmandy G-11
- Losmandy Titan
- Explore Scientific EXOS II
- Explore Scientific iEXOS-100
- Explore Scientific iEXOS-200
- Explore Scientific iEXOS-300
- MSROEQ
- ASKO SX260S
- Scotty

The conversion fixture can be run from the repository root:

```text
python explorescientific/pmc8_conversion_fixture.py
```
