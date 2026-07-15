# Changelog

User-facing release notes for OpenXcom Coop Mod. The release pipeline reads the
`## [<version>]` section matching a pushed `v<version>` tag and uses it as the
GitHub release body, so add a section here (and merge it to `main`) before you
tag a release. Nightlies use auto-generated notes and do not read this file.

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Newest first.

## [Unreleased]
- Notes for the next release accumulate here; rename to `## [x.y.z]` when tagging.

### Fixed
- Co-op transfers: fixed a use-after-free when transferring a craft with crew to
  a co-op base — kept crew are now unassigned before the craft is freed, so their
  craft pointer is never left dangling (could crash or corrupt saves).
- Co-op transfers: the receiver now validates that the target base exists before
  acknowledging a transfer/purchase, so the sender no longer removes goods (or
  deducts funds) for a transfer that can never be applied (silent item loss).
- Co-op transfers: restored the peer notification dropped in the purchase-sync
  rework — an immediate soldier/equipment transfer to a co-op base again arrives
  at the receiving base (fixes gear not moving with a transferred soldier).

<!--
## [8.4.3] - 2026-07-15
### Added
- ...
### Fixed
- ...
### Changed
- ...
-->
