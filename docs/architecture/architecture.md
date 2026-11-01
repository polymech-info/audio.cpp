# pm-audio architecture

`pm-audio-lib` is the C++20 audio library (`pm::audio`). It does not store settings and does not download models. Each diagram is an interactive HTML file next to the JSON it is built from.

| Diagram | What it shows |
|---------|----------------|
| [overview.html](overview.html) | Host, install hooks, and the six parts of the library |
| [session.html](session.html) | Lease, route, capture, echo, ring, playback, record |
| [voice.html](voice.html) | Voice pipeline and the host's function-pointer adapters |
| [speech.html](speech.html) | STT and TTS engines, and what stays idle without a parent |

## Overview

The host (Tanit, or the `pm-audio` CLI) links `pm-audio-lib` and installs the hooks the library cannot see: a log sink, the Whisper allow-gate, plugin tables, and the echo-cancellation backend. Inside the library, session, devices, the voice pipeline, speech engines, denoise, and record are separate. ElevenLabs is the one engine that always leaves the machine, over HTTPS, and only when `PM_AUDIO_SPEECH_CLIENTS` is on.

## Session

One lease per process. `preempt()` returns allow, deny, or stop-the-holder, and a capture or playback backend refuses to start without the current token. A saved route is a device descriptor (type, product, address), not a raw `aaudio:` id. Mic PCM can pass through a host-installed WebRTC echo canceller; with no backend the capture stays dry. The PCM ring is single-writer. The record buffer keeps samples in RAM, then spills, and a session file lets another process read status and request stop.

## Voice

`voice_session` stays on the host. It fills `VoiceAdapters`: capture, enhancement (DeepFilter or GTCRN), and transcription. `VoicePipeline` resamples an utterance to 16 kHz mono, enhances it, matches a wake word, then asks the transcription adapter to run. Trigger, transcript, and state come back through `RouteCallbacks`. Replay uses the same worker without a live mic.

## Speech

`speech_factory` stays on the host and turns flags and settings into option structs. Whisper.cpp and VibeVoice compile in when those targets already exist, or load through a plugin table. Windows TTS and Windows AI speech need C++/WinRT; otherwise they throw. MOSS Nano is a DLL loaded at runtime. The host gates (allow, cancel, ggml loader, VibeVoice table) are installed from outside the library.

## Left off these diagrams

Subtitles, the whistle detector, markdown-to-speech, and WAV helpers are small and don't change the shape above. Proxy STT and TTS (Tanit, OpenRouter, OpenAI) stay on the host and are not part of this package.
