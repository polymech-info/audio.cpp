# AEC3 implementation and test plan

## Goal

Validate WebRTC AEC3 with a real microphone and speaker before changing the
realtime voice agent. Use the same order on both platforms:

1. Build an isolated acoustic test harness.
2. Measure AEC3 with repeatable signals and real hardware.
3. Fix framing, timing, and delay reporting in the harness.
4. Move the proven pipeline into the realtime voice agent.
5. Repeat the harness-first process on Android.

The first target is classic AEC3. Neural residual echo estimation is a separate
experiment and is not part of the initial rollout.

## Existing path

Desktop already has the main pieces:

- `packages/audio.cpp` exposes `EchoCancellationProcessor`.
- `src/audio/audio_apm_host.cpp` and the WebRTC plugin adapt that interface to
  `webrtc::AudioProcessing`.
- `src/core/audio_realtime.cpp` feeds rendered model PCM to
  `ProcessReverseStream` and microphone PCM to `ProcessStream`.
- Capture and AEC run at 16 kHz mono. Realtime network PCM runs at 24 kHz.
- The mic path currently supplies a 200 ms delay hint. Desktop loopback leaves
  delay estimation internal to AEC3.

Android currently uses platform `AcousticEchoCanceler` where available and a
soft APM fallback. The soft APM is render-aware ducking, not adaptive acoustic
echo cancellation. The vendored WebRTC APM build is not yet an Android build.

## Phase 1: desktop acoustic lab

### Harness

Keep the lab inside `packages/audio.cpp`. The package already owns miniaudio
capture, playback, and device enumeration, and `pm-audio` is the CLI. Add an
`aec3` command branch beside `sound-trigger`, not a second executable under
`packages/webrtc`.

Split it the same way as the sound-trigger branch:

- `cli/aec3.cpp` and `cli/aec3.hpp` register `pm-audio aec3 ...` and print the
  existing JSON success and failure contract.
- A small lab module under `src/` runs scenarios, framing, recording, and
  metrics. The CLI does not own that logic.
- Devices, capture, playback, and `EchoCancellationProcessor` stay the library
  APIs the realtime host already uses.

`packages/webrtc` remains the APM implementation. The parent desktop build links
`webrtc_apm` and installs it through `src/audio/audio_apm_host.cpp`. The
standalone `pm-audio` lab needs its own thin installer because that host file is
not part of the package. It links the sibling vendored `packages/webrtc` target
when the lab option is enabled, then installs the same `ApmBackend` callbacks.
A build without that backend still runs `aec3`, reports the backend inactive,
and does not pretend to cancel echo. Do not fetch WebRTC from the network.

Do not route the first experiments through the realtime API. Network behavior,
provider VAD, and model output would make AEC failures harder to isolate.

The harness must:

- list and select input and output devices;
- open 16 kHz mono processing streams;
- play a known render signal through the selected physical speaker;
- feed the exact PCM sent to playback into `ProcessReverseStream`;
- record the physical microphone and feed it into `ProcessStream`;
- buffer arbitrary device callback sizes into complete 10 ms frames;
- preserve callback timestamps and frame counts;
- optionally apply a fixed stream-delay hint or use internal estimation;
- run with AEC on and off without changing gain or device settings;
- save all evidence from one run in a timestamped result directory.

Suggested interface:

```text
pm-audio aec3 devices
pm-audio aec3 calibrate
pm-audio aec3 run --scenario far-only
pm-audio aec3 run --source mix
pm-audio aec3 run --source desktop
pm-audio aec3 replay --capture mic.wav --render far.wav --out session
pm-audio aec3 replay --source mix --capture mic.wav --desktop-capture loop.wav --render far.wav
```

Omitted `--input`, `--output`, and `--desktop` use the system default microphone,
the system default speaker, and loopback of that speaker. A live `run` or
`calibrate` without `--render` plays a one-second log sweep, repeated for the
capture length. `calibrate` records for three seconds. Pass the flags only to
select a different device or a WAV.

`mic` records the microphone. `desktop` records loopback of the playback
endpoint. `mix` records both. Each leg has its own echo canceller, and the
results are summed afterwards. A fixed delay hint applies to the microphone
leg only. The desktop leg keeps internal delay estimation, matching the
realtime voice agent: loopback is a same-clock digital copy, while the
microphone includes the speaker-to-mic path. Android uses the same source
values. On the phone, `desktop` is PCM from the desktop rather than a local
loopback device. The mix itself stays in this library.

Each run should save:

```text
manifest.json
render-reference.wav
capture-raw.wav
capture-processed.wav
near-end-reference.wav       # when supplied by the scenario
events.jsonl
metrics.json
```

`manifest.json` records device names, actual device formats, APM configuration,
volume instructions, delay mode, application revision, and scenario. Do not
record or alter the system speaker or microphone volume silently.

### Signals and physical setup

Use a fixed physical arrangement for comparable runs:

- laptop or external speaker in a marked position;
- microphone in a marked position and orientation;
- measured speaker and microphone volume settings;
- quiet-room noise-floor sample before each batch;
- no headset, Bluetooth processing, conferencing software, or OS voice effects
  unless that route is the subject of the test.

Initial render material:

- logarithmic sweep or chirp for latency and path inspection;
- pink noise for convergence;
- model-like speech clips for the real failure mode;
- alternating speech and silence for reconvergence;
- a clipped/loud sample to exercise saturation handling.

Double-talk tests need a separate near-end source. Prefer a second speaker near
the microphone playing a known speech track. A person speaking is useful for
listening tests but is not repeatable enough for the baseline.

### Measurements

Poll `AudioProcessing::GetStatistics()` no more than once per second and record
available values:

- echo return loss (ERL);
- echo return loss enhancement (ERLE);
- divergent-filter fraction;
- estimated delay and delay stability;
- residual echo likelihood and recent maximum.

Also compute outside APM:

- raw and processed RMS and peak;
- clipping count;
- render/capture cross-correlation and estimated lag;
- residual correlation with the render reference;
- near-end attenuation during double-talk;
- callback-size distribution;
- incomplete-frame buffering, underruns, overruns, and dropped samples;
- processing time per 10 ms frame and worst-case processing time.

The harness should emit a concise terminal summary and retain full JSON data.
Listening to the three WAVs remains required because a strong numeric score can
still hide musical noise or damage near-end speech.

### Desktop scenario matrix

Run at least:

1. Silence/no render: AEC must not invent audible artifacts.
2. Far-end only: rendered speech must be strongly reduced.
3. Near-end only: local speech must remain natural.
4. Double-talk: local speech must remain intelligible while echo is reduced.
5. Render starts and stops repeatedly: AEC must converge and recover.
6. Speaker volume changes during a run: AEC must reconverge.
7. Capture or playback interruption: no stale-reference burst after recovery.
8. Delay sweep: auto, 0, 40, 80, 120, 160, 200, 250, and 300 ms.
9. Long run: at least 30 minutes, checking drift and processing deadlines.
10. At least one built-in and one USB microphone/speaker combination.

Keep separate baselines for microphone acoustic echo and WASAPI loopback.
Loopback is a digital same-clock signal and should not be used to claim acoustic
microphone performance.

### Initial acceptance gate

Set numerical thresholds from the first controlled baseline rather than
inventing universal values before measuring the hardware. Promotion requires:

- no APM processing errors or sample loss;
- every APM call receives exactly one 10 ms frame;
- processing stays safely below the 10 ms real-time budget;
- stable delay estimates after convergence;
- a material, repeatable reduction in far-end speech versus AEC off;
- no material loss of near-end intelligibility during double-talk;
- no severe pumping, clipping, or persistent musical noise;
- results repeat across at least three runs of each core scenario.

Commit the small source WAVs, scenario manifests, and expected metric ranges.
Do not commit large run directories or recordings containing private speech.

### Recorded baseline, 2026-11-21

Route: `Headphones (2- Bose NC 700 HP)` hung on `Microphone (Yeti Stereo Microphone)`.
Classic AEC3, mono 16 kHz, high-pass on, noise suppression and gain control off,
delay left to the estimator. Session volume was full scale. Both devices opened
at 16 kHz, 2 channels. These runs are the reference for this route. They do not
clear the promotion gate: enhancement never becomes a strong, stable reduction.

| 30 s take | Level | Correlation | Delay | Echo return loss | Enhancement |
|---|---|---:|---:|---|---|
| Microphone, far-only | RMS 209 → 34, peak 2137 → 1345 | 0.45 | near 280 ms, wanders 264–284 | climbs to about −21 dB and stays | returns to 0.18 dB |
| Headphone loopback | RMS 8105 → 1512, peak 11472 → 17228 | 0.98 | locked at 20 ms | settled at 3.7 dB by 4 s | peaks at 6.7 dB near 6 s, then slips to about 4.4 dB |

The desktop voice path already links this same library when
`FEATURE_WEBRT_ECHO_CANCELLATION` is on. Its microphone leg still forces a
200 ms hint. That hint is not this baseline. The loopback leg already leaves
delay to the estimator, which matches the 20 ms lock above.

## Phase 2: desktop production migration

Move only behavior that passed the acoustic gate into the shared desktop
adapter and realtime voice path.

### Required implementation work

1. Add persistent 10 ms framing buffers to both render and capture adapters.
   No callback tail may bypass AEC or be discarded.
2. Keep render and capture sample accounting continuous across arbitrary
   miniaudio callback sizes.
3. Expose APM statistics through a diagnostic snapshot without logging every
   audio callback.
4. Replace the assumed 200 ms mic delay with the value justified by harness
   measurements, or use internal estimation when it is more stable.
5. Keep desktop-loopback delay policy separate from acoustic microphone policy.
6. Reset or recreate APM cleanly after device reroute, playback discontinuity,
   sample-rate change, or a changed render path.
7. Preserve processing order:
   render PCM -> 16 kHz render reference -> AEC3;
   mic PCM -> AEC3 -> optional remaining processing -> 24 kHz network resample.
8. Avoid simultaneous independent echo suppressors unless the combination has
   an explicit test result.

### Voice-agent validation

After offline and live-lab acceptance, run the real desktop voice agent with
the same physical setup:

- model speech must not create user turns;
- barge-in must work while the model speaks;
- near-end transcript quality must not regress;
- clearing playback must not feed synthetic silence as a new render reference;
- reconnect, underrun, device reroute, and rapid stop/start must not leave stale
  AEC state;
- desktop-only, mic-only, and mixed input modes must remain distinct.

Capture provider/VAD events alongside local APM metrics. The production gate is
behavioral: no self-triggered turns in the controlled far-end-only scenario,
while double-talk still opens a user turn reliably.

Ship behind the existing WebRTC echo-cancellation feature control first. Keep a
runtime diagnostic switch for AEC bypass so failures can be compared without
changing the rest of the audio path.

## Phase 3: Android acoustic lab

Reuse the same lab module. Android does not grow a second scenario runner or a
separate CLI. A debug entry in the app calls the library API behind
`pm-audio aec3`, with an Android playback and capture backend in place of
miniaudio. Artifact names, scenarios, and metric fields stay identical.

The Android harness must compare:

1. platform `AcousticEchoCanceler`;
2. no echo cancellation;
3. the current soft APM fallback;
4. WebRTC AEC3, only after an Android build exists.

For every run, record:

- device model, Android version, route, and audio mode;
- capture session id;
- whether platform AEC is available, created, enabled, and remains enabled;
- input/output device and actual sample rate;
- raw and processed capture where the API permits it;
- render PCM, timing events, VAD events, and processing cost;
- speakerphone, earpiece, wired, USB, and Bluetooth route behavior separately.

Use built-in speaker and microphone first. OEM platform AEC is commonly tied to
the communication route and exact capture session; merely reporting
`isAvailable()` is not evidence that the active stream is processed.

### Android WebRTC feasibility gate

Port classic AEC3 only when the device matrix shows a material platform-AEC
gap. Before doing so, prove an NDK target with:

- Android task-queue implementation and CPU feature selection;
- required Abseil targets;
- no Windows-only source files or compile definitions;
- acceptable APK size, startup time, CPU, and battery use;
- the same complete 10 ms framing and render-reference timing as desktop.

When platform AEC is active, disable soft render ducking. Do not stack platform
AEC and WebRTC AEC3 by default; compare them independently first. A route may
select one backend based on measured capability and test results.

## Phase 4: Android production migration

Promote the winning Android backend into realtime voice only after the Android
lab passes:

- platform AEC remains the preferred path when it is verified effective;
- WebRTC AEC3 is a measured fallback, not an automatic second layer;
- soft APM is retained only as an explicit half-duplex fallback;
- route changes recreate and revalidate the selected backend;
- processed 16 kHz capture is resampled to 24 kHz only after echo cancellation;
- full-duplex barge-in and self-trigger tests match the desktop behavioral gate.

Test on multiple OEMs. One successful emulator, Pixel, or Samsung run is not a
portable Android result.

## Neural residual echo estimation

The updated WebRTC tree exposes
`BuiltinAudioProcessingBuilder::SetNeuralResidualEchoEstimator`, but the local
build does not compile or inject the TFLite implementation. Treat this as a
later experiment.

Consider it only when classic AEC3 passes timing and framing tests but leaves a
repeatable residual that still triggers VAD or reaches transcription. Evaluate
it as another harness backend and measure quality, CPU, model size, startup,
and double-talk damage. Do not make TensorFlow Lite a production dependency
without that evidence.

## Recommended delivery slices

1. `pm-audio aec3` command branch, library lab module, and deterministic scenarios.
2. Desktop baseline report and agreed acceptance ranges.
3. Shared 10 ms framing plus APM diagnostic snapshot.
4. Desktop realtime integration and behavioral tests.
5. Android platform-AEC harness and device matrix.
6. Android realtime integration using the measured winner.
7. Optional Android classic-AEC3 fallback.
8. Optional neural residual estimator experiment.

Each slice should leave the previous audio path selectable until its replacement
passes the corresponding acoustic and voice-agent gates.

## Concrete implementation plan

### Package layout

Add these files to `packages/audio.cpp`:

```text
include/pm/audio/aec3_lab.hpp
src/aec3/aec3_lab.cpp
src/aec3/aec3_framer.cpp
src/aec3/aec3_metrics.cpp
src/aec3/aec3_artifacts.cpp
cli/aec3.hpp
cli/aec3.cpp
cli/aec3_webrtc_backend.cpp
tests/test-aec3-cli.mjs
tests/fixtures/aec3/
```

Responsibilities:

- `aec3_lab.hpp` is the platform-neutral scenario API. It defines run options,
  backend status, progress events, metrics, and artifact paths.
- `aec3_lab.cpp` owns scenario lifecycle and coordinates playback, capture,
  framing, APM, and artifact writers.
- `aec3_framer.cpp` converts arbitrary callback lengths into lossless 10 ms
  render and capture frames. It owns no device.
- `aec3_metrics.cpp` computes external measurements and aggregates APM
  snapshots.
- `aec3_artifacts.cpp` writes the manifest, JSONL events, JSON metrics, and WAV
  streams. File I/O runs off the audio callback.
- `cli/aec3.cpp` registers and dispatches the command branch.
- `cli/aec3_webrtc_backend.cpp` is standalone-CLI glue that constructs WebRTC
  APM and installs `ApmBackend`. It is not a second scenario implementation.
- JavaScript tests verify command parsing, JSON schemas, replay behavior, and
  failure modes without requiring acoustic hardware.

Do not put scenario code in `packages/webrtc`, `src/core/audio_realtime.cpp`, or
the CLI parser. The first two are implementation and product integration; the
CLI is only an adapter.

### Public lab API

Keep the initial API small:

```cpp
namespace pm::audio::aec3 {

enum class DelayMode { automatic, fixed };
enum class Scenario { silence, far_only, near_only, double_talk, transitions };

struct RunOptions {
    std::string input_device;
    std::string output_device;
    std::filesystem::path output_directory;
    std::filesystem::path render_source;
    std::filesystem::path near_source;
    Scenario scenario = Scenario::far_only;
    DelayMode delay_mode = DelayMode::automatic;
    int delay_ms = 0;
    int duration_seconds = 20;
    bool aec_enabled = true;
    bool ns_enabled = false;
    bool agc_enabled = false;
};

struct RunResult {
    bool ok = false;
    std::string error;
    std::filesystem::path session_directory;
    nlohmann::json summary;
};

RunResult run(const RunOptions& options);
RunResult replay(const std::filesystem::path& session,
                 DelayMode delay_mode,
                 int delay_ms);

} // namespace pm::audio::aec3
```

The final names may change during implementation, but the boundary should
remain: CLI-compatible value types in, structured result out. Audio callbacks
must not emit JSON, write files, allocate unbounded buffers, or print logs.

### APM backend extension

The current `ApmBackend` exposes create, destroy, delay, render, capture, and
active operations. The lab also needs diagnostics without depending directly
on WebRTC types.

Extend `include/pm/audio/echo.hpp` with:

- a portable `ApmStatistics` containing optional ERL, ERLE, divergent-filter
  fraction, delay median/deviation/current value, and residual-echo values;
- `bool (*statistics)(void*, ApmStatistics*)`;
- optionally `void (*reset)(void*)` if tests show that discontinuities need an
  explicit reset rather than object recreation.

Expose `EchoCancellationProcessor::statistics()`. Implement it in:

- `src/audio/audio_apm_host.cpp`;
- `src/plugins/webrtc/tanit_webrtc_plugin.cpp` and the plugin ABI;
- `cli/aec3_webrtc_backend.cpp`;
- Android soft APM as unsupported/empty.

If changing the plugin ABI is undesirable in the first slice, statistics can be
absent for the plugin while the in-process lab implements them. Do not expose
WebRTC headers from `pm-audio-lib`.

### CMake wiring

Update `packages/audio.cpp/CMakeLists.txt`:

1. Add the platform-neutral lab sources to `PM_AUDIO_SOURCES`.
2. Add `cli/aec3.cpp` to the standalone `pm-audio` executable.
3. Add `PM_AUDIO_AEC3_LAB` and `PM_AUDIO_WEBRTC_ROOT`.
4. Default the root to the sibling `../webrtc` only when that directory exists.
5. When the lab option is enabled and `webrtc_apm` does not exist, call
   `add_subdirectory()` on the explicit vendored root.
6. Add `cli/aec3_webrtc_backend.cpp`, link `webrtc_apm`, and define
   `PM_AUDIO_AEC3_BACKEND_WEBRTC=1`.
7. When no backend is available, still compile the command branch and report a
   structured `ENOTSUP` result for processing commands.
8. Add the non-hardware CLI tests to the existing CTest/Node runner.

The lab option must never download WebRTC. A standalone checkout of only
`audio.cpp` remains buildable and retains the rest of the CLI.

### CLI registration

Follow `cli/sound_trigger.hpp/.cpp`:

```cpp
struct Aec3Cli {
    CLI::App* root = nullptr;
    int run() const;
};

Aec3Cli add_aec3(CLI::App& app);
```

In `cli/main.cpp`:

1. Include `aec3.hpp`.
2. Register `const Aec3Cli aec3 = add_aec3(app);`.
3. Dispatch it after parsing.
4. Keep success as one JSON object on stdout.
5. Keep diagnostics and interactive progress on stderr.
6. Preserve exit 0 for success, 1 for runtime failure, and 2 for usage errors.

Initial subcommands:

- `devices`: selected-device capabilities and AEC backend status;
- `calibrate`: noise floor, clipping check, render/capture lag estimate;
- `run`: live acoustic scenario;
- `replay`: deterministic offline processing of a saved session;
- `inspect`: summarize an existing session without processing audio.

Every JSON response needs a schema, such as `pm.audio.aec3.run/v1`. Avoid output
that only a human-readable parser can consume.

### Audio threading and framing

Use the existing device wrappers where they provide the needed callback. Extend
`include/pm/audio/devices.hpp` and `src/devices.cpp` only for missing operations,
such as deterministic PCM playback or callback timing. Do not bypass them with
a second miniaudio wrapper in the harness.

The runtime has four logical paths:

1. Playback producer reads the render fixture into a bounded ring.
2. Playback callback writes PCM to the device and copies exactly that PCM into
   the render-reference ring.
3. Capture callback copies raw microphone PCM into a capture ring.
4. A worker drains both rings, creates complete 10 ms frames, invokes APM, and
   queues artifact and metric records.

The callback never waits on APM or disk. Rings use explicit overflow counters;
overflow invalidates the run rather than silently producing a misleading
quality score.

Render-reference timestamps describe when frames enter the playback device.
Capture timestamps describe when microphone frames leave the capture device.
Store both the callback clock and cumulative sample index. This is required to
separate device latency from AEC processing time.

### Artifact implementation

Reuse existing WAV helpers where suitable. Do not make the audio callback call
`WavWriter` directly.

Write artifacts to a temporary session directory first. On clean completion,
flush writers and atomically finalize the manifest. An interrupted run remains
marked incomplete and `replay` rejects it unless explicitly overridden.

Use versioned schemas:

- `pm.audio.aec3.manifest/v1`;
- `pm.audio.aec3.event/v1`;
- `pm.audio.aec3.metrics/v1`;
- `pm.audio.aec3.run/v1`.

The manifest includes the hashes of source fixtures so two runs can be compared
only when their inputs match.

### Desktop implementation sequence

Implement in this order:

1. Command branch with backend/device status and JSON contract.
2. Offline `replay` using two WAV files and complete 10 ms framing.
3. Artifact writers and deterministic metric tests.
4. Playback-only fixture runner.
5. Simultaneous physical playback and raw capture.
6. Live AEC worker and AEC on/off paired runs.
7. Calibration and delay sweep.
8. APM statistics snapshots.
9. Long-run and interruption scenarios.
10. Controlled hardware baseline and acceptance ranges.
11. Transfer the proven framer, delay policy, and diagnostics into desktop
    realtime voice.

Offline replay comes before live capture because it makes framing and output
fully reproducible. Live acoustic results must not be used to debug basic WAV,
queue, or CLI errors.

### Desktop production files

After the harness passes, expected production changes are limited to:

- `packages/audio.cpp/include/pm/audio/echo.hpp`;
- `packages/audio.cpp/src/echo.cpp`;
- `packages/audio.cpp/include/pm/audio/devices.hpp`;
- `packages/audio.cpp/src/devices.cpp`;
- `src/audio/audio_apm_host.cpp`;
- `src/plugins/plugin_webrtc_api.h`;
- `src/plugins/webrtc/host_webrtc.cpp`;
- `src/plugins/webrtc/tanit_webrtc_plugin.cpp`;
- `src/core/audio_realtime.cpp`.

`packages/llm.cpp/src/kbot/llm_client_realtime.cpp` should not change for AEC.
It receives already-processed 24 kHz PCM and remains a transport/client layer.

### Android implementation files

After desktop production validation, reuse the library runner through:

- `android/tanit/app/src/main/cpp/CMakeLists.txt`;
- a small JNI bridge beside `jni_pm_voice.cpp`;
- `src/audio/audio_apm_android.cpp`;
- `src/android/realtime_audio.hpp` only if shared framing/resampling needs an
  Android adapter;
- `android/tanit/app/src/main/kotlin/com/polymech/pmimage/audio/PlatformAudioFx.kt`;
- a debug-only Android AEC lab screen/service;
- the realtime capture coordinator after the Android lab passes.

The debug UI selects route, backend, scenario, and output directory, then calls
the same C++ scenario API. Android-specific code provides device/session
control and platform-AEC observations; it does not recalculate desktop metrics
in Kotlin.

### Automated test layers

1. Framer unit tests: split, coalesced, zero, odd, and discontinuous callback
   sizes; no lost or duplicated samples.
2. Metrics unit tests: known signals and lags with deterministic expected
   values.
3. Artifact tests: schema, atomic completion, interrupted run, and fixture hash.
4. CLI tests: parser, JSON-only stdout, exit codes, missing backend, and invalid
   device.
5. Offline APM tests: AEC on/off, fixed/auto delay, repeatability, and no process
   errors.
6. Desktop hardware tests: the scenario matrix and acceptance gate above.
7. Desktop voice-agent tests: self-trigger and barge-in behavior.
8. Android device tests: platform AEC first, then optional WebRTC fallback.

Hardware tests are manual or explicitly selected and must not become ordinary
CI tests. Offline replay and CLI tests are the regression layer suitable for
CI.
