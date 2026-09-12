# Source archive and repository boundaries

This repository archives the ESP32-P4 voice terminal, its PC asset tools and
the accompanying SD pack. It is not a complete copy of the developer workspace.

## Included

- `main/`, `scripts/`, `partitions/`: firmware implementation and build inputs.
- `components/`: local component overrides, including the modified LVGL port;
  retain each component's license. Registry downloads belong to `managed_components/`.
- `components/eezui/src/ui/`: the UI implementation selected by its CMake file.
  Nested historical `eezui/eezui/` copies are not build inputs.
- `tools/emotion_tool_dev/`: the canonical PC compiler and regression tests.
- `tools/product_contracts/`: the compiler's shared contract validator/examples.
- `tools/mjpeg_ai_dialogue_v5p3_mouth_focus/`: archived six-expression SD resources.
  The firmware also has a separately compiled idle band in its board asset directory.

## Local only

The independent `game` project is retained outside this archive, not deleted.
Rules, AGENT/AGENTS files, editor workspaces, serial/build logs, device backups,
BIN/ELF firmware images, caches, private configurations and credentials are not
publication inputs. Removing duplicate paths here does not delete the original
workspace copies; previous public versions remain in Git history.

## Status and verification boundary

The current firmware source marker is `boot_trace_v10_s1gy_idle_output_probe`.
It is a diagnostic snapshot: standby rendering is observed in software, but
visible motion and the historical reset root cause are not certified fixed.
This archive does not flash or replace the device's recovery slot.

The reminder example uses placeholder endpoints and an empty authentication
salt. Provision matching server credentials locally; never flash this template
as a ready-to-use deployment configuration. A previous public revision contained
an authentication salt. Removing it here does not erase Git history; coordinate
server/device credential rotation separately.

Public-layout checks passed: seven compiler tests, four legacy-entry contract
tests, and the pack validator. The pack is accepted as legacy v2, with warnings
for missing explicit runtime metadata and eight unapproved life tracks; this is
not approval to enable those tracks. The staging path guard found no forbidden
files. Existing vendor/document whitespace findings are retained without mass
formatting. The generated language header is recreated by the CMake language
generation rule and is intentionally not tracked.

The existing architecture ratchet is not green: the display adapter has 4529
lines against a 4438 baseline; the three hotspots total 9951 against 9935.
These existing findings are retained, not hidden by raising the baseline.
This archival cleanup does not certify completion of the hot-path refactoring.

Use ESP-IDF 5.4.1 and the documented board configuration. Local `sdkconfig` and
device-specific settings are deliberately not exported; a fresh build is not
automatically identical to a previously tested binary. Rebuilding and deployment
still require image/layout checks and relevant hardware validation.

Before committing, run `python tools/code_health/public_archive_guard.py` after
staging. This checks tracked path boundaries, not comprehensive secret scanning,
build completeness or runtime correctness.
