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

GitHub Actions on **tag push only** (not branch push). Tag `x.y.z` creates Draft Release.

### Build targets:
- macOS arm64 (Apple Silicon) — `.pkg`
- macOS x86_64 (Intel) — `.pkg` (uses Intel Homebrew OpenSSL at `/usr/local/opt/openssl@3`)
- Windows x64 — `.zip`
- Ubuntu x86_64 — `.deb`

### Release workflow:
```bash
git tag 0.2.0
git push origin 0.2.0
# → GitHub Actions builds all platforms → Draft Release created
# → Publish manually on GitHub Releases page (or: gh release edit 0.2.0 --draft=false)
```

### Key CI details:
- CMakeLists.txt: `OPENSSL_ROOT_DIR` not overridden if already set (for x86_64 cross-compile)
- `macos-ci-x86_64` preset sets `OPENSSL_ROOT_DIR=/usr/local/opt/openssl@3`
- build-macos script: `--arch` flag controls target architecture
- Intel Homebrew installed via `arch -x86_64` on Apple Silicon CI runners

## OBS Plugin UI Notes

### Button text toggle pattern
`obs_properties_add_button()` callback에서 버튼 텍스트 변경 시:
- `obs_property_set_description(property, "new text")` 사용 (콜백 내에서 직접)
- `return true` → `RefreshProperties()` 호출 → 변경된 description 반영
- `get_properties()`는 호출되지 않음 (`ReloadProperties()`만 호출함)

### Current version: 0.1.1

## Related Project

Sister project: SonioxCaptionPlugIn (same architecture, Soniox API)
Located at: ~/Documents/SonioxCaptionPlugIn
GitHub: github.com/inseokko/soniox-caption-obs
