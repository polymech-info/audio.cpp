# Audio filters — DeepFilterNet on 48 kHz files, GTCRN on realtime speech

Two products, two models. Same `apply_file` / `audio filter` / `audioFilter`
surface (`--filter deepfilter|gtcrn`). `audio record --filter` and xblox
`audioRecord.filter` run that file pass after capture (not on the callback).

| Path | Model | Rate | Runtime |
|------|--------|------|---------|
| HQ recordings / video mux people listen to | **DeepFilterNet3** | **48 kHz** | vendored `packages/DeepFilterNet` **libDF C API** (tract). Not our ORT. |
| Live realtime voice agent | **GTCRN** | **16 kHz** | `OnnxSession` (state is graph I/O). **Shipped** (trial). |

`--filter gtcrn` on files / video / `audioFilter` stays **reserved** (16 kHz
speech, not a 48 kHz master). Voice listen **does** run live GTCRN on
completed 16 kHz utterances. Default mix is **full wet** (`--filter-blend 1`).
`--filter-blend 0` is dry; values in between lerp original and enhanced PCM.

No sidecar `deep-filter.exe`. Runtime is `deepfilter.dll` (libDF cdylib),
soft-loaded via `library_utils` like ORT / face landmarker. Models resolve through
`local_model_catalog` (`${MODELS_DIR}`, `exe/../models`, including
`deepfilter/` and `gtcrn/`). CMake copies the DFN tar to `dist/models/deepfilter/`
and `gtcrn_simple.onnx` to `dist/models/gtcrn/`.
Missing files: `tanit-cli hg download deepfilternet3` / `tanit-cli hg download gtcrn`.

Do **not** load DFN ONNX in Microsoft ORT (GRU state is tract-pulse only).
Do **not** stack WebRTC NS + a neural enhancer.

Related: [video-next.md](./video/video-next.md) (APM AutoGain / normalize
flags, captions / idle), [filters-next.md](./video/filters-next.md)
(virtual **cam** only). SRT → ElevenLabs voice-over is **shipped**
(chapter at the end) — not a filter, not Dubbing.

---

## Decision (locked)

| Question | Answer |
|----------|--------|
| DSP alone (WebRTC NS) enough? | **Yes for office STT.** No for a mic people hear. |
| First file enhancer | **DeepFilterNet** (48 kHz WAV, libDF C API) |
| First live enhancer | **GTCRN** (16 kHz, after AEC) |
| Why not DFN in our ORT | Graphs hide GRU/conv state; tract pulse only |
| Why not ORT + DFN ONNX | Graphs hide GRU/conv state; stream quality collapses |
| Why not llama.cpp / ggml | No DFN/GTCRN ggml model |
| Zoom / Teams virtual mic | **GTCRN is enough for speech.** Not for music / 48 kHz. |
| Stack NS + GTCRN? | **No.** Keep AEC+HPF+AGC; replace WebRTC NS when GTCRN is on. |
| Run on the WASAPI callback? | **No.** Hop-buffer on a worker (same rule as APM / beauty). |

Office STT stays on today’s APM. GTCRN is on the **realtime voice agent**
(`RealtimeVoiceSession`) as a trial — recognition quality TBD over time.
WebRTC NS (stationary Wiener) will not get us there; cranking `kVeryHigh`
muffles consonants. Do not stack NS + GTCRN.

---

## Slices

| Slice | What | Status |
|-------|------|--------|
| **1** | File DeepFilterNet @ 48 kHz via libDF C API + catalog | **Shipped** — CLI `audio filter` / `audio record --filter`, live `video record --filter` (mic/mix), xblox same |
| **2** | Live 16 kHz GTCRN after AEC, NS off, worker | **Shipped (trial)** — `RealtimeVoiceSession` only. `FEATURE_AUDIO_GTCRN` default ON + `k_realtime_gtcrn` |
| **2b** | File / `--filter gtcrn` / `audio denoise` | **Reserved** — do not claim 48 kHz |
| **3** | Virtual **mic** (WASAPI capture endpoint) | **Skipped** — not vcam |
| **4** | Live DFN on `audio_realtime` | **No** — wrong rate |

Store / MSIX: keep the neural flags **OFF** until the models are signed
sidecars. Desktop MSI/EXE first, same as vcam. Viewer / Linux presets
already set `FEATURE_AUDIO_GTCRN=OFF` (no ONNX).

---

## Tech stack (shipped)

Worker / file, never the miniaudio callback.

| Piece | Where | Notes |
|-------|--------|--------|
| Shared API | `src/audio/filter/audio_filter.hpp` | `Kind::{DeepFilter,Gtcrn}`, `apply_file()` |
| DFN backend | `src/audio/filter/deepfilter.cpp` | Soft-load `deepfilter.dll`, hop `df_process_frame` |
| C decls | `src/audio/filter/deep_filter_capi.h` | `df_create` / `df_process_frame` / `df_free` |
| GTCRN file stub | `src/audio/filter/gtcrn.cpp` | Reserved error; same `apply_file` surface |
| GTCRN live | `src/audio/filter/gtcrn_live.cpp` | STFT 512/256/sqrt-hann + `OnnxSession` + hop worker |
| CLI | `tanit-cli audio filter` | Same flags as xblox (`--filter`, `--pf`, `--model`) |
| CLI record | `tanit-cli audio record --filter deepfilter` | Dry capture, then `apply_file_inplace` on `--dst` |
| CLI video | `tanit-cli video record --audio-source mic --filter deepfilter` | Live hops on the AAC mux worker (mic/mix/cameraAudio only) |
| Realtime | `src/core/audio_realtime.cpp` | Mic leg after AEC; WebRTC NS off when GTCRN opens |
| XBlox | `audioFilter` / `audioRecord.filter` | File in → 48 kHz mono s16 WAV out; record can do both |
| Model resolve | `local_model_catalog` | `${MODELS_DIR}` then `exe/../models`, incl. `deepfilter/` / `gtcrn/` |
| Model fetch | `tanit-cli hg download deepfilternet3` / `gtcrn` | Named-file catalog (`onnx-tar` / `onnx` + GitHub raw URL) |
| Vendored crate | `packages/DeepFilterNet` | Official libDF + tract; not our ORT |
| Prebuilt | `packages/DeepFilterNet/prebuilt/win-x64/bin/deepfilter.dll` | ~16 MB **cdylib**. `build-capi.ps1` |
| Load | `library_utils` | Same as `onnxruntime.dll` / face landmarker. No static link. |
| Stage | CMake `FEATURE_AUDIO_FILTER` | DLL next to exe; tar → `dist/models/deepfilter/` |
| Stage | CMake `FEATURE_AUDIO_GTCRN` | `gtcrn_simple.onnx` → `dist/models/gtcrn/` (needs `FEATURE_ONNX`) |

```
tanit-cli audio filter
  → decode any format → 48 kHz mono f32
  → library_utils::load(deepfilter.dll)     /* first use only */
  → df_create(DeepFilterNet3_onnx.tar.gz)
  → df_process_frame per 480-sample hop
  → write 48 kHz mono s16 WAV
```

DLL search: exe dir, `bin/`, then `packages/DeepFilterNet/prebuilt/…/bin` and cargo `target/release`. Missing DLL is a runtime error; the rest of the app does not link tract.

Do **not** reimplement `tract.rs` in C++. Do **not** `LoadLibrary` the official `deep-filter.exe`. Do **not** feed `enc.onnx` / `erb_dec.onnx` / `df_dec.onnx` to `OnnxSession`.

---

## What else is already in tree

| Piece | Where | Notes |
|-------|--------|--------|
| WebRTC APM | `src/audio/echo_cancellation.cpp` | AEC + HPF + NS + AGC2 |
| Defaults | `src/audio/audio_constants.hpp` | `k_apm_*` all `true`; NS level = WebRTC default (~12 dB) |
| Wired today | `AudioInput` @ **16 kHz mono** only | STT / realtime. APM **off** at 48 kHz. |
| Record / xblox | `MixedAudioCapture` | **48 kHz stereo**, gains only — no APM, no enhancer |
| Cloud denoise | `audio voice-change --remove-background-noise` | ElevenLabs only |
| ONNX Runtime | `FEATURE_ONNX` / `OnnxSession` | Image/OCR/detect today; **the** GTCRN runtime |
| Virtual **cam** | `FEATURE_VIDEO_VCAM` / `VcamSink` | Zoom sees **Pixlwiz Cam**. Not audio. |

Rates:

| Path | Rate | Channels |
|------|------|----------|
| STT / AEC / Whisper | 16 kHz | mono s16 |
| Realtime API | 24 kHz | mono |
| Record / mux | 48 kHz | stereo |

GTCRN is native **16 kHz**. That matches the live speech path. It does
**not** match 48 kHz record without a resample (and a telephone-band
result if you upsample back).

---

## GTCRN vs DeepFilterNet vs DSP

| | WebRTC NS (now) | **GTCRN** | DeepFilterNet 2/3 |
|--|-----------------|-----------|-------------------|
| Native rate | whatever APM is (16 kHz today) | **16 kHz** | **48 kHz** |
| Params / compute | few MMACs/s | **48 K / 33 MMACs/s** | ~1.8 M / 350 MMACs/s |
| RTF (i5-class) | ~0.01–0.03 (full APM) | **0.07** | ~0.2–0.4 |
| Work per hop | &lt;0.3 ms / 10 ms | **~1.1 ms / 16 ms** | ~2–4 ms / 10 ms |
| Weights | 0 | **`gtcrn_simple.onnx` ~0.5 MB** | DFN3 tar ~8 MB (LL ~36 MB) |
| Extra RAM | 0 (already linked) | ~2–8 MB (ORT session + ~80 KB caches) | ~20–40 MB inside `deepfilter.dll` |
| Runs in our ORT? | n/a | **Yes** — state is graph I/O | **No** — tract pulse only |
| C++ API | in-tree APM | shipped: `gtcrn_live.cpp` + `OnnxSession` | shipped: `df_process_frame` via `library_utils` |
| New build dep | none | none if ORT stays on | rustc once (`build-capi.ps1`); runtime = 16 MB DLL |
| License | existing | MIT | MIT / Apache-2.0 |

GTCRN’s own VCTK-DEMAND table (vs **DeepFilterNet 2022**, not DFN3):
SISNR 18.83 vs 16.63, PESQ 2.87 vs 2.81, STOI 0.940 vs 0.942. Treat that
as “tiny model beats 2022 DFN,” not “beats DFN3.” DFN3 is later and
full-band; GTCRN never sees energy above 8 kHz.

Official GTCRN streaming graph (`stream/onnx_models/gtcrn_simple.onnx`):

```
mix            [1, 257, 1, 2]      one STFT frame (n_fft=512, hop=256, sqrt-hann)
conv_cache     [2, 1, 16, 16, 33]
tra_cache      [2, 3, 1, 1, 16]
inter_cache    [2, 1, 33, 16]
→ enh + the three caches back out
```

Hop 256 @ 16 kHz = **16 ms**. Algorithmic delay with overlap ≈ **30–40 ms**.
Fine for Zoom if we do not add a big FIFO.

Do **not** pull all of sherpa-onnx for one 0.5 MB model. Prefer
`OnnxSession` + a small STFT/ISTFT helper. sherpa’s
`OnlineSpeechDenoiser` is the reference implementation
([PR 1977](https://github.com/k2-fsa/sherpa-onnx/pull/1977),
[online GTCRN](https://github.com/k2-fsa/sherpa-onnx/pull/3324)).

DeepFilterNet C API (shipped, resolved from `deepfilter.dll`):

```c
DFState* df_create(const char* model_tar_gz, float atten_lim_db, const char* log_level);
size_t   df_get_frame_length(DFState*);   /* 480 samples @ 48 kHz = 10 ms */
float    df_process_frame(DFState*, float* in, float* out);
void     df_free(DFState*);
```

Official GitHub releases ship `deep-filter.exe` / LADSPA — we do **not**
use those. We build the vendored crate as a **cdylib**
(`cargo rustc -p deep_filter --release --features capi -- --crate-type cdylib`)
and rename to `deepfilter.dll`. Static `df.lib` (~180 MB, tract inside the
exe) was rejected.

---

## Hardware

DSP-only incremental cost is **zero** — APM is already on the 16 kHz path.

GTCRN is **~7% of one core** while the mic is open. Next to Whisper
(RTF 0.3–1.0) or a local LLM it does not show up. No GPU. Do not enable
CUDA EP for a 33 MMAC/s model.

Beauty + `Pixlwiz Cam` + Zoom + GTCRN on one desktop is in budget.
DeepFilterNet is the one with a real CPU *and* build-system cost, and
only pays off at 48 kHz.

| Goal | What to run | Hardware |
|------|-------------|----------|
| Live STT, office | **DSP only** (current APM) | already paid |
| Live STT, cafe / keyboard | AEC + **GTCRN** | +7% of one core, +0.5 MB |
| Zoom/Teams talking-head | AEC + **GTCRN** → virtual mic | same + virtual-endpoint work |
| 48 kHz recordings / Original Sound | **DFN file** (`audio filter`) | +one core-slice; 16 MB DLL on demand |
| Always-on, every build | DSP. GTCRN behind `FEATURE_AUDIO_GTCRN` + `k_realtime_gtcrn` | missing onnx = pass-through |

---

## Zoom / Teams / Meet — virtual A/V

Video: already shipped. Zoom picks **Pixlwiz Cam** (1280×720 NV12 @ 30).
See [filters-next.md](./video/filters-next.md) TODO 3.

Audio: **not shipped.** Zoom needs a separate **virtual capture
endpoint** (WASAPI mic). That driver/session work is larger than wiring
GTCRN. Do not pretend the vcam DLL can carry audio.

### Is GTCRN enough?

**Yes for speech meetings.** Default Zoom/Teams/Meet treat the selected
mic as speech; the codec is often 16 kHz (sometimes 32). GTCRN’s ceiling
is mostly hidden.

**No for “sounds like my real mic.”** Falls over when the user enables:

- Zoom **Original Sound** / high-fidelity music
- Teams **music / stereo**
- any 48 kHz podcast expectation

Then 16 kHz enhance + upsample is dull (nothing above 8 kHz). That is
the DFN / Krisp / NVIDIA Broadcast class.

| Bar | GTCRN as virtual mic |
|-----|----------------------|
| Raw laptop mic + Zoom’s own NS | Noticeably better on keyboard, fan, room |
| WebRTC NS we already have | Better on non-stationary noise |
| Zoom / Teams built-in suppression | Competitive; sometimes more “processed” |
| Krisp / NVIDIA / DFN3 @ 48 kHz | **Short** — narrower band |

### Product rules for a virtual speech mic

1. **Present 16 kHz mono.** Do not upsample-and-claim 48 kHz. Fake hi-fi
   in the device list sounds worse.
2. **Tell the user to disable Zoom/Teams NS** when Pixlwiz Mic is
   selected. Two suppressors muffles consonants (same as Krisp’s setup).
3. **Keep AEC + HPF + AGC; GTCRN replaces WebRTC NS.** Zoom still does
   far-end AEC. Our AEC only matters if we also play local TTS/preview.
4. **Latency:** stay in the 30–40 ms algorithmic budget. No big FIFO.
   Worker thread, not the WASAPI callback.
5. **Mono only.** One GTCRN state. Stereo virtual mics are out of scope.
6. Advertise it as a **speech** mic, not a general “Pixlwiz Mic @ 48 kHz”.
   If we ever want that name, that is live DFN (not the file path we
   already have).

---

## Architecture (live GTCRN — shipped on realtime)

```
AudioInput 16 kHz mono s16
  -> EchoCancellationProcessor          /* AEC + HPF + AGC; NS off when GTCRN on */
  -> hop buffer (256 samples)           /* not on the capture callback */
  -> filter::gtcrn::LiveHop
       -> s16 → f32
       -> STFT 512 / 256 / sqrt-hann
       -> OnnxSession(gtcrn_simple.onnx) + cache tensors
       -> ISTFT
       -> f32 → s16
  -> on_mic_pcm  (RealtimeVoiceSession only)
```

`LiveHop` is compiled next to `audio_realtime.cpp` (`gtcrn_live.cpp`),
not into `pm-audio`. Same feature-flag style as
`FEATURE_WEBRT_ECHO_CANCELLATION`: compile-time seam, runtime no-op if
the `.onnx` is missing.

```
FEATURE_AUDIO_GTCRN          CMake, default ON (presets: ON; viewer/linux OFF)
  requires FEATURE_ONNX
  loads gtcrn_simple.onnx via local_model_catalog
  soft-fail: APM NS stays on + one warn, capture never breaks
k_realtime_gtcrn             audio_constants.hpp trial flip (no CMake)
```

Do **not** route this through image `OnnxClassify` helpers. DFN file
path already ships without `FEATURE_DEEPFILTERNET` — do not add a second
flag for the DLL.

Pass-through on any internal error (load fail, ORT exception, worker
behind). Same contract as “never break the call.”

### Where it hangs

```
mic (16 kHz)
  → AEC + HPF + AGC
  → GTCRN                         /* RealtimeVoiceSession mic / mic-leg of mix */
  → realtime API                  /* 16 → 24 kHz */
```

Not on: Whisper / `audio record --stt`, desktop-only realtime, 48 kHz
`MixedAudioCapture`, file `audio filter`, video mux. Do not silently
replace a 48 kHz WAV with an upsampled 16 kHz file.

---

## Feature flags / CLI

`FEATURE_AUDIO_FILTER` (CMake + presets, default ON) compiles the DFN
file TUs. The DLL is optional at runtime.

`FEATURE_AUDIO_GTCRN` (CMake + presets, default ON) compiles
`gtcrn_live.cpp` into the realtime targets. Requires `FEATURE_ONNX`.
Trial on/off without CMake: `k_realtime_gtcrn` in
`src/audio/audio_constants.hpp`.

Shipped:

```
tanit-cli audio filter --input in.wav --dst out.wav
tanit-cli audio filter --input in.wav --dst out.wav --filter deepfilter --pf
tanit-cli audio record --dst rec.wav --filter deepfilter --pf
tanit-cli video record --input screen:0 --dst talk.mp4 --audio-source mic --filter deepfilter --pf
tanit-cli llm agent --realtime
tanit-cli hg download deepfilternet3
tanit-cli hg download gtcrn
```

`--filter` on record is the file pass after capture (48 kHz mono WAV).
`--model` on record is still STT; the DFN tar is `--filter-model`.
AAC/M4A `--dst` is refused (filter output is WAV). Default record stays dry.

XBlox: `audioRecord` with `filter: deepfilter` (default `off`), or
`audioFilter` after `audioRecord`. Params: `filter`, `filterModel`,
`attenLim`, `postFilter`, `compensateDelay`.

Reserved (do not finish unless that product is wanted):

```
--filter gtcrn            same command / block; 16 kHz out, do not claim 48 kHz
audio denoise             offline GTCRN WAV
audio record --denoise    live GTCRN on 16 kHz record / STT
live DFN on audio_realtime
```

`video-next.md` `audioAutoGain` / `audioNormalize` stay a **separate**
plumbing slice (APM @ 48 kHz mic leg, post-mix scaler). Do not block
GTCRN on that. Do not invent a second NS flag there — this doc owns
`noiseReduction`.

---

## TODO 0 — Flags + stub

- [x] `FEATURE_AUDIO_GTCRN` in CMake + presets (default ON; needs ONNX)
- [ ] Reserved `--denoise` on `audio record` / new `audio denoise` — **leave reserved**
- [x] Discover `gtcrn_simple.onnx` (`probe_gtcrn_model_path`)
- [x] Stage official
      [`gtcrn_simple.onnx`](https://github.com/Xiaobin-Rong/gtcrn/blob/main/stream/onnx_models/gtcrn_simple.onnx)
      (~0.5 MB) via CMake + `tanit-cli hg download gtcrn`

---

## TODO 1 — Offline GTCRN

Live hop reused the STFT / `OnnxSession` / caches. File WAV is **not**
the trial — leave reserved.

- [x] STFT/ISTFT 512/256/sqrt-hann in `gtcrn_live.cpp` (no new FFT lib)
- [x] `OnnxSession` named I/O `mix` / `conv_cache` / `tra_cache` /
      `inter_cache` + `*_out`
- [ ] `audio denoise --input/--dst` (16 kHz mono WAV) — **leave reserved**
- [ ] A/B fixture: keyboard-heavy clip vs APM-only vs GTCRN — watch
      realtime recognition over time instead

---

## TODO 2 — Live 16 kHz

Shipped on `RealtimeVoiceSession` (mic / mic-leg of mix). Not on
Whisper / `audio record --stt`.

- [x] After `EchoCancellationProcessor::process_capture`
- [x] Force APM NS off for that session only (`AudioInput` `apm_ns`)
- [x] 256-sample hop queue on a worker; never ORT from the callback
- [x] Pass-through if the worker falls behind (drop enhance, keep audio)
- [ ] Wire `--denoise` on `audio record` when capture is 16 kHz — **leave reserved**
- [ ] 48 kHz record downmix→16→GTCRN for `--stt` only — **leave reserved**

Acceptance: callback never blocks; missing model = one warn + APM NS.
Recognition delta is a long trial (`k_realtime_gtcrn`).

---

## TODO 3 — Virtual mic (Zoom)

Not GTCRN work — a new device, same product as vcam.

- [ ] WASAPI (or equivalent) **virtual capture** endpoint, session
      lifetime, CurrentUser. No OBS. Store / MSIX off.
- [ ] Fixed v1 contract: **16 kHz mono s16**. Session-fixed.
- [ ] Producer = the live GTCRN hop. Public API next to `VcamSink`
      (e.g. `pm::audio::VmicSink`). CLI `--virtual-mic` /
      `--virtual-mic-name "Pixlwiz Mic"`.
- [ ] Docs / in-app note: disable Zoom/Teams built-in NS.
- [ ] Do not hang audio on `tanit_vcam_source.dll`.

Acceptance: Zoom / Teams / Meet / Discord / Chrome show **Pixlwiz Mic**
while the process runs; far end hears speech with keyboard/fan reduced;
stop the process and the device disappears.

If a specific app is blind to the chosen virtual-mic API, treat that
like vcam’s DShow-32 footnote — later, not this slice.

---

## TODO 4 — XBlox

- [x] `audioFilter` after `audioRecord` (file in → file out, DFN)
- [x] Same option names as CLI (`filter`, `model`, `postFilter`, …)
- [ ] Live graphs configure the processor; do not wrap every hop in
      the xblox runner
- [ ] `--filter gtcrn` on the same block — **leave reserved** (16 kHz, not 48 kHz)

---

## Radar — live DeepFilterNet (48 kHz virtual mic)

File DFN is done. This is only if we sell a 48 kHz virtual mic
(“Original Sound” / music).

- Reuse the same `deepfilter.dll` + catalog tar; do **not** add
  `FEATURE_DEEPFILTERNET`
- Do **not** load those ONNX files in `OnnxSession`
- Do **not** stack with GTCRN
- Capture at 48 kHz for that device; APM-at-48 kHz (or resample into
  APM) is a prerequisite — same gap `video-next.md` already notes
- Hop on a worker, never the WASAPI callback
- `DeepFilterNet3_ll_onnx.tar.gz` (0 lookahead, ~36 MB) only if the
  20–30 ms DFN3 lookahead is a Zoom complaint

UL-UNAS (author’s newer ultra-light SE) — same 16 kHz idea, less
production plumbing than GTCRN today. Revisit only if the RT trial
stays flat.

---

## Non-goals

- PyTorch / `pip install deepfilternet` in-process
- Exec `deep-filter.exe` (sidecar or live)
- Static-link `df.lib` into the exe (~180 MB tract)
- LADSPA / PipeWire plugin as the Windows product
- GPU for GTCRN
- Stereo enhance
- Replacing Zoom’s AEC
- Claiming Krisp / NVIDIA Broadcast parity on a 16 kHz model
- Background-blur-for-audio metaphors. This is a speech enhancer.

---

## Play (slice 1 — DeepFilterNet file)

```
set CLI=dist\win-x64\tanit-cli.exe
set IN=tests\audio-filters
set OUT=tests\audio-filters\out
```

Model (once):

```
tanit-cli hg download deepfilternet3
# → ${MODELS_DIR}/deepfilter/DeepFilterNet3_onnx.tar.gz
# CMake also copies packages/DeepFilterNet/models/… → dist/models/deepfilter/

tanit-cli hg download gtcrn
# → ${MODELS_DIR}/gtcrn/gtcrn_simple.onnx
# CMake also copies packages/gtcrn/models/… → dist/models/gtcrn/
```

```
tanit-cli audio filter --input speech-48k.wav --dst play-dfn.wav
tanit-cli audio filter --input speech-48k.wav --dst play-dfn-pf.wav --filter deepfilter --pf
tanit-cli audio record --dst play-rec-dfn.wav --duration-ms 5000 --filter deepfilter --pf
tanit-cli video record --input screen:0 --dst play-talk.mp4 --audio-source mic --filter deepfilter --pf
```

XBlox: `audioRecord` `{ "filter": "deepfilter" }`, or `audioRecord` then
`audioFilter` `{ "filter": "deepfilter" }`.

Realtime trial (look for `GTCRN live (WebRTC NS off)`):

```
tanit-cli llm agent --realtime
```

`--filter gtcrn` on files / video stays reserved.

Virtual mic (skipped):

```
tanit-cli audio record --virtual-mic --denoise --preview
```

Pair with the existing cam when both are up:

```
tanit-cli video record --virtual-out --preview --lips
```

---

## Voice-over — SRT → 48 kHz TTS bed (shipped)

Not a filter. Captions already write timed SRT; this second act places
**scripted TTS** on those cues. Product name: voice-over. Kind:
**`audioTtsScripted`**. CLI: `tanit-cli audio tts-scripted`.

**Do not hang this on `audioSpeak`.** That block is “speak this string /
play / maybe save a clip.” A timed bed is a different job. The xblox
block is I/O only; merge / place / overflow live in `pm::tts::voiceover`.

Related captions work lives in [video-next.md](./video/video-next.md)
slice 5. Filter / GTCRN / DFN above does not feed TTS.

### Decision (locked)

| Question | Answer |
|----------|--------|
| Product | Same-language **tutorial narration bed**. Not dubbing. |
| Input | SRT / VTT / transcribe JSON (`segments` / `cues`). **Not WAV.** |
| Output | Canonical **s16 / 48 kHz / mono** WAV + `{stem}.voiceover.json` |
| Duration oracle | **PCM length after decode to 48 kHz.** `/with-timestamps` unused. |
| Placement clocks | `t0` = first cue; `nominal_t1` = last cue `t1`; `hard_t1` = next `t0` − 80 ms |
| Merge | Score (gap + punctuation + topic-shift), **not gap-only**. Cap ~8 s. |
| Overflow default | Fit in `hard_t1` → DSP ≤12% → ElevenLabs `speed` ≤ 1.2 → overlap + warn |
| Hard-cut | `overflow=truncate` only. Not default. |
| Stitching | Text XOR request IDs. Forward = text. Regen = previous IDs. |
| Voice settings | Narration set (`0.45 / 0.80 / 0.20`). Not chat `0.9 / 0`. |
| API PCM | Prefer `pcm_22050`; fall back to mp3 and decode. **Do not require `pcm_44100`.** |
| Dubbing API | **Out.** |
| On-screen SRT | Keep original. Do not rewrite cue times to match TTS. |

Honest bar: you talking over your own recording, same SRT, new voice
(or a clone). Cue-by-cue TTS sounds like an AI audiobook of an SRT.
Chipmunk-stretch or colliding lines also fail the bar. Do **not** sell
this as translation or lip-sync.

```
Whisper WAV → SRT                    /* already shipped */

A. Tanit placer                      /* this chapter — done */
   merge cues → one take / utterance
   TTS → 48 kHz PCM
   place on a silent bed
   voiceover.wav + .voiceover.json

B. ElevenLabs Dubbing                /* other product — skip */
```

### Status

| Piece | Status |
|-------|--------|
| Video idle auto-pause / encoded 16 kHz caption dump / Whisper SRT | **Shipped** (captions chapter) |
| `pm::subs::read_srt` / `read_vtt` / `read_cues_file` | **Shipped** |
| JSON cues (`segments` / `cues`, `t0Ms`/`t1Ms`) | **Shipped** — `load_cues` |
| Merge + two-clock budget + overflow | **Shipped** — `src/audio/tts/voiceover.cpp` |
| `audioTtsScripted` | **Shipped** — registered **before** `audioSpeak` |
| CLI `audio tts-scripted --srt --dst` | **Shipped** — fields append-only at end of `PmImageCliState` |
| Sample | `tests/xblox/audio_tts_scripted.xblox` + `.srt` |
| `/with-timestamps` | **Reserved** — karaoke / retimed subs / word trim only |
| Mix / duck under original video audio | **Not this slice** — WAV is the bed; mux is a later I/O block |
| ElevenLabs Dubbing / Forced Alignment | **Out** |

Whisper path for a finished recording (unchanged, no TTS):

```
encoded-timeline WAV
    → whisper_full (internal ~30 s windows)
    → segments {t0,t1,text}
    → SRT/VTT/SBV next to the MP4
```

Feed that sidecar (or `audioTranscribe` JSON) into TTS. Whisper already
heard the file.

### Files

| Piece | Where |
|-------|--------|
| Constants | `k_voiceover_*` in `src/audio/audio_constants.hpp` |
| Placer | `src/audio/tts/voiceover.hpp` / `voiceover.cpp` (`pm::tts::voiceover`) |
| Block | `src/xblox/blocks/audio_blocks_tts-scripted.cpp` — I/O + ElevenLabs adapter |
| Register | `register_audio_tts_scripted_blocks` **first**, then `register_audio_blocks` |
| TTS take | `elevenlabs_tts_synthesize_result` — `request_id`, `speed` 0.7–1.2, `seed`, context |
| Subs | `pm::subs::read_cues_file` + JSON in `load_cues` |
| CLI | `tanit-cli audio tts-scripted` in `pm_image_cmd_audio.cpp` |
| UI preview | `apps/xblox/src/xblox/react/BlockModel.ts` |

`TtsCapabilities` (adapter fills; placer branches on them):

```
speed_control, character_alignment, request_stitching
```

ElevenLabs: `speed_control` + `request_stitching` on, `character_alignment`
off. Other providers go through `proxy_tts` (mp3, no speed regen).

### Pipeline

```
SRT / VTT / JSON
  → load_cues
  → merge_utterances          /* one VoiceOverUtterance per take */
  → for each utterance:
        synth (text + neighbor text, seed)
        cache hit? skip API
        trim generated silence
        fit to hard_t1        /* see overflow */
        mix_into 48 kHz bed @ t0   /* saturating add */
  → write s16 WAV
  → write {stem}.voiceover.json
```

One take per **utterance**, never per SRT line.

### Clocks (nominal vs hard)

Do **not** treat the first cue’s `t1` as the placement window.

```
utterance.t0          = first merged cue t0
utterance.nominal_t1  = last  merged cue t1
utterance.hard_t1     = next.t0 − k_voiceover_min_inter_gap_ms     /* 80 ms */
last utterance        = nominal_t1 + k_voiceover_last_pad_ms       /* 15 s */
floor                 = t0 + 200 ms
```

```
budget  = hard_t1 − t0          /* what the placer may occupy */
nominal = nominal_t1 − t0       /* SRT box; stretch is not measured against this */
actual  = pcm.size() / 48000    /* after trim */
```

Example: cues `10.0→12.0` + `12.2→14.0`, next speech at `16.3` → start
10.0, nominal 14.0, hard 16.22. A 4.8 s take is **natural** (fits
nominal) or **used_gap** (fits hard, past nominal). Zero stretch.

The following free gap is already inside `hard_t1`. There is no second
“borrow from the next pause” pass.

### Merge (no LLM)

Estimated duration for the cap only (not the duration oracle):

```
est_ms = max(400, text.size() * 75)     /* ~13 chars/s */
```

Boundary score (integer ms + weights):

```
score  = max(0, gap_ms)
score += 400   if prev ends . ! ?          (skip trailing quotes/space)
score -= 150   else if prev ends , ; :
score -= 250   else                        (mid-phrase / wrap)
score += 500   if next first word is a topic shifter
score -= 200   if next starts with a lowercase letter

merge iff score ≤ k_voiceover_merge_score_limit (400)
       AND est_ms(combined) ≤ k_voiceover_max_utterance_ms (8000)
```

Topic-shift first words: `next then now first finally however also
meanwhile after before so anyway instead plus second third lastly
afterwards let's lets`.

Sample `tests/xblox/audio_tts_scripted.srt` (this is the fixture):

| Boundary | gap | score | Merge? |
|----------|-----|-------|--------|
| `"If you open"` → `"the settings panel."` | 500 | 500 − 250 − 200 = **50** | **yes** (wrap, lowercase continuation) |
| `"…panel."` → `"Next, we're going to…"` | 2300 | 2300 + 400 + 500 = **3200** | **no** (period + `Next`) |
| `"…encoder."` → `"Click Save…"` | 250 | 250 + 400 = **650** | **no** (period; 250 ms is not enough) |

Do not merge `"...Save."` + `"Next, we're..."` because the gap is 250 ms.
Do merge `"If you open"` + `"the settings panel."` despite ~500 ms.

### Synth + stitch

Forward pass (every utterance, in order):

```
text
previous_text = prior utterance text     /* empty on first */
next_text     = next utterance text      /* empty on last */
speed         = options.speed            /* default 1.0 */
seed          = options.seed ≥ 0 ? seed
              : FNV(provider|model|voice|text) & 0xFFFFFFFF
previous_request_ids = {}                /* IDs would drop previous_text */
```

`chunk_sentences = false` — the placer already merged.

ElevenLabs `build_tts_body`: if `previous_request_ids` is non-empty,
**omit** `previous_text`. Same for next. Max **3** request IDs.

Speed regen (only after overflow > 12%, `overflow=resolve`, ElevenLabs):

```
previous_text / next_text cleared
previous_request_ids = last ≤3 accepted take ids
speed = clamp(actual / budget, 1.0, 1.2)
```

`next_request_ids` stay empty in v1 (forward-only; the next take is not
generated yet).

Do **not** send chat defaults. Scripted voice_settings:

```
stability              0.45     /* chat uses 0.9 — monotone on short cues */
similarity_boost       0.80
style                  0.20
use_speaker_boost      true
apply_text_normalization  auto
```

Request `pcm_22050` first. If the plan rejects it, decode mp3 once via
miniaudio to 48 kHz mono. Bytes that already look like `pcm_*` are
wrapped in a tiny WAV header then decoded.

### Silence trim (before overflow math)

```
drop leading / trailing samples with |s| < 512
keep 20 ms pad on each side that still had signal
```

`actual` used below is post-trim PCM duration.

### Overflow (`overflow=resolve`, default)

`apply_fit` against **hard budget**, in this order:

```
actual <= budget:
    place @ t0, no DSP
    actual <= nominal  → natural
    actual >  nominal  → used_gap          /* free gap already in hard_t1 */

actual > budget and (actual/budget − 1) ≤ max_stretch (0.12):
    linear time-compress voiced PCM to budget samples
    → stretched

else if overflow=truncate:
    hard-cut PCM to budget
    → truncated

else:
    place full take @ t0 anyway
    saturating-add into the bed (may overlap the next utterance)
    → unresolved
```

Then, **only if** still `unresolved` and the backend has `speed_control`:

```
regen at speed = clamp(actual/budget, 1.0, 1.2)
trim, apply_fit again     /* may still stretch ≤12%, or remain unresolved */
```

DSP is preferred over a second API call when overflow is ≤12%. Regen
is the >12% path. After regen, a leftover sliver can still DSP-compress.
Hard-cut never runs on the default path.

`maxStretchPercent` is clamped 0–25 in the block; default 12.

### Cache

Directory: `cacheDir` or `<output>/.voiceover-cache`.

Key = FNV-1a of:

```
provider, model, voice, cache_extra,
text, previous_text, next_text, speed, seed, alignment
```

`cache_extra` includes stability / similarity / style / speakerBoost /
textNormalization so the old robotic chat-default takes are not reused.

Store `{key}.s16` (canonical PCM) + `{key}.json` (`request_id`, seed,
speed). Alignment flag is in the key but unused for placement.

### Report

Sidecar `{wav-stem}.voiceover.json`:

```
utterances, natural, used_gap, stretched, regenerated, unresolved
items[]: text, t0Ms, nominalT1Ms, hardT1Ms, placedT0/T1,
         generated_s, free_gap_s, final_stretch_pct,
         request_id, seed, speed, flags, from_cache
```

Block PREVIOUS is the WAV path unless `json: true` (then the report
object). CLI `--json` prints the report; otherwise a one-line summary.

### Play

```
tanit-cli audio tts-scripted --srt tests/xblox/audio_tts_scripted.srt --dst dist/tts-out/audio_tts_scripted.wav
tanit-cli --log-level trace xblox run --src tests/xblox/audio_tts_scripted.xblox
```

Sample pins `eleven_multilingual_v2` + voice `tLK6fPv15M0oKv4V3ACR`.
Live run: 3 utterances, all `natural`, `pcm_22050` accepted.

### Non-goals (this chapter)

- ElevenLabs Dubbing API
- WAV → TTS (use SRT)
- Cue-by-cue TTS
- Hanging this on `audioSpeak`
- Forced Alignment / `/with-timestamps` placement
- Translation / lip-sync
- Rewriting the on-screen SRT to match TTS
- Shrinking Whisper `audio_ctx` as a chunker (captions already forbid that)
- Requiring `pcm_44100`
- Mix / duck / mux into the original MP4 (separate I/O; bed is enough)
- Growing a mini dubbing engine on top of this placer

