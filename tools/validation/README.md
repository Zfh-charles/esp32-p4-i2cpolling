# P4 validation gate

This folder is the host-side validation foundation described by
`.cursor/rules/p4-validation-architecture.mdc`. It is deliberately outside
`main/**`: running it does not build, flash, reset, or access the serial port.

Run the default fast gate from the repository root with the same Python that
started the runner:

```powershell
C:\Users\0000\.espressif\python_env\idf5.4_py3.14_env\Scripts\python.exe `
  tools\validation\gate\run_gate.py --scope fast
```

The command executes all selected checks even after a failure, returns `0` only
when every required check passes, and writes a stable JSON report to
`tools/validation/gate/reports/latest-fast.json`.

## Current proof boundary

For Windows build-graph preparation, `tools/build-idf.ps1 -PrepareGraphOnly`
runs only the `build.ninja` target under the normal IDF environment, checks four
configuration/lock/CMake input hashes, and does not request an application build.
It may regenerate CMake if actually needed; this is not a read-only command or
release validation. Input drift is reported and retained, not silently reverted.
Do not combine it with `-DryRun` or `-Reconfigure`. A plain dry-run may list CMake
pessimistically because it cannot execute a `VERIFY_GLOBS` node and restat its
output. `build-graph` scope tests this entry using real Ninja and isolated tiny
graphs, including no-application-build, drift, failure and conflicting flags.

For a non-flashable translation-unit probe through the same environment, use
`tools/build-idf.ps1 -ProbeSource main/application.cc -ProbeOutput build/<new-probe-directory>/application.o`.
Add `-DryRun` first: it validates the compile-database entry without invoking
the compiler or creating output. The actual run reuses the existing probe tool,
refuses existing object/response files, and exits before Ninja/link/image gates.
It proves only `V0-TU`; never use its object to manufacture a flashable mixed BIN.

The `production-control` scope links real ESP-IDF cJSON with two production
fragments extracted from `main/application.cc`: the TTS input guard and the
complete TTS branch. The branch check queues its lambdas, deletes the input JSON
before executing them, and observes visual/state/subtitle/reminder calls using
device stubs. It covers 32 sequential sentences without per-sentence visual
restart, copied subtitle text, another TTS start, auto/manual/late stop, and
reminder disabled/enabled with/without feedback. Four runtime mutations must
fail: lost first-audio reset, per-sentence visual restart, late-stop transition,
and missing prebuffer release. Extraction-boundary drift fails closed.

This is T2 production-fragment orchestration, not the whole Application callback:
outer JSON dispatch, actual Schedule/SetDeviceState, RTOS, audio drain/playback,
reminder ACK internals, channel-close/wakeup and historical reset attribution
are NOT tested. A 32-sentence fixture is not evidence of uninterrupted long TTS
on a device, nor is a second TTS start a second wakeup.
Set `IDF_PATH` to the SDK root and put `gcc`/`g++` on PATH (or set `CC`/`CXX`).
Missing source/compiler is a failure, not SKIP. Both required checks run in
`fast`. Example: `python -B tools/validation/gate/run_gate.py --scope production-control`.

For the production-source face checks without running the entire fast suite:

```powershell
python -B tools/validation/gate/run_gate.py --scope production-face --report tools/validation/gate/reports/production-face.json
```

This selects the historical-log observer tests, real `visual_port.cc` forwarding
tests (both board configurations), and real `face_route_v2.cc` admission
characterization. Device effects, AFE and RTOS dependencies remain stubbed.
VisualPort also runs in `fast`. FaceRoute characterization deliberately does not:
its PASS reproduces the current singleton/ownership behavior and the known gap
that three sequential transactions are admitted. It does **not** prove a per-TTS
budget, prevent real repeated full-screen work, or fix the historical WDT.
The observer check runs its tests, not an assessment of the current device log.
Neither scope proves full Application event ordering, a complete live conversation,
or current firmware identity. No newly selected check is optional when missing.

- `contracts/` validates small, sanitized, versioned Event/Effect/Trace
  fixtures and their M+/M- oracles.
- `replay/` provides a deterministic FakeClock/FakeExecutor system model and
  six fault-injected session scenarios. Its golden effects are an executable
  product contract, not a copy of the ESP runtime.
- `gate/` runs the Trace contract, T2 scenarios, the existing FaceStateReducer
  and emotion policy host tests, the real architecture ratchet, and the
  code-health checker's own tests.
- A fast PASS proves the modeled T0/T1/T2 contracts and host logic only. It does not prove ESP
  ABI, FreeRTOS scheduling, PSRAM/DMA behavior, audio/display hardware, cold
  boot, or product feel.
- P4 image-layout checking remains a release/build gate because it requires a
  concrete firmware BIN; it is intentionally not part of the default fast
  scope.

## Adding a check

Add a manifest entry with a stable `id`, `tier`, `scope`, timeout, and command.
Checks must be deterministic, non-interactive, and must not hide a required
failure as `SKIP`. Put hardware-dependent validation in T4/T5 instead of
teaching the host gate to claim evidence it cannot produce.
