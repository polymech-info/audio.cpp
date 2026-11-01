# pm-audio follow-ups

Moved: session lease, route, events, PCM graph, self-test, subtitle cues, voice types, match, envelope, the PCM ring, ElevenLabs STT and TTS, voice change, the voice-over timeline, the sound trigger (pffft), the file filter, the offline STT helpers, speech-text preparation (markdown via md4c), the MOSS Nano DLL loader, AAudio (desktop stub, real stream on Android), and the voice pipeline worker (VAD, utterance, replay, DeepFilter and GTCRN enhancers). VibeVoice's in-process engine compiles when Tanit's `vibevoice` target already exists and the plugin DLL is off.

Whisper.cpp and ONNX are pointed at Tanit. The package does not FetchContent either one. A standalone build reads `third_party/onnxruntime-*` and an optional `PM_AUDIO_WHISPER_ROOT`.

Host headers under `src/audio` that already include `pm/audio/...` stay as forwards. Call sites keep those includes. New moves use the same shape: one implementation in this library, a one-line host header, and a host installer for anything that still touches settings, Group Policy, or a plugin manager.

`MINIAUDIO_IMPLEMENTATION` stays a single translation unit: `src/core/audio_devices.cpp` on the host, `src/miniaudio_impl.cpp` on the package executable. The static library calls `ma_*` and does not define them.

## Done in this cut

Whisper inference, the VibeVoice plugin table, echo cancellation, WinRT speech, and device capture/playback moved. Host headers are forwards. `speech_factory.cpp` still turns CLI flags and the settings provider map into the package option structs.

1. Whisper: `set_whisper_gate`, `set_whisper_cancel`, `set_ggml_backend_load`, and `set_whisper_resolve` are installed from `audio_filter_host.cpp`. The `whisper_full` loop compiles when `TARGET whisper` exists and `PM_AUDIO_WHISPER_PLUGIN` is off. The plugin build fills `WhisperBackend` from `PmWhisperApi` on each call.
2. VibeVoice: `set_vibevoice_backend` is installed from `audio_tts_host.cpp` when `PM_PLUGIN_VIBEVOICE` is on. Otherwise the engine calls `vv_capi_*`.
3. Echo cancellation: `EchoCancellationProcessor` calls `set_apm_backend`. `audio_apm_host.cpp` fills it from the WebRTC plugin or the in-process APM. No backend means `create()` returns null.
4. WinRT speech compiles when the host finds cppwinrt and sets `PM_AUDIO_HAS_WINRT` on `pm-audio-lib`. The standalone build keeps the stub that throws. `k_proxy_default_voice` lives next to `k_elevenlabs_voice_id`. Package identity for Windows AI is `set_windows_ai_packaged`, installed from `audio_filter_host.cpp` via `pm::appdata::is_packaged`.
5. `AudioInput`, `DesktopAudioCapture`, `AudioOutput`, the file players, and device enumeration live in this library. `src/core/audio.hpp` and `src/core/audio_devices.hpp` forward here. `MINIAUDIO_IMPLEMENTATION` stays in `src/core/audio_devices.cpp`. `AudioInput` calls `set_device_prepare`, which the host points at `install_host_log`.
6. The PCM record buffer, the Windows AAC writer, and the record session file live here. `set_record_session_dir` and `set_record_cli_exe` are installed from `audio_record_host.cpp`. The `audio record` command and the xblox block stay on the host.

## Stays on the host

- `voice_session.cpp` and `voice_adapters.cpp`: settings, command control, llama embeddings, and `MixedAudioCapture`. The adapter is the function-pointer bridge into the moved pipeline.
- `notifications.cpp`: UI sounds and the settings store. It calls the moved `play_sound_file_*`.
- `speech_factory.cpp`: `resolve_speech_provider`, `make_elevenlabs_tts`, `whisper_options_from_request`.
- `audio_log.cpp` plus the host installers (`audio_filter_host.cpp`, `audio_tts_host.cpp`, `audio_apm_host.cpp`). `src/core/audio_devices.cpp` keeps the one miniaudio implementation.
- `proxy_stt.cpp` and `proxy_tts.cpp`, parked for a later package (Tanit, OpenRouter, and OpenAI speech). Do not fold those into this library.
- `src/core/mixed_audio_capture.cpp`: camera plus mic mix. It consumes the moved `AudioInput` and the capture-config types that already live here.
