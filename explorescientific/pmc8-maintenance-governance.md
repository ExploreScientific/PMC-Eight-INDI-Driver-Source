# Explore Scientific PMC-Eight INDI Maintenance Governance

This repository is the Explore Scientific public staging area for PMC-Eight INDI driver maintenance.

The official wide-distribution target remains upstream INDI:

`https://github.com/indilib/indi`

## Supported Driver Files

Explore Scientific-owned PMC-Eight driver review applies to:

- `drivers/telescope/pmc8.cpp`
- `drivers/telescope/pmc8.h`
- `drivers/telescope/pmc8driver.cpp`
- `drivers/telescope/pmc8driver.h`

## Contribution Flow

1. Contributors open an issue or pull request in this repository.
2. GitHub requests review from `@ExploreScientific`.
3. An Explore Scientific engineer reviews firmware compatibility, mount behavior, and INDI driver impact.
4. Required review and branch protection must pass before merge into the Explore Scientific official staging branch.
5. Explore Scientific submits or endorses the accepted change upstream to `indilib/indi`.
6. Once merged upstream, the change becomes part of the official INDI distribution path.

## Official Support Criteria

A change is not considered officially supported by Explore Scientific until:

1. It is reviewed by an Explore Scientific engineer.
2. It is tested against stated PMC-Eight firmware and hardware.
3. It is merged into the Explore Scientific staging branch.
4. It is submitted to or accepted by upstream INDI.
5. Release or compatibility notes identify the supported firmware and driver behavior.

## Required Review Evidence

Every driver change should state:

- PMC-Eight firmware version tested.
- Mount model tested.
- Connection method tested.
- INDI version or commit used.
- Client software used.
- Whether tracking, slew, stop, and readback were tested.

## Upstream Handoff

When a change is ready for upstream:

1. Sync a clean branch against `indilib/indi:master`.
2. Keep the diff limited and reviewable.
3. Format code according to INDI style.
4. Open a pull request to `indilib/indi`.
5. Include the Explore Scientific test matrix and vendor-support note.

## Maintainer Assignment

This repository is currently owned by the `@ExploreScientific` GitHub user account. GitHub user accounts do not support teams, so CODEOWNERS routes PMC-Eight driver review to `@ExploreScientific`.

If the repository is moved under a GitHub organization later, replace `@ExploreScientific` in `.github/CODEOWNERS` with an engineering team, for example:

`@ExploreScientific/pmc-eight-indi-maintainers`
