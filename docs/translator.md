# Tanit Android — Real-Time Translator

**Engineering handover / implementation plan**  
**Date:** 2026-11-16  
**Status:** Living design and roadmap. The one-device path below is what the
CLI and the phone run today. Bidirectional routing, soft-paired multi-device
media, and the scenarios in sections 18–19 remain future work.

### Implemented now

Two device backends, one session:

| | Desktop | Phone |
|---|---|---|
| Capture / playback | miniaudio mic and speaker FIFO (`packages/audio.cpp/src/translate.cpp`) | AAudio capture, SPSC ring, AudioTrack pull (`src/android/pm_android_translate.cpp`) |
| Provider | `TranslationSession` → `/v1/realtime/translations` | same |
| Playback gate | `TranslatePlaybackGate` | same |
| Digital-silence drop | `model_pcm_level` in `translate_policy.hpp` (peak ≤ 32 and RMS ≤ 4) | same |

Conversational playback holds translated PCM until local VAD `speech_end`, then
plays it after `--playback-buffer-ms` (0–5000, default 3000). That lead is not
re-armed while audio was rendered in the last 500 ms. Half-duplex sends silence
upstream while translated audio is queued or was just rendered. A user turn
that already started keeps its mic frames until `speech_end`. The hold buffer
keeps the newest 60 s. There is no packet-duration play lock.

A language change is `session.update` on the open socket. The first
`session.updated` and each later switch log `warmup_ms`. The phone title shows
`Switching to …` until that event, then `Live · <language> · ready`. The
recording stays one source WAV.

Transcript deltas are concatenated with a space when two word fragments would
otherwise glue. Whitespace-only deltas are kept. `*.transcript.done` replaces
the in-progress utterance, which is where punctuation arrives.

Phone stop sets `Stopping…` immediately and finishes the socket on a worker,
with a 2 s cap. It does not block the UI on the 30 s drain.

CLI examples, including `--switch-lang` and `--playback-buffer-ms`, are in
`releases/web-docs/cli/audio_translate_examples.md`.

## 1. Objective

Build a compact two-way, face-to-face translator for Android (initial example: German ↔ Spanish), reusing the Tanit audio engine, voice agent, recorder, OpenAI and ElevenLabs integrations, and local wake/whistle controls.

Provide three behaviors:

1. **Simultaneous:** stream speech and play translated audio as it arrives.
2. **Conversational (default):** stream source audio continuously but hold *translated output PCM* until local VAD detects a pause; then play the queued audio and continue draining live deltas.
3. **Utterance / accurate:** recognize a whole utterance, translate text, then synthesize TTS. Slower, but provides a cleaner sentence boundary when simultaneous translation misinterprets partial phrases.

The first release targets a **single shared phone microphone** with two people taking turns. It does **not** promise reliable overlapping-speaker separation, telephone-call capture, or perfect speaker diarization.

## 2. Existing Tanit components to reuse

| Existing capability | Proposed role | Work needed |
|---|---|---|
| Native capture / `audio.cpp` / Android JNI | Single shared PCM capture and fan-out | Audit thread ownership and resampling |
| WebRTC APM: AEC / NS / gain processing | Clean input, especially while TTS plays | Connect actual rendered playback reference; verify Android audio effects |
| VAD / voice pipeline | Speech onset, silence, utterance candidates | Add a small, independent translator state machine |
| Whisper.cpp on Android | Optional offline STT; possibly language hints | Keep heavy model initialization off the startup path |
| OpenAI Realtime integration | Dedicated `gpt-realtime-translate` backend | Add translation-session adapter; not standard voice-agent lifecycle |
| ElevenLabs | Scribe Live STT/diarization and existing TTS | Optional alternative route, not a hard dependency |
| Existing LLM provider stack | Text-to-text translation, context-aware fallback | Narrow translation-only prompt and latency budget |
| Recorder, timestamps, SRT | History, playback, export, post-processing | Add utterance metadata and optional original-audio retention |
| Local wake voice commands and whistle detector | Hands-free pause/repeat/direction control | Isolate commands from translated conversation |
| Compose / Material 3 | Small translator screen | Thin presentation, DSP stays native |

**Avoid:** a second capture engine, Kotlin DSP, a new agent framework, an always-running local Whisper model, or mandatory speaker-embedding inference.

## 3. Provider behavior — confirmed vs inferred

### OpenAI dedicated Realtime Translation

- Model: `gpt-realtime-translate`; endpoint: `/v1/realtime/translations`.
- Unlike the general voice-agent Realtime API, translation processes incoming source audio continuously. No documented `turn_detection`, `silence_duration_ms`, or `response.create` control exists for translation sessions.
- WebSocket input: **24 kHz, mono, PCM16 little-endian**, base64 encoded in `session.input_audio_buffer.append`.
- Recommended input cadence: **200 ms** frames. Shorter appends are buffered by the service; larger appends are split into frames and queued back-to-back.
- **Keep sending silence** during gaps. Pausing the send loop does not preserve real-world silence in model time.
- Receive `session.output_audio.delta` and `session.output_transcript.delta`; optional `session.input_transcript.delta` with source transcription enabled.
- A session has **one current output language**. `session.update` may change `audio.output.language`, but this is not automatic bidirectional routing.
- Official conversational guidance recommends **one session per direction**, ideally with separate participant tracks. Our single microphone needs *local routing*.
- `session.close` ends a session and flushes it; it is **not** an utterance-commit event. Continue receiving until `session.closed`.

**Important:** There is no documented end-of-utterance marker that guarantees that all PCM for one local VAD segment has arrived. Output audio and locally detected speech segments must be aligned heuristically and handled conservatively.

### ElevenLabs

- Existing ElevenLabs TTS remains the custom-voice output backend for text-first translation.
- `scribe_v3_live` supports real-time diarization (`diarize=true`) and turn prediction (`commit_strategy=turn_prediction`).
- Speaker IDs are provisional: unresolved labels are possible, and `speakers_merged` events can rewrite earlier assignments. Never use those IDs as immutable identities.
- Scribe produces transcription, not the finished translated speech; it needs our translation LLM and TTS.
- Keep this as **optional comparison/backend**, especially because earlier desktop diarization attempts were not reliable.

## 4. Processing architecture

```text
Android mic (native PCM, ideally 48 kHz)
   |
   +-- raw-PCM tap --> existing whistle / wake-pattern detector
   |
   +-- WebRTC APM (capture ProcessStream; AEC reference from actual TTS playback)
           |
           +-- 16 kHz frames --> WebRTC VAD --> turn state / speaker DSP
           |
           +-- resample 24 kHz PCM16 --> direction router --> OpenAI session DE->ES
           |                                           \--> OpenAI session ES->DE
           |
           +-- optional recording PCM / timestamps

Each provider session --> output PCM + transcript deltas
                            |
                      output arbiter / queue
                            |
                 instantaneous or VAD-gated playback
                            |
                  Android audio output + AEC reference

Utterance metadata --> recorder / transcript / later reclassification
Kotlin Compose <--> small JNI control/event layer
```

**Audio threads:** audio callbacks must never perform network I/O, JSON decoding, model inference, file writes, or unbounded allocation. Use bounded, timestamped SPSC/MPSC queues as appropriate. Distinguish capture, DSP, WebSocket send/receive, playback and persistence workers.

**Rate separation:** process WebRTC APM in its expected frame format (often 10 ms); feed standalone WebRTC VAD 10/20/30 ms supported PCM frames; packetize **24 kHz/200 ms** only at the OpenAI transport boundary. No need to make all subsystems run at a 200 ms cadence.

## 5. VAD and conversational playback

### First choice: WebRTC VAD

Use the standalone native WebRTC VAD, if already compiled into our APM build; do not assume APM's own voice-probability API exposes exactly the same interface. Start with 16 kHz mono PCM16, 20 ms frames, aggressiveness mode 1, and explicit hangover logic.

**Initial tuning values (not measured defaults):**

| Setting | Starting point |
|---|---|
| Frame | 20 ms |
| Speech onset | 60–100 ms consistent activity |
| Pre-roll for clipped beginnings | 150–250 ms |
| Minimum plausible utterance | 200 ms |
| End-of-speech hold | 500 ms, adjustable 200–1200 ms |
| Extended pause / uncertain speech | 700–1000 ms |
| Long utterance | Consider clause splits; do not assume 10 s equals a sentence |

Maintain states such as `IDLE -> MAYBE_SPEECH -> SPEAKING -> MAYBE_END -> ENDED` with hysteresis and a monotonic timestamp. VAD means *acoustic speech activity*, not grammatical sentence completion.

**Spectral DSP:** RMS, band energies, spectral flatness/flux, periodicity and noise-floor estimates are useful diagnostic features, not a substitute for VAD. Frequency alone cannot distinguish speech from music or whistles; the local whistle detector's roughly 2.4–3.8 kHz band overlaps speech information.

### Two ways to delay speech output

**A. Gate *translated audio output* (preferred):** stream source PCM continuously, receive translated PCM continuously, buffer output while the person is speaking, then open playback on a local VAD end. Continue playing fresh output PCM as it arrives. This is low effort, but does **not** make the translation model wait to decide what words to say; any premature phrasing is already encoded in the buffered audio.

**B. Gate *source input*:** collect the source utterance locally and send it afterward. Not the initial design: burst-feeding the service compresses the apparent timing, while stopping appends during gaps removes silence. If tested, pace audio at real-time rate and include real silence, accepting additional latency. Prefer the text-first backend for truly utterance-committed behavior.

Playback policy shipped on both backends:

- Start the conversational burst after VAD `speech_end`, after the jitter lead. Simultaneous mode uses about 200 ms.
- Do not estimate playout from packet size. Mute the uplink from the host queue and from a 500 ms hangover after the last rendered frame.
- Keep feeding a turn that started before playout, until `speech_end`. Ignore new onsets while the speaker path is active.
- Drop provider digital silence before it enters the queue.
- The hold cap is 60 s and keeps the newest tail. Overflow is logged. Do not drop audio because a local timer expired.
- `session.output_transcript.done` / `session.input_transcript.done` are the punctuated utterance. Deltas before that are provisional and may be revised.

Still design, not shipped:

- Queue translated PCM with session ID, received timestamp and a best-effort utterance id.
- On overload, notify and degrade to captions. Do not pretend the subtitles are live when the queue is behind.

## 6. Two-way language direction

A translation session sends audio to **one configured target language**. Example:

- **A:** German source -> Spanish output.
- **B:** Spanish source -> German output.

**Preferred architecture:** two persistent session adapters (one per direction), but do **not** broadcast full microphone speech into both, or duplicate translations will result. Route a detected utterance to one session, with time-preserving silence for the inactive session if it stays connected. Validate the inactive-session strategy, cost and backend timing experimentally.

Routing evidence, in order:

1. Explicit user-selected direction (guaranteed and immediate).
2. Reliable source-language detection from available STT/recognition signals.
3. Optional two-speaker acoustic profile and recent direction/turn history.
4. If uncertain: retain last safe direction or ask for a tap/whistle override; do **not** make a confident arbitrary guess.

**Bootstrap problem:** language may only be confidently known after 300–1000+ ms of speech. Keep a source PCM pre-roll ring and replay its beginning to the chosen direction using a properly paced startup feed. This is especially relevant for short turns. The model should not see the same speech twice. Clearly separate this source buffering from *translated output buffering*.

**Alternative:** one session with `session.update` changing the output language only at a clean idle boundary after draining prior audio. This saves a connection but has more ambiguity across direction changes. Keep as an optimization, not the first POC.

## 7. Echo cancellation and playback

AEC is optional for push-to-talk, subtitle-only, or headphones; **strongly recommended for open-speaker, full-duplex translation with barge-in**.

- Feed the **actual rendered translated PCM** (the audio entering the player, including any resampling/volume effects where feasible) into the WebRTC APM reverse/reference stream, synchronized to output timing.
- Feed mic audio to APM's capture stream. Track capture/render latency and configure required delay/timing for the WebRTC version in use.
- Do **not** send network-arrival audio to the AEC reference before it is rendered: our conversational mode may hold it for seconds.
- Audit Android platform AEC/NS/AGC effects and avoid accidental double processing. Benchmark actual capture routes rather than assuming the effect is present or absent.
- AEC reduces our own TTS feedback; it **cannot** separate two real people speaking simultaneously.
- Preserve a separate raw-PCM whistle branch; gate false triggers from TTS and test whistles while audio is playing.
- Provide a conservative half-duplex fallback if AEC fails or the device route is unsuitable.

## 8. Practical speaker tracking / diarization

Do **not** make a heavyweight neural diarizer a release blocker. Earlier desktop experiments were inconsistent; Android does not improve the underlying single-mic problem.

Implement optional **classic-DSP speaker-change estimation**, with a separate offline refinement pass:

- For VAD-clean windows, extract MFCC means/variances (e.g. 13–20 coefficients), F0 median/spread when voiced, spectral tilt, harmonicity and optional formant statistics.
- Detect sustained acoustic changes near candidate utterance boundaries; maintain tentative profiles `A`, `B`, and `UNKNOWN`.
- Adapt each profile only using reasonably long, clean, high-confidence utterances. Normalize feature scales and don't treat volume as identity.
- Never force short `yes/no/OK` replies, overlap, laughter, music or distant voices into an ID.
- If the user enrolls two voices with 3–5 seconds of clean speech, test whether supervised two-profile matching improves live routing.
- For saved sessions, re-extract features across the full recording, perform global two-cluster comparison and relabel earlier turns when better context is available. Preserve user overrides and provenance/confidence.

**Limits:** identical pitch/timbre, changing language, emotion, distance and overlapping speech will defeat basic DSP. Diarization is a *hint* for routing, not the primary language detector.

Optional experiment: compare this baseline with ElevenLabs Scribe v3 Live speaker events or an existing native speaker-embedding model on **the same recorded test set**. Accept a neural path only if it delivers demonstrably better accuracy/latency/CPU.

## 9. Wake voice commands / whistle interaction

Keep command recognition out of the translation transcript and provider audio where possible. The lightweight whistle detector runs from local raw PCM independently of Whisper/model startup.

| Gesture | Action |
|---|---|
| Double whistle | Toggle/pin translation direction |
| Wake + “pause translation” | Pause/resume |
| Wake + “repeat” | Replay last completed translated segment |
| Wake + “slower” | Adjust playback speed where the player supports it |
| Screen tap | Choose language direction / override speaker |

Use an explicit command-active state with a short suppression window, deduplication and safe defaults. Detect commands during playback, but guard against speakers reproducing or triggering the same signal. Do not forward commands into the translator as conversational content or permit accidental agent tool execution.

## 10. Offline mode and failure handling

The existing Android Whisper.cpp integration has a slow cold start. Therefore:

- Start cloud translation without waiting for Whisper initialization.
- Only preload Whisper on explicit offline request or when a defined idle/charging/memory policy makes it sensible.
- Load asynchronously, cancel safely, expose `loading/ready/unavailable`, and retain warm contexts only while memory permits.
- Whisper by itself provides STT; offline two-way text translation still needs a translation engine/model, and offline spoken output needs Android/offline TTS.
- Do not claim instantaneous offline failover unless the complete STT→translation→TTS chain has been loaded and measured.

On a network interruption: keep capture/recorder alive if permission and policy permit, clearly mark untranslated intervals, reconnect with session generation IDs, and never replay old translated audio silently. For reconnections, invalidate late events from an old session.

## 11. Android platform / security constraints

- Implement the user-visible mic session as an Android microphone foreground service when background operation is needed; obtain `RECORD_AUDIO` and applicable foreground-service permissions. Start under a permitted foreground/visible condition on Android 14+.
- Handle audio focus loss, calls, capture preemption, Bluetooth route changes, output-device latency and hardware effects.
- Ordinary third-party apps cannot assume access to cellular/WhatsApp call audio. That use case is out of scope.
- Do not embed permanent OpenAI/ElevenLabs API keys in the APK. Reuse Tanit's authenticated desktop/server flow or short-lived provider credentials where supported.
- Clearly indicate that live mic audio may be sent to cloud providers; offer stop/mute and configurable local-history retention. Recording/export is opt-in.
- Keep original audio and speaker fingerprints local by default, with deletion controls.

## 12. Minimal C++/JNI contracts

```cpp
struct TranslatorConfig {
    std::string language_a;       // e.g. "de"
    std::string language_b;       // e.g. "es"
    enum class Mode { Instant, Conversational, Utterance } mode;
    int vad_end_silence_ms = 500;
    bool auto_direction = true;
    bool speaker_tracking = false;
    bool record_original = false;
};

enum class SpeakerId { Unknown, A, B };
enum class Direction { Unknown, AtoB, BtoA };

struct Utterance {
    uint64_t id;
    uint64_t start_mono_ns, end_mono_ns;
    Direction direction;
    SpeakerId speaker;
    float direction_confidence, speaker_confidence;
    std::string source_text, translated_text;
    bool committed = false;
};

// Illustrative facade; reuse project types and thread conventions.
class TranslatorSession {
public:
    void configure(const TranslatorConfig&);
    void start();
    void stop(bool drain);
    void setDirection(Direction, bool pin);
    void setPlaybackMode(TranslatorConfig::Mode);
    void onCapturePcm(const float* pcm, size_t frames, uint64_t t_ns);
    void onPlaybackRendered(const float* pcm, size_t frames, uint64_t t_ns);
};
```

Expose Kotlin state/events only: `Idle`, `Listening`, `SpeechDetected`, `Translating`, `Playback`, `Reconnecting`, `Error`; transcript deltas, utterance completions, directions, confidence and metrics. JNI should **not** be called once per 10 ms DSP frame.

## 13. UI: compact mini-app

Shipped on the phone:

- Target language, router, model, noise, playback, silence (200–2000 ms), and jitter buffer (0–5000 ms) in grouped settings. Save writes the sheet.
- Favourite languages sit under the level meter and call `update_language` on the live session.
- The title shows `Switching to …` and then `Live · <language> · ready`.
- Stop shows `Stopping…` immediately.
- Source and translated text, copy, lock-screen session, optional source WAV, transcribe-after-stop.

Still design:

- Large **DE ↔ ES** reverse control and automatic direction.
- Speaker A/B/unknown only when confidence is meaningful.
- Replay, slower playback, and captions-only.
- If streaming cannot keep pace, show `Translation delayed` rather than pretending subtitles are live.

## 14. Delivery plan

### P0 — Smallest working proof

- [ ] Verify our WebRTC APM captures clean PCM and receives **rendered playback** as AEC reference.
- [ ] Add standalone local WebRTC VAD with hysteresis; log onsets/ends.
- [ ] Add OpenAI dedicated translation WebSocket adapter with 24 kHz PCM16 / 200 ms pacing and continuous silence.
- [ ] Implement instant and **VAD-gated output PCM** modes.
- [ ] Support manually selected DE→ES / ES→DE direction first.
- [ ] Add bounded output queues, correct audio route changes and duplicate-output prevention.
- [ ] Test one-direction, speakerphone, 10-minute continuous capture/playback.

### P1 — Two-way conversation

- [ ] Add two-session direction router with pre-roll and inactive-session time preservation.
- [ ] Add source-language evidence and confidence-based automatic routing; unknown/manual fallback.
- [ ] Integrate whistle / wake commands without leaking command audio to translations.
- [ ] Add barge-in, TTS duck/stop policy and playback arbitration.
- [ ] Add transcript history with stable utterance IDs and original/translation timing.
- [ ] Optionally implement `Scribe Live -> LLM translation -> ElevenLabs TTS` as an accurate-mode benchmark.

### P2 — Optional quality features

- [ ] Add simple MFCC/F0 speaker profiles and `UNKNOWN` classification.
- [ ] Add session post-processing/reclassification and SRT export.
- [ ] Add consent-based original-audio recording and history retention settings.
- [ ] Benchmark neural speaker embedding/Scribe diarization against the DSP baseline.
- [ ] Add lazy Whisper loading and a **complete** offline translation pipeline if required.

## 15. Test matrix and acceptance criteria

Use real Android hardware, ideally the previously tested realme ARM64 phone, and repeat recordings with **both** direct-mic and loudspeaker playback.

| Test | What to measure |
|---|---|
| DE → ES, ES → DE | Translation accuracy, direction mistakes |
| Fast 1–3 word replies | Missed/clipped beginnings, wrong language |
| Long sentence with pauses | Premature playback, semantic reversals |
| Interruption during TTS | AEC leakage, VAD response, playback policy |
| Whistle while TTS plays | Missed triggers and false activations |
| Two people overlapping | Unknown labels, wrong turns, graceful degradation |
| Quiet voice / traffic / music | VAD false starts/ends and speaker confidence |
| Bluetooth and speakerphone | Route changes, AEC effectiveness, delay |
| Network loss / reconnect | No replayed or duplicated speech |
| 10–30 minute session | Queue growth, memory, battery, thermal throttling |
| Cold start / offline toggle | Time to first useful translation; model load impact |

Instrument timestamps for capture, VAD onset/end, send, first translation text, first output audio, playback start/finish and queue length. Track **first-audio latency**, **end-of-utterance-to-playback latency**, speaker/direction error rate and backlog separately. Target sub-second provisional captions and approximately 1–2 seconds of turn-end-to-speech latency as goals to **measure**, not guarantees.

**Acceptance for first milestone:** clear one-way conversation translation, conversation-mode playback gated by local VAD, no self-transcription loop under normal speakerphone use, stable audio queues, correct manual direction, and recording-independent operation. Two-way automatic direction and diarization are later milestones.

## 16. Key risks and decisions

1. **Single mic cannot isolate real overlapping speakers.** Prefer turn-taking and fallback/unknown states.
2. **Buffering output delays playback but cannot repair early semantic decisions.** Accurate mode must use committed transcription + text translation + TTS.
3. **Two OpenAI sessions are not automatically bidirectional.** Local direction routing, time preservation and session arbitration remain our job.
4. **AEC requires a real playback reference.** Merely enabling APM is insufficient.
5. **DSP speaker profiles are probabilistic.** Keep speaker identity optional, post-processable and never a hard translation dependency.
6. **Android cold-start and device routes matter.** No blocking Whisper startup or unbounded JNI/audio work.
7. **Privacy and API cost matter.** Continuous streaming, silence cadence, optional second session and retained audio should be measured and disclosed.

## 17. Output artifacts and export options

Live playback, recording and export are separate concerns. Enabling playback
must not implicitly retain audio, and enabling recording must state exactly
which stage is saved.

### 17.1 Available audio products

| Output | Contents | Primary use |
|---|---|---|
| Source raw | Device capture before AEC/NS/gain | DSP investigation; explicit diagnostic consent only |
| Source clean | PCM after local AEC/NS/gain, before provider resampling | Normal source-language recording |
| Source speech-trimmed | Clean source with non-speech removed and a time map | Review, datasets, compact archive |
| Translation provider | Decoded provider PCM before jitter buffering, gain or route conversion | Provider comparison and debugging |
| Translation rendered | Samples actually submitted to the output device, including inserted silence and local processing | Truth for what the listener heard and AEC reference |
| Translation speech-trimmed | Rendered translation with leading/trailing/inter-turn silence removed | Compact listening/export |
| Bilingual mix | Source and rendered translation arranged sequentially or on separate channels | Human review and sharing |
| Per-utterance clips | Stable source/translation clip pairs keyed by utterance ID | Replay, correction and dataset export |

Do not label provider PCM as “played audio.” It may include continuous silence,
late packets or samples that overflowed/never reached the device. The rendered
tap is authoritative for playback history.

Raw source is more privacy-sensitive and usually unnecessary. Default recording,
when enabled, should be **source clean** plus metadata. Provider and rendered
translation are opt-in outputs.

### 17.2 Text and subtitle products

| Output | Timing basis | Notes |
|---|---|---|
| Source transcript | Capture/VAD timeline | Provider deltas are provisional until the utterance closes |
| Translated transcript | Provider output timeline | Keep delta text separate from stable utterance text |
| Bilingual transcript | Shared utterance IDs | Source and translation side by side |
| Source subtitles | Source utterance start/end | SRT/VTT; word timing only when the STT backend provides it |
| Translation subtitles | Actual render start/end | Must follow rendered audio, not network arrival |
| Bilingual subtitles | Source and rendered timelines | Separate tracks preferred; stacked cues optional |
| Event JSONL | Monotonic event timeline | Lossless machine-readable source for regeneration |

SRT cannot represent revisions, confidence, device identity or multiple clocks.
Keep JSONL/manifest as the canonical export and derive SRT/VTT after the session.
Never rewrite a subtitle cue that has already been presented as final without
recording the revision.

Suggested event records include:

- room/session generation, device and direction;
- utterance ID and source/translation relationship;
- capture start/end, provider first/last delta, queue and render start/end;
- source/translated text, language and confidence;
- audio artifact path, sample range and original-to-trimmed time-map range;
- interruption, truncation, overflow, reconnect and user-correction markers.

### 17.3 Trimming and timeline preservation

“Trimmed” must be explicit:

- **Edge trim:** remove only leading/trailing silence from a file or utterance.
- **Speech compact:** remove internal non-speech intervals.
- **Conversation compact:** preserve turn gaps up to a configured maximum and
  shorten longer gaps.
- **No trim:** preserve real elapsed time for forensic replay and subtitle sync.

Do not destructively trim the only recording. Write a derivative and retain a
time map:

```text
original_start_ns, original_end_ns
trimmed_start_sample, trimmed_end_sample
reason = leading_silence | vad_gap | playback_silence | dropped_provider_silence
```

For translated output, provider-generated digital silence may be omitted before
queueing. That is transport cleanup, not an editing decision; count it in the
manifest. Silence intentionally rendered for pacing remains part of the
untrimmed rendered artifact.

### 17.4 Containers and layouts

- WAV: one PCM stream; good for source-clean, rendered translation or a clip.
- FLAC: lossless compact archive where the platform encoder is available.
- Opus/Ogg: shareable speech output and peer-media capture; lossy.
- MKA/MKV: preferred future session package for multiple synchronized audio and
  subtitle tracks.
- SRT/VTT: derived human subtitle formats.
- JSONL + JSON manifest: canonical metadata, event and provenance output.

A bilingual stereo WAV (`source=L`, `translation=R`) is convenient but does not
model different clocks, sample rates or overlapping devices well. Prefer
separate tracks in a session container or separate files plus a manifest.

### 17.5 Proposed option surface

Avoid a boolean for every artifact. Use repeatable output selectors plus focused
format/trim options:

```text
--output source-clean=source.wav
--output translated-rendered=translated.wav
--output subtitles-bilingual=session.vtt
--output events=session.jsonl
--output manifest=session.json
--trim none|edges|speech|conversation
--trim-max-gap-ms 1500
--audio-format wav|flac|opus
--subtitle-format srt|vtt|jsonl
```

Equivalent Android settings should be presets, not a wall of checkboxes:

- **Nothing saved** (default for live-only use);
- **Transcript only**;
- **Source + transcript**;
- **Source and translation**;
- **Full session package** (advanced/debug).

An explicit advanced screen may customize tracks, trimming, codec and retention.
CLI test inputs such as `--src`, loop and timed cancellation are not recording
outputs.

### 17.6 Suggested presets

| Preset | Artifacts |
|---|---|
| Private live | Nothing persisted |
| Meeting notes | Bilingual transcript, VTT, manifest |
| Review | Source clean, rendered translation, bilingual transcript, manifest |
| Compact share | Speech-trimmed translation, translated subtitles |
| Dataset | Per-utterance clean source/translation pairs, JSONL, time maps and consent record |
| Diagnostic | Raw + clean source, provider + rendered translation, events, metrics and manifest |
| Multi-device room | One track per device/direction plus peer transport/render timestamps |

### 17.7 Output TODOs

- [ ] Define `OutputSpec`, artifact kinds and retention/consent in shared C++.
- [ ] Add taps for raw capture, clean capture, provider output and actual render;
  keep file I/O off audio callbacks.
- [ ] Persist stable utterance IDs and original/rendered monotonic timestamps.
- [ ] Implement lossless JSONL/manifest first, then derive SRT/VTT.
- [ ] Add edge trimming with a reversible time map; benchmark speech compaction
  separately from live VAD.
- [ ] Add bounded writer queues, disk-full handling and partial-artifact status.
- [ ] Add per-utterance source/translation clip export without re-encoding when
  the selected container permits it.
- [ ] Add MKA/MKV multi-track packaging after separate-track exports are stable.
- [ ] Apply two-party consent and per-device provenance to softly paired rooms.
- [ ] Test that disabled recording creates no recoverable audio artifact.

## 18. Future deployment scenarios

The single shared phone remains the first supported topology. Multi-device
operation is a separate transport and coordination problem; it must not be
implemented by making two independent translator screens listen to the same
room.

| Scenario | Capture | Translation owner | Playback | Initial policy |
|---|---|---|---|---|
| Two people, one shared phone | One phone mic | That phone | That phone | Coordinated half-duplex |
| Two people, one phone each, nearby | Each phone captures its owner | Capture-side phone | Peer phone only | Coordinated half-duplex; headset preferred |
| Two people, one phone each, remote | Each phone captures locally | Capture-side phone | Peer phone only | Full duplex when both routes have working AEC |
| Two headsets / earbuds | Each headset mic | Capture-side phone | Peer headset | Preferred full-duplex topology |
| Two phones plus paired desktop | Phones remain audio endpoints | Phone or explicit relay capability | Intended peer only | Desktop supplies signaling/observability, not an implicit PCM hop |
| One phone plus Bluetooth speaker | Phone mic | Phone | Selected Bluetooth route | Measure route latency; fall back to half-duplex |

Do not play the same translation on both phones by default. In an `A -> B`
turn, phone A captures A, owns the provider session targeting B's language,
and sends the translated audio to phone B. Phone B is the only normal renderer.
The reverse direction is symmetric.

```text
person A -> phone A mic -> A-to-B translation -> encrypted media -> phone B speaker -> person B
person B -> phone B mic -> B-to-A translation -> encrypted media -> phone A speaker -> person A
```

Captions and state events may appear on both devices. Audio has one elected
renderer per direction. Optional local monitoring is a separate, explicit
accessibility setting and must not create duplicate room playback.

### 18.1 Tanit soft pairing

A **soft pair** is a short-lived association for one translation room. It is
not permanent account trust, speaker identity, or permission to expose all
phone capabilities.

The invite/rendezvous exchanges:

- room ID and short-lived join secret;
- authenticated Tanit device IDs and ephemeral media keys;
- supported languages, codecs, sample rates and route capabilities;
- role preference (`A`, `B`, observer) and preferred output device;
- LAN candidates and optional cloud/TURN rendezvous candidates;
- protocol version and feature bits.

The room state is explicit:

```text
unpaired -> inviting/joining -> negotiating -> paired-idle
         -> live -> degraded/reconnecting -> ended
```

Joining requires visible consent on both phones. Ending, revoking, changing
account, or exceeding the idle lifetime invalidates room keys. A saved desktop
pairing or `tanit://pair` record may help discover/sign the invite, but it must
not silently authorize microphone or media access for another phone.

The control plane carries room membership, direction leases, language changes,
mute, route state, utterance metadata and health. The media plane carries
audio. Do not tunnel live PCM through chat SSE, tool calls, log shipping, VFS,
or the desktop REST control path.

### 18.2 Media transport and packet contract

Prefer an existing encrypted real-time media transport (WebRTC
DTLS-SRTP/Opus, with STUN/TURN when required) over polling HTTP or a custom
unbounded PCM socket. LAN-only direct transport is the first slice.

The capture-side phone receives 24 kHz provider PCM, resamples at the transport
boundary if the selected codec requires it, and sends translated audio to the
peer. The destination jitter buffer decodes and renders it. The destination's
**actual rendered samples** feed its local AEC reverse stream.

Every media/control stream needs:

- `room_id`, `generation`, `direction` and unique `stream_id`;
- monotonically increasing packet sequence and media timestamp;
- codec, sample rate, channels, frame duration and discontinuity marker;
- source utterance ID when known (best-effort, never required to decode);
- bounded jitter/backlog limits and an explicit late/drop policy;
- sender capture/translation timestamps and receiver render timestamps for
  diagnostics.

Use media timestamps and estimated monotonic-clock offsets; do not assume wall
clocks match. A reconnect creates a new generation/stream ID. Late packets from
an old generation are discarded. Never replay queued speech after reconnect
without visible user action.

### 18.3 Routing and floor control

The room coordinator owns a small routing state machine, not the provider
session:

```text
Idle -> AHasFloor -> DrainingAtoB -> Idle
Idle -> BHasFloor -> DrainingBtoA -> Idle
```

For nearby speakerphone use, only one direction initially owns the floor.
While `A -> B` translated audio is queued or rendered:

- phone B renders it and feeds that render into B's AEC;
- phone B sends capture-time silence upstream unless barge-in is accepted;
- phone A may continue local VAD for the end of A's active utterance, but must
  not treat peer playback leakage as a new turn;
- both devices receive `render.begin/end` and queue-depth state so neither
  independently guesses when the floor reopened.

For remote/headset full duplex, both directions may be active. Each direction
still has an independent stream, jitter buffer and provider session. Full
duplex is enabled only after route capability checks; it is not inferred merely
because two phones joined.

### 18.4 Nearby-phone acoustic-loop constraint

Two phones on a table are harder than one phone. Each phone can cancel its own
speaker using its local render reference, but phone A cannot reliably cancel
sound emitted by phone B: that audio crossed a network, a different clock and
an unknown room path.

Therefore:

1. Prefer earbuds/headsets for full duplex.
2. Default nearby speakerphone pairing to coordinated half-duplex.
3. Elect one renderer per direction and avoid mirrored playback.
4. Exchange render state for gating, but do not claim that exchanging remote
   PCM creates a valid AEC reference.
5. Detect sustained loop evidence (translated transcript repeating recent
   output, growing queue, output-correlated mic energy) and automatically pause
   the affected direction with a visible warning.
6. Never solve a loop by allowing an unbounded mute or queue. Reopen, degrade
   to push-to-talk, or stop with an actionable error.

### 18.5 Ownership, recovery and UX

- One device is room coordinator; either peer can take over using a higher
  room generation after coordinator loss.
- Provider sessions stay owned by their capture-side device. A coordinator
  handover does not migrate an in-flight provider WebSocket.
- Language or role changes apply at an idle/drained boundary. Direction changes
  carry a monotonically increasing revision.
- Losing a peer stops its outgoing media immediately and drains or discards
  incoming audio according to a visible policy.
- A degraded room shows which leg failed: capture, provider, peer transport,
  decoder, route or render.
- UI shows **You speak German -> Anne hears Spanish** rather than only `DE ↔ ES`.
- Both phones expose mute, leave, route selection, peer health and latency.
  Recording requires consent from both room members and records provenance.

## 19. Multi-device TODOs

### M0 — Contracts and deterministic simulation

- [ ] Define versioned room, peer, direction-lease, media-stream and health
  schemas in shared C++.
- [ ] Add two in-process virtual endpoints with independent clocks, packet
  delay/loss/reordering and bounded jitter buffers.
- [ ] Verify target-only playback, no duplicate render, generation rejection,
  drain, cancellation and role/language changes without Android UI.
- [ ] Record capture/VAD/provider/send/receive/decode/render timestamps in one
  trace format.

### M1 — Soft pair and one-way LAN media

- [ ] Add QR/link invite, visible two-device consent, short-lived room keys and
  explicit revoke/leave.
- [ ] Negotiate one direct LAN media stream with authenticated encryption.
- [ ] Run `A capture -> A translation -> B render`; B returns queue/render
  health and actual playback timestamps.
- [ ] Handle Wi-Fi changes, app backgrounding, audio focus and peer loss without
  replay or a stuck microphone lease.

### M2 — Bidirectional coordinated mode

- [ ] Add the reverse stream and one provider session per direction.
- [ ] Implement floor requests, grant/release, render begin/end and drain
  acknowledgement.
- [ ] Keep continuous provider-time silence where required without transporting
  or rendering unbounded silence.
- [ ] Add push-to-talk fallback and explicit interruption policy.
- [ ] Test 1–3 word replies, long pauses, simultaneous starts and a peer speaking
  during the other's playback.

### M3 — Full duplex and route capability

- [ ] Classify routes: handset, speakerphone, wired headset, Bluetooth SCO/A2DP
  and USB.
- [ ] Enable full duplex only for measured-safe routes; retain coordinated mode
  as fallback.
- [ ] Add loop detection and output-correlation telemetry.
- [ ] Test nearby phones at several distances/volumes and remote phones over
  LAN, mobile data and TURN.

### M4 — Operations, privacy and cost

- [ ] Define reconnect/backoff, coordinator handover, room expiry and provider
  quota behavior.
- [ ] Add per-direction bitrate, packet loss, jitter, queue depth, first-audio
  latency and end-to-render metrics without logging conversation content.
- [ ] Document whether translated audio traverses Tanit infrastructure, TURN or
  only a direct peer link.
- [ ] Add two-party recording consent, local retention and deletion/export
  controls.
- [ ] Measure two simultaneous provider sessions, media transport and AEC for
  battery, thermal load, data use and API cost.

### Multi-device acceptance scenarios

| Scenario | Required result |
|---|---|
| A speaks, both phones nearby | Translation plays once on B; no repeat enters either mic |
| B replies after a short pause | B-to-A opens without a multi-second stale mute |
| A and B start together | Deterministic floor winner or visible overlap fallback; no mixed duplicate output |
| Headsets on both phones | Stable full duplex with independent directions |
| One phone switches to speaker | Capability renegotiates and safely degrades if needed |
| Wi-Fi changes during speech | New generation reconnects; old queued audio is not replayed |
| Peer force-stops | Other phone releases room/audio leases and reports peer loss |
| Coordinator disappears | Peer elects/takes over or ends cleanly; no split-brain renderer |
| Packet loss/reordering | Bounded jitter; no unbounded latency or queue growth |
| Provider sends continuous silence | Silence is not transported/rendered as an endless active turn |
| Phones sit close at high volume | Loop detector warns/degrades before repeated translation runs away |
| Recording enabled | Both users consent; artifacts identify direction/device and can be deleted |

## 20. Android app feature gap

Checked against Google Translate, Microsoft Translator, iTranslate, DeepL, SayHi,
Papago, Timekettle, Wooask, and Vasco MultiTalk. Tanit should not copy camera,
dictionary, or phrasebook products. The relevant gap is conversation routing.

Already on the phone:

- One live speech-to-speech session, Tanit or OpenAI. Language changes stay on that socket.
- Simultaneous and conversational playback, silence time, and jitter buffer.
- Near-field / far-field noise reduction, AEC, and half-duplex during playout.
- Live source and translated text, with `transcript.done` as the punctuated line. Copy.
- Lock-screen session, stop feedback, and a ready state after `session.updated`.
- Favourite languages under the level meter.
- Optional source WAV (one file across a language change) and transcribe-after-stop.
- Target language, router, and model.

| Feature other apps already ship | Who | Fit for Tanit |
|---|---|---|
| Face the other person: split or flipped screen | Google, Microsoft, iTranslate | High. One phone, two readers, no second device |
| Each person uses their own phone and language | Microsoft, Vasco | High. This is the soft-pair room |
| Join by QR or short code, including a browser | Microsoft | High. Do not require the Tanit app for a guest |
| Presenter: one speaker, many listeners, captions only | Microsoft, Timekettle listen mode | High. No spoken playback in the room |
| Replay the last turn, slower or louder | iTranslate, planned whistle commands | High. Needs a completed-utterance buffer |
| Save and export the bilingual conversation | Timekettle, Wooask | High. Use the output contract in section 17 |
| Type when the room is too loud | Microsoft, Google | Medium. Same translation session, text instead of PCM |
| Headphones hear the translation; the room does not | Google Live Translate, Timekettle earbuds | Medium. A route preset, not a new translator |
| Ordinary Bluetooth earbuds as the private output | Wooask | Medium. Same route preset; do not claim earbud AEC |
| Auto-detect who is speaking and flip direction | Google Conversation | Later. Unsafe on one mic until direction confidence exists |
| Group chat, each member picks a language | Vasco, Microsoft | Later. Same room model, more than two peers |
| Offline speech | Google text/camera packs; Wooask for a few pairs | Later. Text packs are not a spoken translator |
| Camera, menus, documents, handwriting | Google, DeepL, iTranslate | Out of scope for this audio session |
| Translated phone calls | Timekettle | Out of scope. Android cannot take the call audio |
| Cloned voice | Newer voice-translator apps | Out of scope until a voice-consent design exists |

First additions that match the current app, in order:

1. Split view on one phone, with the far side flipped.
2. Captions-only mode, for a lecture or a noisy table.
3. Replay and slower playback of the last completed turn.
4. Text fallback into the same session.
5. Guest join by QR or short code, captions first and audio second.
6. Headphone route that keeps translated speech out of the room mic.

## 21. References

- OpenAI Realtime Translation guide: https://developers.openai.com/api/docs/guides/realtime-translation
- OpenAI translation client events (update, PCM append, close): https://developers.openai.com/api/reference/resources/realtime/translation-client-events
- OpenAI translation server events: https://developers.openai.com/api/reference/resources/realtime/translation-server-events
- ElevenLabs Scribe Live diarization: https://elevenlabs.io/docs/eleven-api/guides/how-to/speech-to-text/realtime/diarization
- ElevenLabs turn prediction/commit behavior: https://elevenlabs.io/docs/eleven-api/guides/how-to/speech-to-text/realtime/transcripts-and-commit-strategies
- WebRTC APM interface (reverse-stream reference): https://webrtc.googlesource.com/src.git/+/0f46441772a1e30b4297e7e6b919fdc6f69b1b4f/modules/audio_processing/include/audio_processing.h
- WebRTC VAD frame/mode overview: https://github.com/wiseman/py-webrtcvad
- Android foreground-service microphone requirements: https://developer.android.com/develop/background-work/services/fgs/service-types
- Android background foreground-service restrictions: https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start
