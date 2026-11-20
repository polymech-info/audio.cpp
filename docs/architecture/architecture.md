# pm-audio architecture

`pm-audio-lib` is the C++20 audio library (`pm::audio`). It does not store settings and does not download models. Desktop Tanit, the `pm-audio` CLI, and the Android app all link that same library. `packages/server.cpp` does not implement a second audio stack. On the desktop listener, `node_audio.cpp` mounts `SourceHost` and `SinkHost`.

Each diagram is an interactive HTML file next to the JSON it is built from.

| Diagram | What it shows |
|---------|----------------|
| [overview.html](overview.html) | Desktop host, Android host, `node_audio`, and the library |
| [session.html](session.html) | Per-process lease, and the desktop `/v1/audio` pipe |
| [voice.html](voice.html) | Desktop voice pipeline, and the phone path beside it |
| [speech.html](speech.html) | STT and TTS engines, and what stays idle without a parent |

## Hosts

Desktop Tanit and the `pm-audio` CLI link `pm-audio-lib`. `voice_session` and `speech_factory` stay on that host. One miniaudio implementation lives in `src/core/audio_devices.cpp`.

Android links the same library with `PM_AUDIO_SPEECH_CLIENTS` off and `PM_AUDIO_SOUND_TRIGGER` on. File filters and DeepFilterNet stay out. GTCRN live is compiled in against the Android ONNX Runtime. `jni_pm_voice.cpp` and `jni_sound_trigger.cpp` call the library. Kotlin keeps the speech recognizer, Android TTS, the media session, and the LAN pipe. `miniaudio_impl.cpp` is the one miniaudio implementation inside `pm_image_jni`.

`node_audio.cpp` is compiled into the desktop listener only. The phone image compiles the vfs listener, the paired-peer listener, and the pair file. It does not compile these audio routes. Authorization is the node gate.

Live voice PCM stays on the phone. The pipe is a separate HTTP path: `GET /v1/audio/stream` and `POST /v1/audio/play`.

`pm/audio/clock_trial.hpp` is a header for speech and the phone mic. Pass-through and speakers do not use it. Nothing in it runs on an audio callback. The desktop is the master when a caller opts in. One estimate does not step a playing device.

## Overview

Inside the library, session, devices, the voice pipeline, and the speech engines stay separate. `SourceHost` and `SinkHost` are the objects `node_audio` mounts. Denoise, the record buffer, and the ElevenLabs client are still in the library. The C++ ElevenLabs client is linked only when `PM_AUDIO_SPEECH_CLIENTS` is on, so the APK does not include it. Phone cloud speech uses the Kotlin client.

## Session

One lease per process. `preempt()` returns allow, deny, or stop-the-holder, and a capture or playback backend refuses to start without the current token. The phone mic takes that lease inside the phone process. The desktop capture is a different process. A saved route is a device descriptor (type, product, address), not a raw `aaudio:` id. Mic PCM can pass through a host-installed WebRTC echo canceller; with no backend the capture stays dry. The PCM ring is single-writer. The record buffer keeps samples in RAM, then spills, and a session file lets another process read status and request stop.

The phone does not hold the desktop lease. It is an HTTP client of the routes `node_audio` mounts.

## Voice

`voice_session` stays on the desktop host. It fills `VoiceAdapters`: capture, enhancement (DeepFilter or GTCRN), and transcription. `VoicePipeline` resamples an utterance to 16 kHz mono, enhances it, matches a wake word, then asks the transcription adapter to run. Trigger, transcript, and state come back through `RouteCallbacks`. Replay uses the same worker without a live mic.

Android does not install `VoiceAdapters`. JNI calls the lease, AAudio, and the recorder. Kotlin keeps the recognizer, Android TTS, and the media session.

## Speech

`speech_factory` stays on the desktop host and turns flags and settings into option structs. Whisper.cpp and VibeVoice compile in when those targets already exist, or load through a plugin table. Windows TTS and Windows AI speech need C++/WinRT; otherwise they throw. MOSS Nano is a DLL loaded at runtime. The host gates (allow, cancel, ggml loader, VibeVoice table) are installed from outside the library. The Android build leaves the C++ speech clients out.

## Left off these diagrams

Subtitles, the whistle detector, markdown-to-speech, and WAV helpers are small and don't change the shape above. Proxy STT and TTS (Tanit, OpenRouter, OpenAI) stay on the host and are not part of this package. The clock trial is a header, not a stage in the pipe or the voice pipeline.
