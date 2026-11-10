# TTS orchestration

This layer writes the text that TTS speaks. It does not synthesize audio.
The reader in [tts-next.md](./tts-next.md) still speaks plain text when the user
wants the page as it is. This pipe is the other choice: a document or a photo
goes through an interpreter, then a short agent pass, and the speakable text
is what `audio tts` receives.

The user keeps both. The interpreter text stays on disk next to the speakable
text. Speaking is a later step, and it can use either file.

Desktop `tanit-cli` first. The Windows loader is
`packages/commons.cpp/src/markdown/markdown_win.cpp`. It calls `anydoc.dll`.
Android does not compile that file. The phone keeps its own loader.
`audio.cpp` does not call it, and it does not link `llm.cpp`. The `reader`
command does the call, then stores the markdown.

Scope is one page the user is looking at, or a batch of pages. A batch is a
list of page jobs, not one prompt stuffed with the whole file.

---

## Pipe

Two interpreters, one tail.

```
page or image
    |  scope: this page | these pages
    v
interpreter
    doc  -> anydoc, or pdfium when the user needs a page cut or OCR
    image -> ocr_text (PictureTools), prompt is the guide
    v
source text, kept
    v
intent pre-step          fast internal preset, one turn, no tools
    v
direction turn           same preset, unless the intent opens tools
    v
extra turn               only when the intent says so (translate, clarify)
    v
speakable text, kept
    v
audio tts                existing client, unchanged
```

Each box is a function with a typed input and a file it writes. A later box
reads the earlier file. Skipping the direction turn is valid: the speakable
text is then the source text.

Context is always passed in. It is not inferred.

- The user wants this spoken.
- Preferred spoken language, when they have set one.
- Scope: path, page number or page list, and for an image the file path.
- Direction, when they typed one: "what is this", "only the totals", "translate".

Intent is not a form. A pre-step reads the source text plus that context and
returns a small result: `speak`, `what`, `only`, `translate`, `clarify`, or
`fact_check`, plus the language to use. A typed direction wins over the guess.

---

## Interpreters

anydoc is the fast document interpreter and a poor one on complex PDFs.
`pdf md` defaults to it (`--decoder anydoc`). It converts the whole file.
`anydoc_incompatible` refuses `--pages`, `--per-page`, `--json`, `--password`,
`--pipe`, and OCR flags. A scanned page returns `NeedsOcr` and the CLI tells
the user to switch to pdfium `--pipe with-ocr`.

pdfium is the page interpreter. `pdf md --decoder pdfium --pages 3` and
`pdf render --pages 3` already exist. OCR of a PDF page is `raster_and_ocr`:
render the page to PNG, then run the configured OCR provider. That path is
inside `pdf md`, not a function the orchestration can call without the CLI.

`ocr_text` in `packages/llm.cpp/src/llm/tools/pictures/PictureTools.cpp` is the
image interpreter. Input is image paths. The default prompt is verbatim text
with line breaks. A caller can replace that prompt, which is the guided OCR
for an invoice, a contract, or a supermarket label ("only the line items and
totals", "read the price and the product name"). Provider resolution is Chat
OCR, then the recognition slot, then a local VLM on the `ocr` slot. There is
no PDF input. A PDF page reaches it only after `pdf render`.

The user picks the interpreter. The pipe does not silently replace anydoc with
OCR. When anydoc returns `NeedsOcr` or the page text is empty, the result says
so and the speakable step does not run.

| Input | Interpreter | Page cut | Guided prompt | Gap |
| --- | --- | --- | --- | --- |
| pdf, epub, office | anydoc | no | no | junk on complex pages, whole file only |
| pdf | pdfium `pdf md --pages` | yes | no | CLI, not a library call from this pipe |
| pdf scan | pdfium render + OCR | yes | only inside `pdf md` | same |
| photo, label, camera still | `ocr_text` | n/a | yes, `prompt` | agent tool, not a direct library call |

---

## Internal presets

The host is `AgentFactory` in `packages/llm.cpp/src/llm/agent_factory.cpp`,
the same way other library callers build an `Agent`. This pipe does not call
`llm agent`. Chat presets stay the user's provider settings. The reader does
not read a rewrite prompt out of them.

`prepare` takes an optional turn function. With none, or with LLM off, the
speakable text is the source text. A direction is stored and `pending` is
true, so a later turn can still run. The `reader` command leaves LLM off
unless `--llm` is passed.

The fast agent, when that turn is connected, is a text-only `Agent`: system
prompt feature only. Tools, MCP, skills, and the planner stay off. One turn.
`ask` and `check` keep that clamp. Their tool allow-list is empty until a
tool is named.

| Preset | Clamp | When |
| --- | --- | --- |
| `fast` | no tools, no MCP, no skills, no planner, one turn | Intent, "what is this", "only X", translate |
| `ask` | same clamp | Clarification. One question, no catalog |
| `check` | same clamp, tools still unknown | Fact check. Not enabled until a tool is named |

`PlannerStrategy::IntentOnly` classifies a chat turn so the planner can skip
tool schemas. It is not this speech-intent list, and this pipe does not turn
the planner on to get it.

The clamp is `preset_flags` in `packages/audio.cpp/include/pm/audio/tts/reader.hpp`.
The command is gated by `FEATURE_READER`. `audio tts` is not part of this command.

---

## Turns

1. Interpret. Write `source.txt`. No LLM.
2. Intent. One `fast` turn. Input is a clipped source (the start of the page,
   not the whole batch) plus the context. Output is JSON: intent, language,
   and whether an extra turn is required. On a parse failure the intent is
   `speak` and the language is the user's, or unchanged text when they set none.
3. Direction. One `fast` turn unless the intent is `speak` and no direction
   was typed. Then the speakable text is the source text.
4. Extra turn. Only `translate`, `clarify`, or `fact_check`. `translate` stays
   on `fast`. `clarify` uses `ask` and must return a question, not a speech.
   `fact_check` does not run until a tool is chosen. Guessing a web search
   here would turn the pipe into a general agent.

The direction prompt sees the source text, the intent, the language, and the
typed direction. It does not see tool schemas. The text it returns is the only
thing TTS receives. Rewrite instructions are not sent to ElevenLabs. That rule
is already in the studio notes and holds here too.

---

## Scope

One page: the host passes a 1-based page number. Batch: the host passes a
page list or `all`. Each page is its own interpreter job and its own
speakable file. The batch manifest lists them so a later speak of "this page"
does not rerun the other pages.

There is no CLI for "the page the user is looking at". `pdf --pages` is an
explicit number. The viewer’s current page is a host input this pipe does not
have yet. Desktop tests pass `--page 3` or `--pages 1-3`.

An image job has no page index. A camera capture is an image path once the
file exists. `do_camera_phone` is Android-only and is out of this pass.

---

## Files

Under a job directory the user can open. Nothing is deleted between runs.

```
source.txt          interpreter text
intent.json         pre-step result
speak.txt           text handed to audio tts
job.json            path, pages, interpreter, preset, language
```

A batch adds `page-3/source.txt` and `page-3/speak.txt`, and `job.json`
lists the pages. TTS reads `speak.txt`. The existing harness in `tests/tts-next`
can point `--text` at that file when a listen is wanted. This pipe's own
tests stop at `speak.txt`.

---

## Unknowns

- Spoken language has no setting. `display_language` is the Win32 UI language.
  `VoiceCommandSettings.language` is the listen language and defaults to `auto`.
  Which of those, if either, is the voice the user wants to hear is unset.
- How bad anydoc is on the fixtures we would actually speak. `NeedsOcr` is a
  hard failure. "Junk but non-empty" has no detector.
- Whether a pdfium page extract is callable without shelling `pdf md`. Today
  it is a CLI command in `src/cli/pm_image_cmd_pdf.cpp`.
- Whether `ocr_text` is callable without an agent turn. The function is
  `do_ocr` inside the tool file. The orchestration should call that, not ask
  the agent to decide to call a tool, or the fast preset cannot reach it.
- Which tool a fact check would be allowed to use. None is named. MCP stays off
  until one is.
- Whether `IntentOnly` can be reused as the pre-step. Its fields are
  intent, action, subject, confidence for tool selection. The speech pre-step
  wants a closed set of speech intents. Treating those as the same will
  mis-route "only the totals" into a tool plan.
- The current viewer page is not on the CLI. A desktop command cannot see it.
- Cost and latency of a guided OCR on a rendered PDF page versus anydoc on
  that same page. Unknown until one invoice and one text-native page are timed.
- Camera stills. The Android camera tool writes a file. This pipe accepts the
  path. Who invokes the capture is outside the pipe.

---

## Next tests

Desktop only. `npm run test:tts-orch` does not call a model. Generated files
stay in `tests/tts-orch/out/`.

- [x] `reader --text` with LLM off writes `source.txt`, `intent.json`,
      `speak.txt`, and `job.json`. `speak.txt` equals the source.
- [x] A direction with LLM off keeps the source and sets `pending`.
- [x] `reader --print-preset` reports tools, MCP, skills, and the planner off.
- [ ] One text-native PDF page through pdfium `--pages`, and the same file
      through anydoc. Save both source texts. The test asserts anydoc has no
      page cut and pdfium output is only that page.
- [ ] A scanned or photo fixture. Record the `ocr_text` prompt and the image
      path. A live call of `do_ocr` is a later flag.
- [ ] Connect `--llm` to `AgentFactory` with the fast clamp. One intent turn
      writes `intent.json`. A broken reply falls back to `speak`.
- [ ] One translate turn. `speak.txt` is the translation. `source.txt` is unchanged.
- [ ] Point `audio tts --text` at `speak.txt` for a single sentence fixture.
      That is the handoff. No new TTS behavior.

`fact_check` and the viewer’s current page stay out until the unknowns above
are answered.

## Try it

`tanit-cli` is on PATH. Run these from the repo. Nothing here calls a model.
`--dst` is the reader root. The job lands in `<root>/<path hash>/`. Omit
`--dst` and the root is the config directory `reader` folder. `source.md` is
the text anydoc returned, or the text file itself. `page-N.md` is what speech
reads. `--next` moves the cursor. `--stop` holds it.

Which interpreter a path would use:

```
tanit-cli reader --print-interpreter --text tests/reader/sample.epub
```

The sample book. Page 1 is the title, page 2 is chapter one, page 3 is chapter two:

```
tanit-cli reader --text tests/reader/sample.epub --dst tests/reader/out
```

`--src` and `--input` take an image, a folder of images, or a glob, and may be repeated. Repeated options stay in the order given. One glob or one folder is ordered by the number in the name, so `plate_1.png` comes before `plate_2.png` and `plate_10.png`.

```
tanit-cli reader --src tests/reader/out/seq/plate_*.png --dst tests/reader/out --no-speak
```

The same file again. This prints `cached` and does not convert:

```
tanit-cli reader --text tests/reader/sample.epub --dst tests/reader/out
```

The job directory is the path plus the interpreter, so `text` and `anydoc` do not share a cache. `--refresh` writes the pages again. A rewrite is spoken only when it was made from that source. Older `page-N.vK.md` files stay in the directory. `--clean` deletes the directory and exits.

```
tanit-cli reader --text tests/reader/sample.epub --dst tests/reader/out --clean
```

Speak is the output of this command. It writes `page-N.mp3` next to the page
and plays it. Ctrl+C cancels the next chunk and stops playback. A page longer
than `--chunk-max-chars` (800) is split on sentences, each request gets the
neighbour text, and the result is one wav. `--voice-id` is an ElevenLabs id.
Empty uses the chat voice, then Sarah `EXAVITQu4vr4xnSDxMaL`. The command
prints `voice <id>` before the audio path. `--no-play` only writes the file.
`--no-speak` caches the pages and does not call the proxy.

Chapter one, spoken, and the mp3 left in the job directory:

```
tanit-cli reader --text tests/reader/sample.epub --dst tests/reader/out --next --no-play
```

`--all` joins every page, using each page's latest spoken version, into one file.
The script is `spoken.md`. The audio is `document.mp3`, or `document.wav` when the
text takes more than one chunk. `--audio` overrides that path. Before speech it
writes `plan.json` and prints `progress plan`. Each chunk then prints
`progress speak i/n chars done/whole elapsed Ns estimate Ms left`. The estimate
starts once the first chunk has finished. `--print-estimate` checks the same
line without calling the voice proxy.

```
tanit-cli reader --text tests/reader/sample.epub --dst tests/reader/out --all --no-play
```

The `read` tool does this for an agent. `path` is a file or an http(s) URL.
`prompt` rewrites the joined text once. `play` defaults to false. On Android the
phone voice writes one wav, chunked the same way. PDF and office stay on the
desktop loader.

A prompt rewrites the current page only. `page-N.md` stays the converted
source. The spoken text is `page-N.v1.md`, then `page-N.v2.md` for the next
prompt. `--chat-preset` selects the saved chat router and model for that turn.
One text turn, no tools. Ctrl+C cancels it. A later pass can run the
same prompt across the whole document, one chunk at a time.

```
tanit-cli reader --text tests/tts-orch/fixture.txt --dst tests/tts-orch/out --prompt "summarize, in german" --chat-preset "My Preset" --no-play
```

`--interpreter` overrides the extension map. The names are `text`, `anydoc`, `vision`, `image_understand`, and `web`. `vision` renders each PDF page to a picture and reads that picture. `--prompt` is the spoken direction. It is stored on the job and used by `--optimize`. The page-image prompt is `--vision-prompt`, or an inferred ingest record that keeps every printed line and every picture. That record does not include the spoken direction. `--pages 34-38` keeps those PDF pages, as reader pages 1 through 5. Word, PowerPoint, and EPUB have no page renderer, so `vision` stops on those files. `--no-speak` renders the pages and records both prompts, and does not call the vision model. Leaving `--no-speak` off calls `image_understand` on each page, then speaks.

A picture-book PDF, 53 PDF pages. The default interpreter is anydoc. These stay on `--no-speak`, so nothing calls a vision model or the voice proxy. The parent prompt is the one below. `--pages` counts PDF pages. On this file the number printed in the corner is one lower, so PDF page 34 shows 33.

What the extension map would do:

```
tanit-cli reader --print-interpreter --text tests/reader/child-ebook.pdf
```

Anydoc on the whole file:

```
tanit-cli reader --text tests/reader/child-ebook.pdf --dst tests/reader/out-child --no-speak
```

Pages 34–38 as pictures. Reader page 1 is PDF page 34. The job records `source_pages` and an inferred `vision_prompt`. Each page is `page-N.png` plus `page-N.md`. A run against an existing job prints `cached`.

```
tanit-cli reader --text tests/reader/child-ebook.pdf --dst tests/reader/out-child --interpreter vision --pages 34-38 --prompt "Read this page aloud to a child. Keep the story gentle. Use the printed words, and skip anything that is not part of the story." --no-speak
```

The same pages again. This prints `cached`:

```
tanit-cli reader --text tests/reader/child-ebook.pdf --dst tests/reader/out-child --interpreter vision --pages 34-38 --prompt "Read this page aloud to a child. Keep the story gentle. Use the printed words, and skip anything that is not part of the story." --no-speak
```

PDF page 36 is reader page 3. The corner of that picture prints 35:

```
tanit-cli reader --text tests/reader/child-ebook.pdf --dst tests/reader/out-child --interpreter vision --pages 34-38 --prompt "Read this page aloud to a child. Keep the story gentle. Use the printed words, and skip anything that is not part of the story." --page 3 --no-speak
```

The next reader page:

```
tanit-cli reader --text tests/reader/child-ebook.pdf --dst tests/reader/out-child --interpreter vision --pages 34-38 --prompt "Read this page aloud to a child. Keep the story gentle. Use the printed words, and skip anything that is not part of the story." --next --no-speak
```

`--no-tts` still reads each page image and writes `spoken.md`. The voice proxy stays off. `--no-speak` does not call the vision model.

`--optimize` is an optional review after the harvest. It writes `revisions/<preset>/run-NNN/` and rebuilds `spoken.md`. `page-N.md` stays. The default preset is `reader-accessible-child`. `reader-vision-verify` reads each page image once. The other presets do not.

```
tanit-cli reader --text tests/reader/child-ebook.pdf --dst tests/reader/out-child --interpreter vision --pages 34-38 --prompt "Read this page aloud to a child. Keep the story gentle. Use the printed words, and skip anything that is not part of the story." --no-tts --optimize
```

```
tanit-cli reader --text tests/reader/child-ebook.pdf --dst tests/reader/out-child --interpreter vision --pages 34-38 --prompt "Read this page aloud to a child. Keep the story gentle. Use the printed words, and skip anything that is not part of the story." --no-tts
```

Every selected page joined into `spoken.md`:

```
tanit-cli reader --text tests/reader/child-ebook.pdf --dst tests/reader/out-child --interpreter vision --pages 34-38 --prompt "Read this page aloud to a child. Keep the story gentle. Use the printed words, and skip anything that is not part of the story." --all --no-speak
```

A page range without `vision` stops:

```
tanit-cli reader --text tests/reader/child-ebook.pdf --dst tests/reader/out-child --pages 34-38 --no-speak
```

Erase that job:

```
tanit-cli reader --text tests/reader/child-ebook.pdf --dst tests/reader/out-child --interpreter vision --pages 34-38 --prompt "Read this page aloud to a child. Keep the story gentle. Use the printed words, and skip anything that is not part of the story." --clean
```

A text file with no prompt is one page. anydoc is not called. The page is what is spoken:

```
tanit-cli reader --text tests/tts-orch/fixture.txt --dst tests/tts-orch/out --no-speak
```

```
 tanit-cli reader --text tests/reader/test.pdf --dst tests/reader/out --page 3 --voice-id EXAVITQu4vr4xnSDxMaL  --prompt "in german, skip parts that are unsuited for text to speech - explain as for a 8th grader"
```

An http(s) URL is cached by that URL. The path extension is the first guess. The start of the downloaded stream then decides: PDF and office bytes go through anydoc, an image stays an image, HTML is the DOM converted to markdown, and plain or markdown text is spoken as text.
```
 tanit-cli reader --text https://service.polymech.info/user/3bb4cfbf-318b-44d3-a9d3-35680e738421/pages/katbot-injection-machine.md --dst tests/reader/out-url --prompt "in french, skip parts that are unsuited for text to speech - explain as for a 8th grader"
```
npm run test:tts-orch
npm run test:reader


```

## References

https://elevenlabs.io/docs/overview/capabilities/text-to-speech/best-practices
https://github.com/k2-fsa/sherpa-onnx?utm_source=chatgpt.com
https://elevenlabs.io/app/create-voice
https://developers.openai.com/api/docs/guides/realtime-vad
https://github.com/firecrawl/anydoc/blob/main/src/formats/epub/mod.rs

