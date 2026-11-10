# Hear Eleven v4 through Tanit

`tanit-cli` is on PATH. These play on the speakers. The Tanit key is the one
in App Settings. The model is `tanit-speech-v4`, which the proxy sends to
ElevenLabs as `eleven_v4`.

Premade voices (the list on `tanit-speech-turbo` / `tanit-speech-v4`):

| Voice | Id |
| --- | --- |
| Sarah | `EXAVITQu4vr4xnSDxMaL` |
| Alice | `Xb7hH8MSUJpSbSDYk0k2` |
| Matilda | `XrExE9yKIg1WjnnlVkGX` |
| Daniel | `onwK4e9ZLuTAKqWW03F9` |
| Brian | `nPczCjzI2devNBz1zQrb` |
| Adam | `pNInz6obpgDQGcFmaJgB` |
| George | `JBFqnCBsd6RMkjVDRZzb` |
| Jessica | `cgSgspJ2msm6clMCkdW9` |
| River | `SAz9YHcvj6GT2YYXdXww` |
| Liam | `TX3LPaxmHKxFdv7VOQHJ` |

Sarah is the proxy default. The CLI's own default id,
`tLK6fPv15M0oKv4V3ACR`, is not one of these. Pass `--voice-id`.

## One sentence, Sarah

```
tanit-cli audio tts --provider tanit --model-id tanit-speech-v4 --voice-id EXAVITQu4vr4xnSDxMaL --text "Our goal is to make the application easier to use while keeping its advanced capabilities."
```

## Same line, saved, still played

```
tanit-cli audio tts --provider tanit --model-id tanit-speech-v4 --voice-id EXAVITQu4vr4xnSDxMaL --text "Our goal is to make the application easier to use while keeping its advanced capabilities." --dst sarah-v4.mp3
```

## Alice, then George

```
tanit-cli audio tts --provider tanit --model-id tanit-speech-v4 --voice-id Xb7hH8MSUJpSbSDYk0k2 --text "This is Alice, reading the same sentence so the voice change is obvious."
```

```
tanit-cli audio tts --provider tanit --model-id tanit-speech-v4 --voice-id JBFqnCBsd6RMkjVDRZzb --text "This is George, reading the same sentence so the voice change is obvious."
```

## Hear the chunk join

Two sentences, split after the first. Tanit speaks each one as its own
`tanit-speech-v4` request and passes the neighbour as `previous_text` or
`next_text`. Sarah reads both in one file, and the speakers play it.

```
tanit-cli audio tts --provider tanit --model-id tanit-speech-v4 --voice-id EXAVITQu4vr4xnSDxMaL --chunk-target-chars 100 --chunk-max-chars 200 --dst tests/tts-next/out/chunk-join.mp3 --text "The first chunk is Sarah speaking, and this sentence keeps going so the chunker has to close it before the next one starts. The second chunk begins now, and the same voice should carry across that join without a new breath or a new pace."
```

The harness writes the same take, plus the two chunk texts, as `chunk-join`.

```
npm run test:tts-next:live -- --only chunk-join
```

## The harness slices

Plan mode only checks the CLI and writes the text. It does not speak.

```
npm run test:tts-next
```

Live mode asks the proxy for the three slices (short cursor, 1,200 character
cursor, 3,500 character document) and writes `tests/tts-next/out/*.mp3` next
to the `.txt` slices. It does not play them. Open those files to listen.

```
npm run test:tts-next:live
```
