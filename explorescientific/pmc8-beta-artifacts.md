# PMC-Eight INDI Beta Artifacts

This repository can produce beta binary artifacts for the Explore Scientific PMC-Eight INDI server/driver.

The artifacts are release downloads. They are not committed to the repository.

Each artifact contains:

- `bin/indi_pmc8_telescope`
- `bin/indiserver`
- matching INDI shared libraries built from this source tree
- release notes
- an installer script

## Artifact Targets

- Linux PC/notebook x86_64: `pmc8-indi-beta-<version>-linux-x86_64.tar.gz`
- Raspberry Pi 64-bit Linux arm64: `pmc8-indi-beta-<version>-linux-arm64.tar.gz`
- macOS Apple Silicon arm64: `pmc8-indi-beta-<version>-macos-arm64.zip`
- macOS Intel x86_64: `pmc8-indi-beta-<version>-macos-x86_64.zip`

The macOS target is split into Apple Silicon and Intel artifacts because INDI and Homebrew dependencies are architecture-specific.

## Create Artifacts With GitHub Actions

Run the workflow:

```text
Actions -> PMC-Eight Beta Artifacts -> Run workflow
```

Inputs:

```text
release_label: PMC8-INDI-2.2.1-es1
upload_to_release: true or false
```

When `upload_to_release` is `false`, artifacts are attached only to the workflow run.

When `upload_to_release` is `true`, artifacts are uploaded to the GitHub release whose tag matches `release_label`.

## Create A Local Linux Artifact

From a Linux shell with INDI build dependencies installed:

```bash
cd PMC-Eight-INDI-Driver-Source
bash explorescientific/package_pmc8_beta.sh
```

The output appears in:

```text
artifacts/
```

## Install From An Artifact

Extract the artifact, then run:

```bash
sudo ./install.sh
```

By default this installs:

```text
/usr/local/bin/indi_pmc8_telescope
```

The installer backs up existing files in the same install directories before replacing them.
