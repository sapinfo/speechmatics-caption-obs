# Speechmatics Captions for OBS

[English](README.en.md) | **한국어**

Speechmatics RT API를 사용한 OBS Studio **실시간 자막 + 번역** 플러그인입니다.
한국어로 말하면 자막이 표시되고, 동시에 영어(또는 다른 언어)로 번역된 자막도 함께 표시됩니다.

---

## Speechmatics란?

[Speechmatics](https://www.speechmatics.com/)는 업계를 대표하는 실시간 음성 인식(STT) API입니다.

- **50개 이상 언어 지원**: 한국어, 영어, 일본어, 중국어, 스페인어, 프랑스어, 독일어 등
- **높은 정확도**: Enhanced 모델로 최고 수준의 인식률 제공
- **실시간 번역**: 다국어 동시 번역 지원
- **유연한 설정**: 지연시간 vs 정확도 트레이드오프 조절 가능 (`max_delay`)

## 주요 기능

- **실시간 음성→텍스트** (Speechmatics RT Enhanced 모델)
- **실시간 번역** — 말하는 즉시 번역된 자막이 함께 표시 (한↔영, 한↔일, 한↔중 등 7개 언어)
- 단축키 지원 (Properties 열지 않고 시작/중지)
- 네트워크 끊김 시 자동 재연결
- 한중일(CJK) 폰트 지원
- 폰트 크기 조절 가능

## 무료 플랜

Speechmatics는 **신용카드 등록 없이** 무료로 시작할 수 있습니다.

| 항목 | 무료 제공량 |
|------|------------|
| 실시간 음성 인식 (STT) | **매월 8시간 (480분)** |
| 동시 세션 | 최대 2개 |
| 지원 언어 | 55개 이상 |
| 실시간 번역 | 지원 |

> 개인 방송이나 소규모 스트리밍에는 무료 플랜으로 충분합니다.

## 빠른 시작

### 1. Speechmatics API 키 발급

[speechmatics.com](https://www.speechmatics.com/)에서 가입 후 API 키를 발급받으세요. (신용카드 불필요)

### 2. 다운로드

[**최신 Release 다운로드**](../../releases/latest)

| 플랫폼 | 파일 |
|--------|------|
| macOS (Apple Silicon) | `speechmatics-caption-obs-x.x.x-macos-arm64.zip` |
| Windows | `speechmatics-caption-obs-x.x.x-windows-x64.zip` |
| Linux (Ubuntu) | `speechmatics-caption-obs-x.x.x-x86_64-linux-gnu.deb` |

### 3. 설치

<details>
<summary><b>macOS</b></summary>

1. `speechmatics-caption-obs-x.x.x-macos-arm64.zip` 다운로드 후 압축 해제
2. OBS 메뉴 → **File** → **Show Settings Folder** 클릭
3. 열린 폴더에서 **plugins** 폴더로 이동
4. `speechmatics-caption-obs.plugin` 을 **plugins** 폴더에 복사
5. OBS Studio 재시작

**제거:** OBS 메뉴 → **File** → **Show Settings Folder** → **plugins** 폴더에서 `speechmatics-caption-obs.plugin` 삭제
</details>

<details>
<summary><b>Windows</b></summary>

1. `speechmatics-caption-obs-x.x.x-windows-x64.zip` 다운로드 후 압축 해제
2. 내용물을 아래 경로로 복사:
   ```
   %APPDATA%\obs-studio\plugins\speechmatics-caption-obs\
   ```
3. OBS Studio 재시작
</details>

<details>
<summary><b>Linux (Ubuntu)</b></summary>

```bash
sudo dpkg -i speechmatics-caption-obs-x.x.x-x86_64-linux-gnu.deb
```

또는 수동으로 `~/.config/obs-studio/plugins/speechmatics-caption-obs/` 에 복사
</details>

### 4. 사용법

1. OBS에서 소스 **+** 클릭 → **Speechmatics Captions** 선택
2. 소스 우클릭 → **속성**:
   - **Speechmatics API Key** 입력
   - **Audio Source** 에서 마이크 선택 (예: Mic/Aux)
   - **Language** 선택
   - (선택) **Enable Translation** 체크 후 번역 대상 언어 선택
3. **Start Caption** 클릭
4. 마이크에 말하면 실시간 자막이 화면에 표시됩니다!

### 단축키

**OBS 설정 → 단축키 → Toggle Speechmatics Captions** 에서 단축키를 지정하면 Properties를 열지 않고도 시작/중지할 수 있습니다.

---

## 소스 빌드

<details>
<summary>펼치기</summary>

### 사전 요구사항

- CMake 3.28+
- Xcode 16+ (macOS) / Visual Studio 2022 (Windows) / GCC 12+ (Linux)
- OpenSSL (macOS: `brew install openssl`)

### macOS

```bash
cmake --preset macos
cmake --build --preset macos
# 결과물: build_macos/RelWithDebInfo/speechmatics-caption-obs.plugin
```

### Windows

```bash
cmake --preset windows-x64
cmake --build --preset windows-x64
```

### Linux

```bash
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

모든 의존성(IXWebSocket, nlohmann/json, OBS SDK)은 CMake FetchContent를 통해 자동 다운로드됩니다.

</details>

## 후원

이 프로젝트가 도움이 되셨다면 커피 한 잔 사주세요!

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-FFDD00?style=for-the-badge&logo=buy-me-a-coffee&logoColor=black)](https://buymeacoffee.com/inseokko)

## 라이선스

GPL-2.0 - [LICENSE](LICENSE) 참조
