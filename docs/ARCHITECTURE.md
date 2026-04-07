# Architecture — Speechmatics Captions for OBS

## 개요

OBS Studio 플러그인으로, Speechmatics RT WebSocket API를 통해 실시간 음성 인식 및 번역 자막을 제공합니다.

## 시스템 구성

```
┌──────────────┐     ┌──────────────────┐     ┌─────────────────────┐
│  OBS Studio  │     │  Plugin          │     │  Speechmatics RT    │
│              │     │  (plugin-main)   │     │  API Server         │
│  Audio Src ──┼──►  │  48kHz→16kHz ────┼──►  │  wss://eu2.rt...    │
│              │     │  float→pcm_s16le │     │                     │
│  Text Src  ◄─┼──── │  ◄── JSON parse  │◄──  │  AddTranscript      │
│  (자막표시)  │     │                  │     │  AddTranslation     │
└──────────────┘     └──────────────────┘     └─────────────────────┘
```

## 핵심 흐름

### 1. 연결 (StartRecognition)

```
Client                          Server
  │                               │
  │──── WebSocket Connect ────►   │  (Authorization: Bearer <key>)
  │◄─── HTTP 101 Upgrade ─────   │
  │                               │
  │──── StartRecognition ─────►   │  (audio_format, transcription_config, translation_config)
  │◄─── RecognitionStarted ────   │
  │                               │
  │──── Binary PCM audio ─────►   │  (반복)
  │◄─── AudioAdded (seq_no) ───   │
  │◄─── AddPartialTranscript ──   │
  │◄─── AddTranscript ─────────   │
  │◄─── AddPartialTranslation ─   │  (번역 활성화 시)
  │◄─── AddTranslation ────────   │
  │                               │
  │──── EndOfStream ───────────►  │  (last_seq_no)
  │◄─── EndOfTranscript ───────   │
```

### 2. 오디오 변환

- **입력**: OBS 오디오 (float32, 48000Hz, mono)
- **변환**: 3:1 다운샘플링 → pcm_s16le, 16000Hz
- **전송**: IXWebSocket binary frame

### 3. 자막 표시

- `AddPartialTranscript`: 중간 결과 (계속 덮어쓰기됨)
- `AddTranscript`: 확정 결과 (final_text에 누적)
- `AddTranslation` / `AddPartialTranslation`: 번역 (원문 아래에 줄바꿈으로 표시)
- 텍스트 길이 > 200자 시 앞부분 자동 제거

## 파일 구조

```
src/
  plugin-main.cpp       # 플러그인 전체 로직 (단일 파일)
  plugin-support.h      # OBS 플러그인 매크로/로깅
  plugin-support.c.in   # 플러그인 이름/버전 (CMake 생성)
```

## 의존성

| 라이브러리 | 버전 | 용도 |
|-----------|------|------|
| IXWebSocket | v11.4.5 | WebSocket 클라이언트 (TLS/OpenSSL) |
| nlohmann/json | v3.11.3 | JSON 파싱 |
| OBS Studio SDK | 31.1.1 | 플러그인 API |

모두 CMake FetchContent로 자동 다운로드.

## Speechmatics RT API 특이사항

### 인증
- HTTP 헤더: `Authorization: Bearer <api_key>`
- Soniox와 달리 JSON 메시지에 키를 포함하지 않음

### 번역 응답 구조 차이
- **Transcription**: `results[].alternatives[].content`
- **Translation**: `results[].content` (직접 포함, alternatives 없음)

### 언어 코드
- 중국어: `cmn` (ISO 639-3, Soniox의 `zh`와 다름)
- 한국어: `ko` (동일)

## 빌드 프리셋

| 프리셋 | 용도 | 아키텍처 |
|--------|------|---------|
| `macos` | 로컬 빌드 | arm64 |
| `macos-ci` | GitHub Actions | arm64 |
| `windows-x64` | Windows | x64 |
| `ubuntu-x86_64` | Linux | x86_64 |

## CI/CD

- **트리거**: main push → 빌드, 태그 push → 빌드 + Draft Release
- **플랫폼**: macOS (macos-15), Windows (windows-2022), Ubuntu (ubuntu-24.04)
- **아티팩트**: `.tar.xz` (macOS), `.zip` (Windows), `.deb` (Ubuntu)
