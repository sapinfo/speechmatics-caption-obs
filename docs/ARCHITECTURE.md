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

### 레이턴시 / 사일런스 세분화 파라미터 (v0.2.0+)

`StartRecognition.transcription_config`에 들어가는 사용자 노출 파라미터:

| 필드 | 타입 | 범위 | 기본값 | 설명 |
|------|------|------|--------|------|
| `max_delay` | float | 0.7 ~ 20.0 | 2.0 | 최종 자막 출력까지의 최대 대기 시간 (초) |
| `max_delay_mode` | string | `"flexible"` / `"fixed"` | `"flexible"` | 단어 경계에서 max_delay를 살짝 넘길지 여부 |
| `end_of_utterance_silence_trigger` | float | 0.0 ~ 2.0 | 0.0 (disabled) | 무음이 이 시간 이상 지속되면 EndOfUtterance 발화 마감. 0이면 메시지 자체를 보내지 않음 |

**구현 노트:**
- `eou_silence > 0.0f`일 때만 `transcription_config`에 필드를 추가 (Speechmatics는 0을 disabled로 해석하지만, 명시적 누락이 더 안전)
- 캡션 시작 시 시점에 락된 값이 람다로 캡처되어 WebSocket 핸들러로 전달됨 — 캡셔닝 중 변경 시 다음 Start에서 반영
- UI는 `obs_property_set_long_description()`으로 모든 속성에 시나리오별 권장값 포함된 툴팁 제공

## 빌드 프리셋

| 프리셋 | 용도 | 아키텍처 |
|--------|------|---------|
| `macos` | 로컬 빌드 | arm64 |
| `macos-ci` | GitHub Actions | arm64 |
| `macos-ci-x86_64` | GitHub Actions (Intel) | x86_64 |
| `windows-x64` | Windows | x64 |
| `ubuntu-x86_64` | Linux | x86_64 |

## CI/CD

- **트리거**: 태그 push만 (`x.y.z` 형식) → 빌드 + Draft Release (일반 push에는 빌드 안 함)
- **플랫폼**: macOS arm64 + x86_64 (macos-15), Windows (windows-2022), Ubuntu (ubuntu-24.04)
- **아티팩트**: `.pkg` (macOS arm64, x86_64), `.zip` (Windows), `.deb` (Ubuntu)

### macOS x86_64 크로스 컴파일
- Apple Silicon CI runner에서 Intel Homebrew (`/usr/local/bin/brew`)를 `arch -x86_64`로 설치
- OpenSSL x86_64: `/usr/local/opt/openssl@3`
- CMakeLists.txt: `OPENSSL_ROOT_DIR`가 이미 설정되어 있으면 `brew --prefix openssl`로 덮어쓰지 않음
- build-macos 스크립트: `--arch` 플래그로 아키텍처 선택
