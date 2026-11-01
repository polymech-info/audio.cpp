# Tanit Android — Hands-free voice agents and trainable sound triggers

Date: 2026-10-30  
Status: active implementation tracker; Win32 DSP first, Android host second  
Primary target: portable C++ detector/trainer, initially exercised on native Win32,
then hosted by ordinary non-rooted Android phones.

## 1. Product decision

Implement an explicit Hands-free mode. The user enables it while Tanit is visible, grants microphone access, and sees a foreground-service notification with Stop listening.

The service keeps microphone capture open while enabled. A lightweight local detector recognizes a deliberate sound, acknowledges it, and starts the existing voice conversation pipeline. The agent can execute tools locally or through Tanit desktop/cloud and speak its response. After a follow-up window, return to local trigger detection.

Do not require root, Accessibility, assistant-role selection, overlays, or battery-optimization exemption for this feature. Rooted integration is a separate optional future capability.

The basic detector and service are straightforward. Reliable background lifecycle, false-trigger rejection, and measured battery consumption are the release work. Do not promise arbitrary sounds learned perfectly from three recordings.

## 2. User flow

1. Open Tanit → Hands-free → choose a built-in trigger or teach a sound.
2. Explain: microphone stays active; idle audio is processed locally and discarded.
3. Request RECORD_AUDIO; request notification permission where applicable.
4. Start the microphone foreground service while the activity is visible.
5. Show Waiting for trigger, with Stop listening available in app and notification.
6. User locks the phone, makes the trigger, hears a brief acknowledgement, and speaks.
7. Transcribe the request, dispatch it to the selected agent, then speak a concise result.
8. Offer a short follow-up listening window; otherwise return to waiting.

Notification denial must not be represented as microphone denial: foreground services still require a notification, but notification permission is not itself required to start them. Explain reduced notification visibility and retain an in-app Stop action.

No silent boot-start assumption. After force-stop, reboot, permission revocation, or an unrecoverable service loss, expose an honest inactive state and a user-driven restart path. Do not promise uninterrupted operation on every OEM.

## 3. Android declarations and lifecycle

Baseline manifest sketch; adapt names and SDK guards to the existing app:

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android">
    <uses-permission android:name="android.permission.RECORD_AUDIO" />
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_MICROPHONE" />
    <uses-permission android:name="android.permission.POST_NOTIFICATIONS" />
    <uses-permission android:name="android.permission.INTERNET" />

    <application ...>
        <service
            android:name=".voice.HandsFreeService"
            android:exported="false"
            android:foregroundServiceType="microphone" />
    </application>
</manifest>
```

- RECORD_AUDIO is a runtime permission. POST_NOTIFICATIONS is runtime on Android 13+.
- FOREGROUND_SERVICE_MICROPHONE is the type-specific permission introduced for modern foreground-service enforcement; implement against the actual target SDK.
- Start the service through an allowed visible-user flow, after permission succeeds. Promote promptly to foreground with the correct type, then capture.
- Keep the capture/service session alive between conversations. Restarting microphone access later from an arbitrary background event is a different, restricted operation.
- A foreground service improves execution priority; it does not inherently keep the CPU awake or guarantee network availability in Doze.
- Measure screen-off capture on real devices. If required, use a tightly scoped partial wake lock with WAKE_LOCK permission, released on every exit/error path. Account for its energy cost; never add it blindly.
- Detect capture errors, silenced input, privacy-toggle changes, calls, route changes, and permission loss. Show Interrupted or Unavailable instead of pretending to listen.
- For ongoing background TTS/audio playback, evaluate adding mediaPlayback to the service types and FOREGROUND_SERVICE_MEDIA_PLAYBACK. Activate applicable types for actual work and verify current audio-focus rules.
- TTS has no separate dangerous runtime permission. Audio focus, routing, engine availability and offline voice availability still need handling.
- Bluetooth headset support is a separate route/permission test matrix. Do not make it a prerequisite for speaker-and-phone-mic MVP.
- Declare and justify applicable foreground-service uses in Play Console before release.
- A sound trigger is not authentication. Apply existing tool consent policy, and require unlock/confirmation for sensitive actions while locked.

## 4. Components and ownership

| Component | Responsibility |
|---|---|
| HandsFreeService | Android lifecycle, foreground status, permission checks, capture ownership |
| AudioCapture | One AudioRecord owner, route/error events, timestamps and bounded PCM buffers |
| TriggerDetector | DSP or model inference, candidate scores; no Android UI dependencies |
| TriggerPolicy | Pattern matching, hysteresis, debounce, cooldown and playback suppression |
| VoiceSessionController | Acknowledgement, STT, agent dispatch, TTS and follow-up state |
| AgentJobClient | Durable job IDs, reconnect, progress, cancellation and final results |
| TriggerTrainer | Enrollment, feature extraction, validation and profile persistence |
| HandsFreeSettings | Trigger selection, sensitivity, enrollment and status |

Use existing Kotlin service/UI and C++ audio infrastructure where suitable. Keep the DSP core platform-neutral if desktop reuse is valuable. Avoid a plugin framework just for this feature.

One capture stream feeds bounded consumers. Do not open competing microphone recorders for detector, STT and WebRTC. Adapt/resample at explicit boundaries if the conversation pipeline needs another rate. If audio source/effects must change between modes, make the transition explicit and test for lost samples.

## 5. State machine

| State | Active work | Exit |
|---|---|---|
| OFF | No capture, no detector | User enables mode |
| ARMED | Capture + cheap local detection | Confirmed trigger |
| ACKNOWLEDGING | Haptic/chime; suppress retrigger | Cue completes |
| LISTENING | STT/VAD and bounded utterance capture | End of utterance or timeout |
| DISPATCHING | Submit request once, obtain job ID | Accepted or failed |
| SPEAKING | TTS, trigger detection suppressed | Playback ends |
| FOLLOW_UP | Short speech-listening window | New utterance or timeout |
| INTERRUPTED | Capture unavailable; explicit status | Safe recovery or user restart |

Long-running jobs are independent of this state machine. After acknowledgement, return to ARMED while the remote job continues. Completion may queue a spoken announcement according to user settings; do not interrupt an active utterance or announce sensitive content unexpectedly.

MVP is turn-based audio. Suppress triggers during playback and a short measured tail afterward. Full-duplex interruption requires echo cancellation and additional tests; ship later.

Stop listening stops capture, clears buffers, stops voice playback and releases service resources. Cancel job is a separate explicit action. Do not accidentally cancel remote work when the user only disables the microphone.

## 6. Low-consumption detection pipeline

Start with mono PCM, nominal 16 kHz, 16-bit. Validate actual device support and model requirements. Avoid unnecessary upsampling or multiple resamplers.

Suggested initial tunables below are engineering starting points, not validated production thresholds:

| Stage | Initial approach | Cost controls |
|---|---|---|
| Capture | Blocking reads in a dedicated audio worker | No busy polling; reuse buffers |
| Ring buffer | 1–2 seconds of PCM with timestamps | Bounded memory; 2 seconds at 16 kHz PCM16 is about 64 KB |
| Activity gate | RMS/peak energy relative to adaptive noise floor | Linear pass; no model in quiet periods |
| Whistle candidate | Windowed FFT, peak concentration, spectral flatness and pitch continuity | Run only after gate; reuse FFT plans |
| Learned sound candidate | MFCC/log-mel features, template distance or small classifier | Run on candidate windows; bounded inference cadence |
| Decision | Duration, repeated-event pattern, hysteresis and cooldown | Tiny state machine |

A 20–32 ms spectral window and roughly 10–20 ms hop are reasonable DSP starting points. Inference may run at a coarser cadence; tune for the actual shortest sound supported. Avoid a large generic model executing on every audio frame.

The activity gate must retain pre-roll so it does not clip the beginning of a sound. Estimate the noise floor robustly and slowly; freeze or limit adaptation during candidates. Include absolute floors and bounded thresholds so quiet-room hiss or loud-room adaptation cannot make the detector unstable.

Do not use speech-only VAD before whistle detection: it can reject whistles. Use VAD only for the spoken-command stage.

Energy gating reduces processing, not the cost of continuous microphone capture. No duty-cycled capture in MVP: it introduces blind spots. Ordinary apps must not assume access to privileged always-on hotword DSP hardware.

Avoid GPU initialization for tiny classifiers. Compare a simple CPU implementation first. Quantization is useful only after checking detection quality and end-to-end energy.

## 7. Built-in double-whistle detector

Implement this before training infrastructure:

1. Gate on energy above the estimated ambient floor.
2. Find a dominant spectral peak within a configurable broad whistle band; approximately 1–4 kHz is a starting hypothesis, not a universal limit.
3. Require narrow-band concentration, sufficient prominence above surrounding bins, and continuity over several frames.
4. Reject very short clicks, stationary hum and excessively long tones.
5. Confirm a whistle event; wait for a second event within a configured gap.
6. Require comparable pitch or a chosen pitch-change pattern.
7. Emit one trigger, then enter cooldown.

Goertzel is useful for a few fixed frequencies, particularly after pitch calibration. An FFT is a better initial fit for whistles with variable pitch. Include harmonics and octave ambiguity in tests.

Initial tuning ranges: whistle duration around 100–700 ms, inter-event gap around 150–900 ms, cooldown around 2–5 seconds. Tune with real recordings; support users who cannot whistle.

Whistles are the first supported pattern. Finger snaps, tongue clicks and claps need distinct event features and should not inherit whistle thresholds.

## 8. Trainable sounds: bounded implementation

Expose Teach a sound, initially for short repeatable sounds rather than arbitrary environmental audio.

### Enrollment

- Collect approximately 8–12 deliberate examples over multiple takes, including near and normal-use distances.
- Capture a brief sample of ordinary ambient audio.
- Detect clipping, weak signal and examples that differ strongly; ask for another take where needed.
- Hold out some complete takes for validation; never validate solely on the templates used to fit.
- Explain when a sound is indistinguishable from common background noise and suggest a repeated pattern.
- Keep training local by default. Retaining raw recordings requires an explicit user setting; saved features/profiles must also be deletable.

### First learned implementation: feature templates

Extract normalized MFCC or log-mel sequences from segmented sound examples. Normalize carefully: retain an independent signal-quality gate so normalization does not amplify silence into a match.

Use constrained dynamic time warping (DTW) against a small bounded set of templates, or a compact statistical classifier over event-level features. DTW handles timing variation; a narrow alignment band and duration limits bound work.

Compare each positive take with the other accepted takes. That score is leave-one-out calibration. Compare ambient takes with the same set. The threshold is the midpoint of the worst positive and best negative and rejects unknown audio. If those scores overlap, reject the enrollment. The stored margin applies when two armed trigger profiles both score the event. With one armed profile, the threshold alone decides.

Optionally require two matches in sequence. This is especially useful for common transient sounds.

This supports real personalization without backpropagation or a large model on the phone. Training is feature extraction, template selection and threshold fitting. It is appropriate for a small vocabulary of short sounds; do not claim universal audio recognition.

### Upgrade path

If templates fail across realistic conditions, evaluate a small offline-trained depthwise CNN on log-mel features, exported to a supported mobile inference runtime. A frozen audio embedding model with a small personalized head is another option, but benchmark its idle cost before adopting it.

Use separate background/unknown examples and hard negatives. Augment training with noise, gain, room responses and time variation, but keep original recording sessions separated across train/test splits. Quantized accuracy must be tested after conversion.

Generic audio classifiers such as YAMNet-style embeddings can help experimentation; they are candidates, not selected dependencies. Confirm runtime support, licenses, model size and battery cost before integration.

Do not automatically train from every activation. Explicit false-trigger feedback may produce a negative example only with the user's chosen retention policy. Never silently upload ambient recordings.

## 9. Detector contract

Illustrative API, not drop-in project code:

```kotlin
interface TriggerDetector {
    fun reset()
    fun process(
        pcm: ShortArray,
        offset: Int,
        count: Int,
        sampleRate: Int,
        monotonicTimeNs: Long
    ): Detection?
    fun close()
}

data class Detection(
    val triggerId: String,
    val score: Float,
    val startTimeNs: Long,
    val endTimeNs: Long
)
```

Define score direction and calibration per detector; a distance is not a probability. Keep pattern/cooldown policy outside the classifier. Reset on discontinuity, route/rate changes and playback transitions. Do not retain caller-owned PCM buffers without copying into bounded storage.

Profile fields: schema version, detector type, feature configuration, sample rate, thresholds, duration limits, pattern timing, templates/model reference, model hash/version, validation summary and creation time. Reject incompatible profiles; write atomically and permit rollback.

## 10. Agents and spoken responses

Reuse Tanit's existing agent/tool catalog and STT/TTS providers. Trigger detection must never invoke the LLM.

- Trigger → bounded spoken request → transcript → existing agent dispatch.
- Keep idle audio local. Send command audio only under the selected STT/provider policy.
- Use request IDs and idempotent submission so reconnect does not repeat actions.
- Store durable job identity and last-seen event position; reconnect to existing work.
- Speak concise results; render rich output and full details in chat.
- Announce long-task completion only if enabled, respecting audio focus and conversation state.
- Apply normal authorization and confirmation rules to every tool; sounds do not authenticate speakers.
- Support progress and cancellation by explicit job ID/context.
- A microphone foreground service is not a universal long-job exemption. Use the existing scheduler or appropriate Android execution facility for local durable jobs.
- Do not claim immediate remote progress or announcements during every Doze/network condition. Test reconnection and delayed completion delivery.

## 11. Power and reliability acceptance

Measure total power rather than quoting a CPU percentage as battery life. Compare matched runs on at least a recent Pixel-class device, a Samsung device and a lower-end device.

| Scenario | Measure |
|---|---|
| Screen-off baseline, feature off | Device baseline |
| Capture only | Microphone and wakefulness cost |
| Armed, quiet room | Incremental DSP cost |
| Armed, TV/music/traffic | Worst sustained candidate workload |
| Repeated triggers and responses | Real session cost |
| Bluetooth route, if supported | Separate route cost and failures |
| Doze, battery saver, OEM restrictions | Capture continuity, network delay, truthful status |

Record incremental energy/power where possible, battery percentage per hour over long controlled runs, CPU time, inference counts, wake-lock duration, audio overruns, thermal behavior, false triggers per hour, missed-trigger rate and latency.

Proposed release gates to agree after baseline measurements:
- At least 95% trigger detection within a defined supported distance/noise envelope.
- Fewer than one false activation per 24 listening hours on a representative negative corpus as an initial target.
- No unbounded buffering, repeated self-triggering, duplicate agent submissions or resource leaks.
- Stop reliably releases microphone and any wake lock.
- A documented power budget derived from real devices before claiming all-day listening.

These are targets, not measured results. Report sample sizes and per-environment failures. Many hours with no false positives do not prove universal reliability.

## 12. Implementation sequence

1. Prove capture, deterministic replay, FFT/features, matching and enrollment in
   portable C++ on native Win32.
2. Add the Android service and permissions, single capture owner, truthful
   status, Stop and lifecycle handling.
3. Host the proven C++ detector through a thin AAudio/JNI adapter.
4. Connect existing STT → agent → TTS with follow-up timeout and durable remote
   job handling.
5. Validate against unseen takes and long negative audio; tune rejection and power.
6. Add a compact classifier only if measured failure cases justify it.
7. Add wake words, Bluetooth and full-duplex interruption as independently tested extensions.

Keep the MVP small: one service, one audio owner, pluggable detectors, one session controller, existing agents. No continuous Whisper inference, LLM polling, raw-audio logging by default, or mandatory cloud training.

## 12.1 Desktop-first implementation tracker

The desktop slice is an instrumentation and algorithm harness, not a second
product implementation. Capture is a host adapter; segmentation, features,
matching, enrollment, profile validation and decision policy stay portable.

### Confirm before implementation

- [ ] 🔨 **Gate and naming:** add `FEATURE_SOUND_TRIGGER`, default OFF, enabled
  for the development/release-debug presets that already enable audio and voice
  testing. The portable detector does not require STT, `FEATURE_REALTIME`, or an
  LLM. Proposed CLI verb: `sound-trigger`.
- [ ] 🔨 **FFT dependency:** vendor PFFFT behind a tiny internal FFT interface.
  It is BSD-like, allocation-free while transforming, and has x86 and ARM NEON
  paths. Keep a scalar reference DFT in tests for numerical checks. Do not reuse
  the private fixed-512 radix-2 implementation in `gtcrn_live.cpp`.
- [ ] 🔨 **First built-in trigger:** ship a configurable double-whistle profile
  before learned profiles. A single whistle is available as a diagnostic event
  but is not an activation default.
- [ ] 🔨 **Enrollment minimum:** require 8 accepted positive takes plus ambient
  negatives; reserve complete takes for validation. Refuse enrollment when
  positive and negative scores overlap instead of manufacturing a threshold.
- [ ] 🔨 **Raw sample retention:** default to profile/features only. Keep source
  recordings only when the CLI receives an explicit retention option.
- [ ] 🔨 **Profile location and format:** versioned JSON metadata plus bounded
  binary feature data, written atomically under the Tanit profile. Reject
  unknown feature/schema versions and retain the last valid profile.

### Portable C++ layout

Target namespace and directory: `pm::audio::trigger` in `src/audio/trigger/`.

| Unit | Responsibility |
|---|---|
| `fft` | Internal real-FFT interface and selected backend; no detector policy |
| `frame_features` | Windowing, energy, peak/prominence, flatness, log-mel and MFCC |
| `event_segmenter` | Adaptive noise floor, hysteresis, pre-roll and bounded event windows |
| `whistle_detector` | Frequency contour continuity and whistle event score |
| `pattern_policy` | Repetition/gap rules, cooldown, suppression and multi-trigger arbitration |
| `template_matcher` | Constrained DTW and normalized distance; no capture dependency |
| `trigger_trainer` | Quality checks, template selection, leave-one-out calibration and rejection |
| `trigger_profile` | Versioned serialization, compatibility checks and validation summary |
| `trigger_engine` | Shared feature pass, profile registry, typed detections and statistics |

The reusable contract accepts timestamped mono PCM blocks and emits a detection
with stable operation ID, trigger ID, score, score kind, start/end timestamps
and profile version. A score is never labelled a probability unless it has
actually been calibrated as one. Reset on discontinuity or sample-rate change.

For multiple armed triggers, compute framing and spectral features once per
block, evaluate bounded detector states, and require both an absolute threshold
and a winner margin. Pattern/cooldown policy remains separate from feature
matching. The future accelerometer implementation may reuse segmentation, DTW,
threshold fitting and repetition policy, but not audio-specific FFT/mel
features.

### Win32 adapter and CLI

- Add the verb in `src/cli/pm_image_register_cli.cpp` and keep implementation in
  a dedicated `pm_image_cmd_sound_trigger.{hpp,cpp}`.
- Reuse the existing `MixedAudioCapture` adapter for `mic`, `desktop` and `mix`;
  the portable library must not include miniaudio, WASAPI or Win32 headers.
- Coordinate microphone ownership with the existing voice/realtime paths before
  allowing a long-running listener.
- Suggested commands:

```text
tanit-cli sound-trigger devices
tanit-cli sound-trigger capture --source mic --out sample.wav --duration-ms 3000
tanit-cli sound-trigger inspect --input sample.wav --json
tanit-cli sound-trigger match --profile profile.json --input sample.wav --jsonl
tanit-cli sound-trigger listen --profile profile.json --source mic --jsonl
tanit-cli sound-trigger train --id desk-whistle --samples tests/audio/patterns/... --ambient ... --out ...
```

`inspect` is the first debugging surface: frame/event timing, RMS/noise floor,
clipping, dominant frequency, concentration, flatness, continuity and rejection
reason. It must not print raw PCM. Library diagnostics use `logger::trace` for
frame/candidate detail, `debug` for event summaries, `info` for lifecycle and a
warning/error only for actionable degradation or failure.

### Ordered test gates

Implementation status (2026-10-30):

- [x] Portable PFFFT-backed framing/features, adaptive startup calibration,
  bounded event segmentation and discontinuity reset.
- [x] Built-in double-whistle timing, pitch-continuity, cooldown and EOF handling.
  A tone stays open across holes shorter than half the minimum gap, so one
  rising whistle is not split into two events. WAV ingest averages stereo and
  resamples to 16 kHz. The default band gate is concentration 0.28 and
  flatness 0.50, which keeps the middle of a real whistle. A whistle must
  also carry at least twice as much energy in its band as below that band, so
  ordinary speech does not form a pair. Equal-level speech or noise still
  hides the whistle. Music is a different failure: the thin whistle line is
  still there, and one peak hop rejects the whole tone. See
  [audio-patterns.md](audio-patterns.md).
- [x] Shared event segmentation for enrollment and runtime. Internal pauses
  stay in the sequence, trailing quiet is trimmed, and overlong events are
  rejected until the input is quiet again.
- [x] Log-band power features and constrained DTW. Positive calibration is
  leave-one-out; the threshold rejects unknown audio, while the stored margin
  applies only when two template profiles compete.
- [x] Versioned profile metadata plus hashed bounded binary feature sidecar,
  compatibility checks and rollback-preserving replacement.
- [x] Win32 `sound-trigger inspect|match|train|listen` CLI and bounded worker queues.
- [x] Offline-first synthetic orchestrator with optional live microphone capture.
- [x] Release-debug build and `npm run test:sound-trigger`. Synthetic tones
  still separate, and the recorded 48 kHz pattern files do too: double-whistle
  matches once, single-whistle does not.

- [ ] **A — audio plumbing:** feature-disabled help/skip behavior, device
  enumeration, generated WAV decode/resample, deterministic replay, optional
  live mic capture and clean release.
- [ ] **B — DSP primitives:** FFT versus scalar reference, window/bin mapping,
  energy/noise adaptation, ring wrap/discontinuity, synthetic tones, silence,
  clipping and noise.
- [ ] **C — built-in matching:** single-whistle diagnostics, accepted
  double-whistle timing, wrong gap, hum/music/speech rejection, cooldown and
  multiple-profile arbitration.
- [ ] **D — enrollment:** quality rejection, leave-one-out take separation,
  constrained-DTW invariance to bounded timing/gain changes, profile round-trip,
  incompatible/corrupt profile rejection and deterministic threshold fitting.
- [ ] **E — corpus:** user-provided `tests/audio/patterns` positives, ambient
  negatives and hard negatives; report per-profile confusion counts, latency
  and score distributions rather than only pass/fail.
- [ ] **F — live training:** capture accepted/rejected takes from the mic,
  train, arm, match, stop, then verify the device can immediately be acquired
  by the existing voice path.

Create `tests/orchestrator/test-sound-trigger.mjs`, following the voice-command
harness's offline-first and opt-in `--live` behavior, plus a
`test:sound-trigger` package script. Synthetic/basic tests must run before a
sample corpus exists. Building the project remains a separate explicit step.

### Android handoff gate

The detector stays in shared C++. The phone settings sheet can run it on the
microphone service that was started while Tanit was visible. A match starts
the existing live voice session. The microphone notification is the stop
control while the app is in the background. There is no overlay.

### Phone microphone check

Two checks sit on the main Settings page, and again under Speech. They stay
when chat is set to the LAN server, which hides Speech.

Hands-free check opens the microphone for 60 seconds. A rise into activity
plays `sounds/attention.wav` once. It does not run the detector or an agent.

Voice Agent in Settings is a switch. On stays armed until the switch or the
microphone notification turns it off. It runs the
shared detector on that same open capture. A match starts the
existing live voice session and plays `sounds/attention.wav` once, without
taking audio focus. When that session ends it plays `sounds/finish.wav` once,
the same way. While chat is open, the live voice controls are
the display. Leaving the app does not hang that session up. `suspend_voice`
ends the voice session and this trigger listens again. Stop on the
microphone notification ends the voice and the microphone. An overlay is not
used: it needs a separate permission, and this notification is already
allowed to show while the activity is stopped. The trigger does not call
`startForegroundService`. The button does, while Settings is visible.

The trained whistle profile is `whistle.json` plus
`whistle.json.6aca3bab79573947.features.bin` in that folder. Both are
required. Without the feature file the app loads only the built-in
double whistle, which matches two tones and ignores a single whistle. Remove
`HandsFreeProbeService`, both settings rows, and the manifest `microphone`
service entry together.

Both checks ask for the microphone. Notification permission is separate: a
denial is logged and the check still starts. Each promotes a microphone
foreground service, takes the existing dictation audio lease, and opens the
shared AAudio capture with the `recognition` preset. Leaving the chat screen
does not drop that lease. The level check stops after 60 seconds. The
trigger check stops after 3 minutes, or when voice ends. Stop and a lost
lease also stop it. Swiping the task away is logged as `task-removed`.

On the level check, a rise from quiet into activity plays `sounds/attention.wav`
once. The sound does not take audio focus. Playback and the following 2.5
seconds do not start another play. The next one waits until the level is
quiet again, then rises. That check has no speech and no detector. The
settings row shows "Playing a sound" while that file plays. `npm run android:logcat` includes
the `TanitHandsFree` tag; a stream started before that change does not.

Each line is Logcat tag `TanitHandsFree`, which is the `component` on the
paired log path. `check=hf-<millis>` is stable for that run. The lines do not
contain PCM.

```text
npm run logs:query -- "_time:15m source:tanit-android component:TanitHandsFree"
```

| phase | What it means |
|---|---|
| `permission` | Microphone and notification results. Notification denial is not a microphone failure |
| `start` | SDK, both permissions, input types, and the limit for this run |
| `service` | Foreground promotion with type `microphone`, or `foreground=fail` |
| `lease` | Dictation lease grant. `holder_before` is who already had it |
| `capture` | Open result and the following status: rate, device id, running |
| `level` | Once a second. `band` is `quiet` (RMS under 200), `activity`, or `nosamples`. `frames` and `dropped` come from the capture status |
| `profile` | Profiles copied from assets, then which ones loaded. A missing feature file is an error for that file only |
| `detect` | Once a second while armed, then `id` and `score` on a match. No PCM |
| `voice` | Live voice started, or why it did not |
| `cue` | `attention.wav` started or finished after a rise into activity |
| `interrupted` | The lease token changed. `orphan=true` means this check still owns the stream and will close it |
| `stop` | `user`, `limit`, `lease`, `capture`, `profile`, `detect`, `voice`, `voice-ended`, `lease-lost`, `preempted`, `task-removed`, or `error` |

`nosamples` with `running=true` means the stream is open and no frames arrived.
`dropped` climbing means the capture ring overflowed. `preempted` with
`holder=ai` means live voice took the microphone; the check does not close
that stream.

Lock the phone after `phase=capture`. Further `phase=level` lines mean capture
survived the screen turning off. A gap, then one `interrupted` or `stop`,
means the process lost the microphone. There is no wake lock in this check.

## 13. Verification notes and official references

Live documentation retrieval was unavailable while preparing this handover. Android details above are implementation guidance, not a verified matrix for the project's exact target SDK. Before merge, check the current official pages and applicable Play requirements, especially microphone service starts, audio focus, background playback, and restart behavior.

- Foreground service types: https://developer.android.com/develop/background-work/services/fgs/service-types
- Background foreground-service start restrictions: https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start
- Notification permission: https://developer.android.com/develop/ui/views/notifications/notification-permission
- Audio focus: https://developer.android.com/media/optimize/audio-focus
- AudioRecord: https://developer.android.com/reference/android/media/AudioRecord
- Doze and App Standby: https://developer.android.com/training/monitoring-device-state/doze-standby
- MediaPipe Android audio classifier candidate: https://ai.google.dev/edge/mediapipe/solutions/audio/audio_classifier/android
- YAMNet transfer-learning candidate: https://www.tensorflow.org/tutorials/audio/transfer_learning_audio

- PFFFT portable real-time FFT candidate: https://github.com/marton78/pffft
- https://github.com/hayguen/pocketfft/tree/78a0254546e3cd0e042e0216b71f7e1e79f03099
- Mellinger, frequency-contour sound detection:
  http://docs.mbari.org/internal/projects/901502%20Passive%20Acoustic%20Sensing/Papers/MellingerJASA2011.pdf
- Nandwana et al., pitch-pattern features for non-speech sounds:
  https://www.isca-archive.org/interspeech_2015/nandwana15_interspeech.pdf

The desktop-first implementation is present but has not been compiled or
device-tested. The phone has a removable microphone-service check only.
No trigger, power measurement, or model benchmark has been run on a device.

