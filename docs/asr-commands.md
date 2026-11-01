# Voice Command Center

Status: v1 CLI/core implemented behind `FEATURE_VOICE_COMMANDS`; in-process UI
start/stop/toggle app commands are wired. XBlox `voiceListen` maps matches to
child blocks. HUD remains follow-up work.

Reference runtime: `ref/audio.cpp/README.md`.

## Goal

Provide a lightweight, cross-platform voice loop that can:

1. listen to microphone, desktop output, or both;
2. segment utterances without running a large ASR model continuously;
3. recognize a configurable wake prefix such as `Tanit`;
4. emit a versioned trigger envelope for later UI/XBlox command mapping;
5. cancel an already-running command through the shared command runtime; or
6. hand recognized free-form text to the existing realtime voice agent.

The command center and realtime voice agent are separate lifecycles. The command
center is idle/listening most of the time. The realtime agent is an active duplex
speech session and starts only after an `agent` route trigger.

## V1 architecture

```text
MixedAudioCapture (mic | desktop | mix)
  -> callback-safe bounded PCM queue
  -> worker-thread RMS VAD + preroll
  -> completed utterance
  -> DeepFilterNet (auto, off callback)
       unavailable -> dry/APM utterance
  -> selected STT adapter
  -> pure normalize + wake-prefix matcher
  -> control intent or trigger envelope
  -> trigger JSONL | realtime agent handoff
```

V1 intentionally uses ASR phrase matching as the wake adapter:

- RMS/VAD runs continuously.
- ASR runs only for completed speech segments.
- The pure matcher removes the configured prefix and rejects text without it.
- An empty wake phrase (settings or omitted `--wake-phrase`) disables the
  prefix: the whole utterance is the command.
- This supports arbitrary phrases without introducing an ONNX/KWS dependency.

This is a transitional solution. A model-backed keyword detector can later be
inserted before ASR through the adapter boundary, without changing envelopes,
command mapping, or the CLI.

## Existing stack reused

- `src/core/mixed_audio_capture.*`: mic, desktop loopback, and mix capture.
- `src/core/pcm_stt_feeder.*`: downmix/resample to 16 kHz mono STT PCM.
- `src/audio/echo_cancellation.*`: WebRTC APM for microphone capture.
- `src/audio/filter/deepfilter.*`: 48 kHz DeepFilterNet utterance enhancer.
- `src/audio/filter/gtcrn.*`: optional 16 kHz live/utterance enhancer.
- `src/audio/stt/whisper_local.*`: offline whisper.cpp batch STT. The ggml
  context is cached and preloaded on listen start; voice utterances pass
  16 kHz PCM (no temp WAV). `initial_prompt` carries the wake word plus
  opted-in command phrases. The cached session is listed as `whisper:stt`
  on run-IPC `loaded_models` and can be freed with `stop_model` the same
  way as `onnx:…` (refuses while a transcription is in flight).
- `src/audio/stt/proxy_stt.*`: Tanit/OpenAI-compatible batch STT.
- `src/audio/stt/elevenlabs.*`: ElevenLabs realtime PCM STT.
- `src/core/command_control.*`: shared command discovery and cancellation.
- `src/core/cmd_ipc.*`: cross-process listener status/stop and run IDs.
- `src/plugins/chat/host_chat.*`: conditional realtime-agent handoff.

Platform notes:

- Microphone capture is cross-platform through miniaudio.
- Desktop loopback uses WASAPI on Windows and Pulse/PipeWire monitor sources on
  Linux.
- macOS desktop capture currently requires a configured virtual device or a
  future CoreAudio process-tap implementation. Microphone mode remains usable.
- Core files under `src/audio/voice` contain no Win32/UI/XBlox types.

## Feature gate

`FEATURE_VOICE_COMMANDS` gates the runtime and top-level CLI verb. It requires
`FEATURE_STT`/audio I/O, but not ONNX. `--route agent` is available only when
`FEATURE_REALTIME` and the chat add-on are available.

The feature defaults on in the base preset and on in the development preset.

## CLI

```text
tanit-cli voice listen [options]
tanit-cli voice replay [wav] [options]
tanit-cli voice replay --text "Tanit stop" [options]
tanit-cli voice status [--json]
tanit-cli voice stop [--json]
```

Common options:

```text
--route trigger|agent                  default: trigger
--wake-phrase <text>                   optional; empty = no prefix
--filter auto|off|deepfilter|gtcrn    default: auto
--filter-model <path>
--post-filter                         aggressive DeepFilter mode; default off
--filter-blend <0..1>                 wet/dry mix; default 1 (full filter)
--provider whisper|tanit|pixlwiz|elevenlabs
--model <id-or-path>
--language <auto|de|german|de-DE>     whisper ISO code or English name; default auto
--backend cpu|gpu                     whisper.cpp only; default gpu (cpu fallback)
--gpu-device <n>                      whisper.cpp GPU index
--flash-attn auto|on|off
--threads <n>                         whisper.cpp CPU threads; 0 = auto
--jsonl,--json                        envelopes on stdout; heard speech on stderr
```

`listen` also accepts:

```text
--input-source mic|desktop|mix        default: mic
--input <microphone substring>
--desktop <loopback device substring>
--mic-gain <number>                   default: 1
--desktop-gain <number>               default: 1
--duration-ms <number>                default: 0 (until stop/Ctrl+C)
```

Behavior:

- `trigger` resolves spoken text against opted-in custom commands and
  app-command verbs, then dispatches like a hotkey (Runner or `app_cmd` IPC).
  Replay resolves only; it does not execute.
- `agent` pauses the command-center capture, seeds the realtime agent with the
  recognized command text, and resumes listening after the agent exits.
- `voice stop` cooperatively stops active `voice listen` processes through
  run-IPC.
- `replay --text` exercises matching/envelopes without audio hardware, a model,
  network access, or a build-time KWS backend.
- WAV replay supports batch whisper/Tanit STT. ElevenLabs is used for live PCM.

## UI app commands

In-process listen loop in the running UI (not `CreateProcess tanit-cli`, and
not realtime `voicestart` / `togglevoice`). Options come from pipe JSON or
`userDataJson`:

```text
voicecommandstart|{...}     aliases: startvoicecommand, voicelistenstart, startvoicelisten
voicecommandstop            aliases: stopvoicecommand, voicelistenstop, stopvoicelisten
togglevoicecommand|{...}    aliases: voicecommandtoggle, togglevoicelisten, voicelistentoggle
```

JSON keys (camelCase or snake_case):

```text
route, wakePhrase/wake_phrase, filter, filterModel/filter_model, postFilter/post_filter,
filterBlend/filter_blend, provider, model, language, backend, inputSource/input_source,
input, desktop, micGain/mic_gain, desktopGain/desktop_gain
```

Defaults match the CLI (`route=trigger`, `wakePhrase=Tanit`, `filter=auto`,
`language=auto`, `backend=gpu`, `inputSource=mic`, gains `1`). STT provider is
`whisper | tanit | pixlwiz | elevenlabs` — Moss is TTS and is rejected.
A nested `options` object overlays top-level keys.

Empty JSON uses `custom.voice_commands` plus Audio & Video source/gains.
`voice_commands.audio_input_device` is the dedicated listen mic; empty
falls back to Audio & Video `audio_input_device`, then the OS default.
`voice_commands.backend` is `gpu` (default) or `cpu` for local whisper.cpp.
GPU load failure falls back to CPU until the next listen start.
Saving that settings page reloads a running in-process listener. Matching
lives in core (`src/core/voice_commands.*`), the same catalogs as hotkeys
and CLI — not the leftover ribbon command IDs:

- opted-in `commands.json` entries (`asVoiceCommand`); spoken aliases are
  `voiceCommand` when present (comma / `;` / `|` / newline, or a JSON
  array — e.g. `screenshot, screen shot`), otherwise the command name/title
- built-in app-command verbs (id, label, aliases, including `${named}` leftover
  templates such as `search ${query}`)

STT text is stripped of Unicode punctuation, Whisper `[…]`/`(…)` artifacts,
filler words, and a leftover possessive `s` after the wake word
(`Sonic's screenshot` → `screenshot`). Exact / compact phrase first;
then a carrier+`${named}` leftover template on the same alias list
(`search cats` against `search ${query}` shows native search with `cats`); then a
suffix-anchored edit-distance near match (covers `queen shot` →
`screenshot`) and a stem-near match for dropped trailing letters
(`explore` → `explorer`, both ≥ 6 compact chars, length delta ≤ 2);
if a local embedding model is loaded (`EmbeddingModel` /
`embed_inference`), cosine ≥ 0.72 with an 0.08 lead over the runner-up
is accepted. Templates match on spaced normalized text only; the leftover
is opaque (no compact/near/embed on `cats`). Bare `search` with an empty
slot still opens the overlay. A leftover prefers show over toggle so an
open overlay is not closed. CLI `voice listen`
dispatches a match the same way a global hotkey does (Runner or `app_cmd`
IPC). The UI runs app commands in-process and custom commands through the
shared command catalog.

Each matched utterance also fills command variables `${VOICE_WAKE}` (prefix),
`${VOICE_COMMAND}` (text after the wake), and `${VOICE_FULL}` (raw transcript).
They expand in custom commands and XBlox like `${CURRENT_FILE}`. Pass
`--VOICE_COMMAND value` as extra args, or rely on the last utterance in this
process (`PM_VOICE_*` is stamped onto spawned children). Slot leftovers such as
`${query}` stay separate from these three.

## XBlox

`voiceListen` (Audio group) is a named listen instance, same lifecycle shape
as `videoCapture` Still / Start / Stop / Status. Hidden aliases:
`voiceReplay`, `voiceStop`, `voiceStatus`.

```text
action     Still | Start | Stop | Status | Replay
instance   default (parallel listeners use distinct names)
phrase     optional filter (spoken text, compact form, or catalog id)
text       Replay only: "Tanit screenshot"
```

Behavior:

- **Still** — wait for one matching utterance, run `items`, then stop (or wait
  on an already-running instance without tearing it down).
- **Start** — keep listening. With child items, each match runs them
  (PREVIOUS = `pm.voice.trigger/v1` plus `intent.commandId`) until Stop or
  cancel. Without children, Start returns immediately and the instance stays up.
- **Stop** / **Status** — same `instance` name.
- **Replay** — inject `text` through the shared matcher (no mic / STT).

On match, child blocks run only when `phrase` is empty or the spoken/catalog
id matches — the same “filter then Then” idea as `videoDetect`. Catalog
dispatch is off by default (`dispatch=false`); children are the mapping.
`${VOICE_WAKE}`, `${VOICE_COMMAND}`, and `${VOICE_FULL}` are bound before
`items` run (same as a custom command). The listen loop lives in
`VoiceListenHost` (`src/audio/voice`), not a copy inside `audio_blocks.cpp`.

## Enhancement policy

`--filter auto` is the POC default:

1. microphone capture keeps WebRTC APM noise suppression enabled;
2. each completed 48 kHz utterance is enhanced by DeepFilterNet on the worker;
3. missing DLL/model or inference failure falls back to dry/APM audio;
4. the requested/effective filter and fallback reason are included in the
   trigger envelope.

Explicit `--filter deepfilter` and `--filter gtcrn` are strict. `--filter off`
does not load an enhancer. `--post-filter` enables DeepFilterNet's aggressive
noise attenuation and remains opt-in because it can suppress quiet speech.
`--filter-blend` (0..1, default **1**) mixes dry utterance with the enhancer
output. `0` is original PCM (the enhancer is skipped). There is no separate
GTCRN blend today beyond this; `--filter gtcrn` with the default blend is
100% wet.

DeepFilter/GTCRN, file I/O, STT, IPC, and JSON never run on the audio callback.

## Core composition

`src/audio/voice` contains:

- `voice_types.*`: pipeline configuration, PCM and event value types.
- `voice_adapters.*`: capture, enhancement, transcription, and route callback
  tables. Adapter contexts must outlive the pipeline.
- `voice_match.*`: pure ASCII-normalization, wake-prefix, and control matching.
- `voice_envelope.*`: `pm.voice.trigger/v1` and `pm.voice.cancel/v1`.
- `voice_pipeline.*`: bounded queue, preroll, RMS VAD, worker, replay, and
  cancellation.

The adapter tables are deliberately plain function pointers plus context:

```cpp
struct VoiceAdapters {
    CaptureAdapter capture;
    EnhancementAdapter enhancement;
    TranscriptionAdapter transcription;
    RouteCallbacks routes;
};
```

This permits whisper, provider STT, future audio.cpp, ONNX KWS, and tests to be
composed without inheritance or platform coupling.

## State and threading

```text
stopped -> starting -> listening
listening -> processing -> listening
listening|processing -> stopping -> stopped
any setup/worker failure -> failed
```

Rules:

- The capture callback only copies into a fixed-capacity queue.
- One worker owns VAD, utterance assembly, enhancement, STT, and routing.
- Queue overflow drops the incoming callback block and increments diagnostics.
- Utterance length, silence, preroll, and queue memory are bounded.
- Stop is idempotent; atomics and condition variables wake all waits.
- Ctrl+C and run-IPC stop use the existing CLI cancel sink.
- No UI thread joins or inference are introduced.

## Envelopes and cancellation

Trigger output uses `pm.voice.trigger/v1`; control output uses
`pm.voice.cancel/v1`. Every CLI envelope adds a point-in-time activity snapshot:

```json
{
  "schema": "pm.voice.trigger/v1",
  "triggerId": "voice-trigger-...",
  "text": "open settings",
  "raw_text": "Tanit open settings",
  "route": "trigger",
  "enhancement": {
    "requested": "auto",
    "effective": "deepfilter",
    "fallback": ""
  },
  "intent": {
    "kind": "command.candidate",
    "execute": false
  },
  "activity": {
    "runner": [],
    "external": [],
    "instances": []
  }
}
```

Activity rows expose only cancellation-safe identifiers:

- stable `commandId`;
- local `runId`;
- cross-process `instanceId`;
- PID, source, label, and start time.

IPC descriptor tokens are never serialized.

Control policy:

- `Tanit stop listening` stops the voice loop.
- `Tanit stop` / `Tanit cancel` emits a cancel envelope.
- If exactly one cancellable command ID is active, it is stopped through
  `command_control::stop_command_by_id`.
- Multiple candidates are returned unresolved for the future UI mapper; the
  runtime does not guess.

## Tests

`npm run test:voice:commands` provides offline-first orchestration:

- clean skip when the feature or CLI binary is absent;
- command/help and enum validation;
- `status --json` schema;
- deterministic `replay --text` trigger/cancel envelopes (matcher
  goldens plus filter `requested` metadata; `--text` does not load
  DeepFilter/GTCRN);
- spoken MP3 fixtures from `tests/voice-commands/fixtures` first;
- optional `--text` / generated-tone fallbacks when fixtures are absent;
- opt-in `--heavy-filters` for spoken dry vs DeepFilter (tree-killed on
  timeout so a hung enhancer cannot stall the suite);
- opt-in bounded mic/desktop/mix checks with device/provider soft-skips.

Create spoken fixtures with:

```text
bash tests/voice-commands/create_fixtures.sh
```

No microphone is required for deterministic matching/envelope tests. The harness
replays those spoken files before falling back to `--text`.

## Next TODOs

Priority is **testability, precision, and speed**. XBlox / KWS / larger ASR
workers stay parked until the current whisper+catalog loop is measurable.

### Testability

- [x] `--text` matcher cases for possessive wake leftovers
      (`Tanit's screenshot` → `screenshot`) and compact/near hits
      (`queen shot` → `screenshot`), asserting `intent.commandId`.
- [x] Spoken MP3 fixtures in `tests/voice-commands/fixtures` (wake +
      command, cancel). STT wake misses (`Tannit` / `Tonnette` / `TENET`)
      soft-skip instead of treating an empty trigger as success.
- [ ] Record per-utterance timings on the trigger envelope: VAD end →
      enhance ms → STT ms → match ms. `voice replay` should print them
      so regressions are a number, not a feel.
- [ ] Golden-file the catalog resolver: `replay --text` → `intent.commandId`
      for every `asVoiceCommand` phrase, including Whisper-style
      punct/possessive variants.
- [x] Filter/STT comparison on spoken files is `--heavy-filters` only;
      default filter checks use `--text` and do not load DeepFilter.

### Precision

- [x] STT normalize: Unicode punct, Whisper `[…]`/`(…)`, fillers, leftover
      possessive `s` after the wake word (`Sonic's` → command without `s`).
- [x] Core catalog matcher: exact / compact, suffix-near edit distance,
      stem-near (`explore` → `explorer`), then embedding cosine.
- [x] Whisper `initial_prompt` from wake + opted-in phrases (biases
      `small.en` away from `Queen shot` for `screenshot`).
- [ ] Measure miss rate on a fixed spoken set before chasing AGC. Current
      evidence: RMS/peak are usable; misses are decoder substitutions on
      `whisper` `ggml-small.en.bin`.
- [x] Add per-command spoken aliases in `voiceCommand` (e.g. `screen shot`)
      rather than growing a global confusion table. Split on comma / `;` /
      `|` / newline (not space). Built-in `takescreenshot` includes
      `screen shot` so Whisper's split form is a catalog phrase.
      Carrier+`${named}` leftover slots live on the same list
      (`search ${query}`, `search for ${query}`) and dispatch
      `showsearchnative|{"query":"cats"}`.
- [ ] Compare `small.en` vs `base.en` vs `tiny.en` on the same replay
      corpus: word error on wake+command only, not free-form dictation.
- [ ] Optional temperature-0 / `max_tokens` cap for the trigger route
      (not agent) if prompt+near-match still miss short commands.
- [ ] Reject low-confidence / no-speech segments instead of routing
      hallucinated leftovers.

### Speed

- [x] Cache the whisper.cpp context across utterances; preload on listen
      start. The previous path `whisper_init` + `whisper_free` per
      utterance was the ~2 s stall (and the `dropped=8000` queue overflow).
- [x] Skip temp-WAV I/O for in-process whisper (PCM 16 kHz float).
- [x] End-of-speech wait `silence_ms` 700 → 450.
- [ ] Log one line per utterance at info: heard + match + `sttMs`. Drop
      periodic idle RMS (done) and duplicate heard/state lines (done).
- [ ] Time GTCRN vs `filter=off` on the same clip; keep GTCRN only if it
      wins WER, not because it is the settings default.
- [ ] Consider a smaller English command model or GPU whisper if cached
      `small.en` is still >400 ms on a 2 s clip.
- [ ] Later: keyword-spot the wake word so ASR never runs on non-wake
      speech (see Lightweight KWS).

### Later

- [x] XBlox: `voiceListen` / `voiceReplay` / `voiceStatus` / `voiceStop`
      against `VoiceListenHost` (`audio_blocks_voice_commands.cpp`).
- [ ] HUD: listening/processing/degraded plus a privacy-visible mic
      indicator (status-bar toggle already exists).
- [ ] Lightweight KWS before ASR; keep ONNX optional.
- [ ] audio.cpp worker only after a replay-corpus bench beats cached
      whisper on wake+command WER and latency.
