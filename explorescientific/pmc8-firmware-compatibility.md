# PMC-Eight INDI Firmware Compatibility Notes

This file tracks the PMC-Eight firmware versions used to validate Explore Scientific-supported INDI driver behavior.

| Firmware Version | Mounts Tested | Connection Modes Tested | INDI Driver Commit | Status | Notes |
|---|---|---|---|---|---|
| 20A02.1.8.3.bt | TBD | TBD | TBD | Pending | Current recommended PMC-Eight firmware at time of repository setup. |

## Compatibility Review Triggers

Review the INDI driver whenever PMC-Eight firmware changes:

- Command names or accepted parameters.
- Response formats.
- Timing or timeout behavior.
- WiFi, BLE, BT, TCP, or UDP connection handling.
- State vector contents.
- Slew, tracking, pulse-guide, or stop behavior.
- Mount identity or version reporting.
