# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.4] - 2026-10-05

### Added

- Show on the waiting screen where the connection breaks: an AppMessage error on the watch, or "Phone reached, no image" when the phone acknowledges requests but sends no frames. The capture notice names the error when the watch couldn't send at all.
- Show a notice and vibrate when the phone doesn't acknowledge a capture. The companion app ignores captures while it isn't open in the foreground, so a press used to look successful while no photo was taken.

### Changed

- Move the sources to the standard SDK layout (`src/c/`, tinflate under `src/c/lib/`) and use the SDK's default `wscript`. Builders that assemble the project themselves, like CloudPebble-style services, only pick up `src/c/` and their own `wscript`, so `lib/` was missing there and the build failed.

### Fixed

- Fix color previews never completing on Emery (Pebble Time 2) when a frame needs three or more messages. The watch re-requested the continuation it had just received instead of the next one, so the phone and watch kept resending the same message.

## [2.0.3] - 2026-04-26

### Fixed

- Fix action bar width handling to be based on platform, not hardcoded. Fixes previews crashing on the Emery platform.
