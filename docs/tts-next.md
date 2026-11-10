# TTS next

Two products. Separate task lists. One stitcher.

- **Document reader.** PDFs and ebooks. Speak from a cursor, or render the whole text. Desktop `tanit-cli` first.
- **Speech studio.** A recording already on the phone. Transcribe, rewrite with a preset, direct a voice, generate, export. New editor. Not another button on the recorder list.

The Android recorder already stores a transcript and word times (`RecorderActivity`, `AudioStudio`, `TranscriptPanel`: local Whisper, cloud STT, copy, share, post into chat, voice change). Chat presets are provider configs (`ChatPreset`), not rewrite prompts. The rewrite catalog below is new.

Shared rules, not a third checklist:

- ElevenLabs direct for stitch, seed, and timestamped TTS until `/v1/audio/speech` is proven to keep them. LiteLLM is for the rewrite.
- Request stitching is documented for v4, not for v3. Do not plan either product on `eleven_v3`.
- Long-form default to confirm: `eleven_v4` (expressive, 10,000 characters) or `eleven_multilingual_v2` when stability matters more. `eleven_turbo_v2_5` stays the short/cursor alias until its cap is confirmed. Flash v2.5 is 40,000. v3 is 5,000.
- `previous_request_ids`: at most 3, newest last, about two hours, and only after that response body is fully read. Persist the audio. Treat the id as session state. Later edits fall back to `previous_text` / `next_text`, or re-render the neighborhood.
- `seed` is best-effort. Same text and settings still are not a guarantee.
- Chunk on paragraph or sentence boundaries, about 500–1,500 characters, under the model cap. Not on a fixed duration and not on source SRT cuts.
- Assemble PCM in `audio.cpp`. Do not concatenate MP3 bytes.

---

## Document reader

Desktop first (`tanit-cli`). Android waits until these commands are stable.
The direct client in `elevenlabs_tts.cpp` already sends `seed`, `previous_text` /
`next_text`, and `previous_request_ids` / `next_request_ids`, and it keeps the
upstream `request-id`. The OpenAI-compatible proxy path does not.

Two speak modes, one chunker:

- **Cursor.** From a character offset in the extracted text, speak the next N
  characters. One request when the window fits the model cap; a short stitch
  chain when it does not. Stream to the speakers. Do not render the rest of
  the document.
- **Document.** Any length. Split under the model character cap, stitch in
  order, write audio plus a manifest so a rerun resumes.

Text extraction stays where it is (`pdf md`, anydoc for pdf / epub / the other
office types). `audio.cpp` takes plain text plus an offset. The SRT voice-over
timeline is a different product and stays as it is.

### LiteLLM

Tracked alias is `tanit-speech-turbo` → `elevenlabs/eleven_turbo_v2_5` on
`POST /v1/audio/speech`. `drop_params: true` plus
`get_supported_openai_params` = `voice`, `response_format`, `speed` drops a
top-level `seed` or stitch field before the ElevenLabs transform. The speech
response is rebuilt with LiteLLM headers, so `request-id` never comes back.
The raw `/elevenlabs` pass-through would forward a native body, but no TTS
client calls it, and the custom gateway only owns Scribe and speech-to-speech.

- [ ] Overlay the ElevenLabs speech transform (same pattern as the OpenRouter
      video overlay) so `/v1/audio/speech` forwards `seed`, `previous_text`,
      `next_text`, `previous_request_ids`, and `next_request_ids` for
      `elevenlabs/*`. `drop_params: true` must not strip these.
- [ ] Copy the upstream `request-id` onto the speech response. Request-id
      stitching cannot close without it. Text stitching does not need it.
- [ ] Keep spend on character count. Seed and stitch fields do not change the
      list price. `speech_cost_hook.py` stays a price map, not a second body
      parser.
- [ ] Add a long-form alias (`eleven_v4` or `eleven_multilingual_v2`) when the
      reader or studio needs a chapter-sized chunk. Cursor reads can stay on
      turbo. Confirm turbo’s cap before using it for long chunks.
- [ ] Do not add a second TTS handler on `/elevenlabs`. The OpenAI speech route
      is the tracked one.

### audio.cpp

- [ ] Add a document speaker next to voice-over. Input is plain text, a
      character offset, and a length, or the whole buffer. Output is audio
      bytes (cursor) or a chunk manifest plus audio files (document).
- [ ] Chunk on paragraph, then sentence, under the model cap. Cursor uses the
      existing small chunks when streaming. Document uses the largest split
      that still fits the cap, targeting 500–1,500 characters.
- [ ] Stitch with `previous_request_ids` (max 3, newest last). Send
      `previous_text` / `next_text` only when ids are absent. Same rule
      `build_tts_body` already applies. Model must be one that documents
      stitching (`eleven_v4`, not `eleven_v3`).
- [ ] Derive a stable `seed` per chunk from the text, voice, and model. Store
      it on the manifest. Same span must hit the cache on a second run.
- [ ] Persist `request_id` with each chunk, with the time it was issued. If the
      id is older than two hours, resume with `previous_text` instead of the
      dead id.
- [ ] Cache key covers text, voice, model, seed, voice settings, and the stitch
      context, so a cursor replay and a full render share bytes. A chunk whose
      context is a live request id depends on that upstream generation.
- [ ] Cursor speak streams the window and stops. Document speak walks chunks
      in order, fully reading each body before the next stitch, and writes a
      manifest (`offset`, `length`, `seed`, `request_id`, path).
- [ ] Extend `proxy_tts` with the same fields and `request-id` capture once the
      proxy returns them. Until that lands, the reader calls the direct client
      only.
- [ ] On `audio tts-scripted`, pass stitch fields through the proxy branch
      after the above. Today only `provider == "elevenlabs"` sets
      `request_stitching`, and the proxy call sends the cue text alone.
- [ ] `audio tts --seed` on the direct path. `make_elevenlabs_tts_cfg` leaves
      `seed` at `-1`, so the field is omitted.

### tanit-cli

First host for the reader. No Android reader work in this pass.

- [ ] `audio read` (name flexible): a document path, then either `--from` plus
      `--length` or the whole document. Text comes from `pdf md` / anydoc.
      Play, write, or both. Document mode prints the manifest path.
- [ ] Default provider is direct ElevenLabs on a stitching model (`eleven_v4`
      unless the stability listen says `eleven_multilingual_v2`).
      `--provider tanit` stays off this command until the proxy todos are done.
- [ ] Exercise both modes on a multi-page PDF and an epub: cursor window under
      the cap, cursor window over the cap, and a document long enough to take
      several stitched requests. Second run of the same span is a cache hit.

---

## Speech studio

Record → transcribe → rewrite → direct the voice → generate → refine → export.
The user edits meaning, delivery, and sound separately. A rewrite prompt is
never sent to ElevenLabs. The provider adapter compiles a direction profile
into voice settings, punctuation, and model-specific tags, and reports a
control it cannot express.

First slice is one speaker, one voice, paragraph chunks, four rewrite presets,
non-destructive edits, and an export. Multiple speakers, translation,
performance-preserving voice change, and video lock come after that.
Voice change already exists on the recorder (five-minute source segments);
it keeps an existing performance and does not speak a rewritten script.

Rewrites go through the existing LiteLLM text route. Generation stays on the
direct ElevenLabs client, and it reuses the reader chunker rather than growing
a second one. Chunks that depend on `previous_request_ids` run in order.

### Rewrite presets

Versioned prompt templates, plus structured knobs that stay out of the free
prompt: how tightly meaning is kept, how aggressive the edit is, language,
glossary, output shape. Apply to the whole transcript or to selected
paragraphs. Long transcripts get neighboring context and a document style
note; the model may change only its assigned segments.

Shipped presets for the first slice:

| Preset | Freedom | Job |
| --- | --- | --- |
| Clean speech | Very low | Fillers, stutters, repeated words, false starts |
| Clear & natural | Low | Grammar, clarity, spoken flow |
| Professional | Medium | Concise, confident narration |
| Storytelling | Medium–high | Rhythm and transitions, no new facts |

Later, not in the first slice: Educational, Podcast, Custom.

Each preset returns segments, not audio and not invented timestamps:
`source_segment_ids`, `rewritten_text`, `change_type`, `confidence`,
`needs_review`.

- [ ] Store rewrite presets apart from `ChatPreset`. Editable text, a version,
      and the structured knobs above. Seed the four first-slice presets.
- [ ] Apply a preset to a saved transcript or to a paragraph selection. The
      call is a normal LiteLLM completion. Original segments stay. Accept,
      reject, and restore are per paragraph.
- [ ] Compare original and rewrite without dropping either. Ambiguous lines
      come back as `needs_review`, not as a guess.

### Voice direction and render

Internal profile, not the ElevenLabs body. Example fields: voice id, model,
persona, emotion, energy, pace, pauses, accent, stitching on, context window
3, glossary on. v4 audio tags (`[thoughtful]`, `[excited]`, …) are local
exceptions. A stable read uses one global direction, not a tag on every
sentence. Tags are not guaranteed; the adapter says so.

Two modes, only the second in the first slice:

- **Rewrite and narrate.** TTS of the finalized script. Presentations,
  teaching, polished narration.
- **Preserve performance.** Clean the take and optionally run voice change.
  Speeches and interviews. Already partly on the recorder. Not this slice.

- [ ] Compile the direction profile in the ElevenLabs adapter. Unsupported
      knobs are returned to the caller, not dropped.
- [ ] Finalize the rewritten script, then render. First chunk gets following
      text. Later chunks get up to three previous request ids plus following
      text. Store request id, audio, seed, and settings per chunk.
- [ ] Regenerate one chunk from its neighbors. If the join is wrong, regenerate
      the adjacent chunks. Do not rerender the project for a paragraph edit.
- [ ] CLI listen: the same three to five paragraphs, independent vs stitched,
      judged at the joins (pitch, pace, emphasis, voice). This is the gate
      before any editor UI.

### Timeline, cache, export

Source word times (Whisper or cloud STT) belong to the recording. After a
rewrite they do not time the narration. Generated timing comes from the
[timestamped convert endpoint](https://elevenlabs.io/docs/api-reference/text-to-speech/convert-with-timestamps).
Keep the source-segment → rewrite map. Do not stretch a shorter line back to
the original duration unless a picture lock asked for it.

```
recording + source words
        → rewrite preset version
        → edited script version
        → direction profile version
        → chunk plan
        → chunk audio (locked or dirty)
        → PCM assemble
        → final audio + SRT
```

- [ ] Call convert-with-timestamps from the direct client. Map characters back
      to sentences. Source times stay on the original transcript.
- [ ] Cache key: script text, voice, model, settings, glossary version, seed,
      and the continuity inputs actually used. A live `previous_request_ids`
      dependency invalidates the downstream chunk when that upstream audio
      changes.
- [ ] Decode chunks to one PCM format, trim edge silence, short fades,
      loudness, then encode. Export wav or mp3 plus an SRT from the generated
      alignment.
- [ ] Job state is per chunk and resumable: progress, cancel, retry, character
      cost. A failed chunk does not discard finished neighbors.

### Editor

After the CLI listen, not before. Android first surface is a mode opened from
a recording, not a rewrite of `AudioStudio`.

Phone: Transcript / Rewrite / Voice as tabs, player always reachable.
Desktop, when it comes: original and rewrite side by side, voice timeline
under them.

Actions: Apply preset, Compare, Restore original, Preview voice, Regenerate
selection, Export. Every transform is reversible.

- [ ] Open a saved recording into the editor with its transcript and source
      words. Empty transcript offers the existing local and cloud transcribe
      actions.
- [ ] Paragraph list with accept, reject, and restore. Preset picker edits the
      template and the knobs, then applies.
- [ ] Preview and regenerate the selected paragraph through the chunk renderer.
      Export writes the assembled file and the SRT next to the recording.

---

## Protocol

Not a product. The convert body has fields neither client sends, and v4 ignores two settings the direct client still writes. The proxy allow-list in `speech_cost_hook.py` is `seed` plus the four stitch fields. `drop_params` strips anything else.

- [x] `language_code` on the direct body and on that allow-list. ISO 639-1, or ISO 639-3 for `ast`, `yue`, `ceb`, `fil`, `kam`, `ckb`. Omit the field and v4 detects the language from the text. An unsupported code is ignored. `eleven_multilingual_v2` does not accept the field.
- [x] `audio tts --language` maps to `language_code`. No language detector of our own.
- [x] On `eleven_v4` and `eleven_v4_turbo`, send `stability` and `similarity_boost` only. v4 has no Style or Speed slider, and it does not take SSML. Delivery stays audio tags inside `text`. `clone_strength` (0–1) is the extra setting, and only for a professional clone fine-tuned on v4.
- [x] `pronunciation_dictionary_locators`, at most 3. IPA and CMU tags apply on v4, v4 turbo, flash v2, and v3. Other models skip those tags and need an alias spelling.
- [x] `apply_text_normalization` (`auto`, `on`, `off`) on the proxy body. The direct config has the field. `--text-normalization` sets it.
- [x] `apply_language_text_normalization` for Japanese. It adds a lot of latency. Default stays off. `--language-normalization` turns it on.
- [x] `use_pvc_as_ivc` when a caller wants the instant-clone read of a professional voice.
- [x] `audio_effects`: `filter_preset_id`, `distance`, `environment_id`, `background_noise_id`, `send_level`, `pan`. Reverb and a non-zero pan are stereo, and the client rejects them for `ulaw`, `alaw`, and `pcm`.
- [x] The proxy allow-list in `speech_cost_hook.py` names the fields above. `merge_convert_fields` copies them. `output_format` replaces the mapped OpenAI format. The cost hook does not parse the body.
- [x] The proxy chunk loop in `audio tts` decodes each chunk to PCM, concatenates, and writes one WAV. `chunk-join` plays that file.
- [x] The direct client sends `output_format` and nothing else on the query string. `enable_logging` stays at the API default, on. `optimize_streaming_latency` is not sent.
- [x] The proxy `response_format` stays the OpenAI name (`mp3`). `output_format` carries `mp3_44100_128` beside it.

---

## Test

`tests/tts-next` drives `tanit-cli audio tts` through the Tanit proxy.
The alias is `tanit-speech-v4` (`elevenlabs/eleven_v4`). The voice is Sarah,
`EXAVITQu4vr4xnSDxMaL`, from the premade list in `dev_config.yaml`. Input is
plain text cut from documents already under `tests/` (the onboarding script).
PDF and epub stay out until `audio read` exists. The harness writes a `.txt`
slice and passes that path as `--text`. `resolve_tts_text` already loads a file.

```
npm run test:tts-next
npm run test:tts-next:live
```

Plan mode is the default. It does not call ElevenLabs. It flattens the
fixture, cuts the three slices below, and leaves them in `tests/tts-next/out/`
(`cursor-short.txt`, `cursor-span.txt`, `document.txt`). Live mode adds the
mp3 and the CLI log beside each slice. Nothing in that folder is deleted.
The harness checks that `tanit-cli` is the one from `dist/win-x64` (or `PM_IMAGE_CLI`)
and that `audio tts` without `--text` is rejected. Build that binary with
`npm run build:cpp:cli` when it is missing. Do not rebuild from the harness.

| Case | Cut | What it stands in for |
| --- | --- | --- |
| `cursor-short` | 280 characters from the start, snapped to a word | One request, under the chunk cap |
| `cursor-span` | 1,200 characters from offset 400 | Cursor window that the current 800-character chunker must stitch |
| `document` | Paragraphs from the start until 3,500 characters | Whole-document walk, several chunks, paragraph boundaries |
| `chunk-join` | Two sentences, target 100 / max 200 | The stitch between chunk 1 and chunk 2. Tanit `tanit-speech-v4` with `previous_text` / `next_text`, and live mode plays it |

Live mode is `--live`, or `test:tts-next:live`. The Tanit key comes from
App Settings. Each case:

```
tanit-cli audio tts
  --provider tanit
  --model-id tanit-speech-v4
  --voice-id EXAVITQu4vr4xnSDxMaL
  --text <slice.txt>
  --dst <slice.mp3>
  --no-play
  --chunk-target-chars 400
  --chunk-max-chars 800
```

`eleven_v3` is the CLI default and does not stitch, so the harness always
passes `tanit-speech-v4`. `--no-chunk` is a single-request control. A live
case passes when the process exits 0 and the mp3 is non-empty. Hear-ready
commands are in [tts-examples.md](./tts-examples.md). Request id, seed, and a
second-run cache hit are not asserted here. Speech-studio listen is a later
case in this same directory.
