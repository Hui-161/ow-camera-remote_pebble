# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.4] - 2026-10-05

### Fixed

- Fix color previews never completing on Emery (Pebble Time 2) when a frame needs three or more messages. The watch re-requested the continuation it had just received instead of the next one, so the phone and watch kept resending the same message.

## [2.0.3] - 2026-04-26

### Fixed

- Fix action bar width handling to be based on platform, not hardcoded. Fixes previews crashing on the Emery platform.
