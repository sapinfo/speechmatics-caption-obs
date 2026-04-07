# CLAUDE.md — Speechmatics Captions for OBS

## Project Overview

OBS Studio plugin for real-time speech-to-text captions using Speechmatics RT WebSocket API.
Single-file C++ plugin (`src/plugin-main.cpp`) with CMake build system.

## Build Commands

```bash
# macOS (local)
cmake --preset macos
cmake --build --preset macos

# Install to OBS
cp -R build_macos/RelWithDebInfo/speechmatics-caption-obs.plugin \
  ~/Library/Application\ Support/obs-studio/plugins/
```

## Key Architecture

- Single source file: `src/plugin-main.cpp` (~530 lines)
- WebSocket client: IXWebSocket (via CMake FetchContent)
- JSON parsing: nlohmann/json (via CMake FetchContent)
- Audio: OBS float32 48kHz → pcm_s16le 16kHz (3:1 downsample)

## Speechmatics RT API Protocol

1. Connect to `wss://eu2.rt.speechmatics.com/v2` with `Authorization: Bearer` header
2. Send `StartRecognition` JSON → wait for `RecognitionStarted`
3. Stream binary PCM audio → receive `AddPartialTranscript` / `AddTranscript`
4. Translation: `AddPartialTranslation` / `AddTranslation` (separate messages)
5. Stop: send `EndOfStream` with `last_seq_no` → receive `EndOfTranscript`

### Translation response format differs from transcription:
- Transcription: `results[].alternatives[].content`
- Translation: `results[].content` (no alternatives nesting)

## Language Codes

Standard ISO 639 except: Chinese = `cmn` (not `zh`)

## CI/CD

GitHub Actions on push to main and tags. Tag `x.y.z` creates Draft Release.
macOS builds arm64 only (Homebrew OpenSSL limitation on CI runners).

## Related Project

Sister project: SonioxCaptionPlugIn (same architecture, Soniox API)
Located at: ~/Documents/SonioxCaptionPlugIn
GitHub: github.com/sapinfo/soniox-caption-obs (or inseokko)
