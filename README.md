# Speechmatics Captions for OBS

OBS Studio 플러그인 — Speechmatics RT API를 사용한 실시간 음성 자막

## 기능

- 실시간 음성 인식 (STT) 자막
- 다국어 지원 (한국어, 영어, 일본어, 중국어, 스페인어, 프랑스어, 독일어)
- 실시간 번역 (Speechmatics Translation)
- OBS 핫키로 자막 시작/중지 토글
- 폰트 및 크기 커스터마이징

## 요구사항

- OBS Studio 31.x+
- Speechmatics API Key ([speechmatics.com](https://www.speechmatics.com/)에서 발급)
- macOS 12.0+ / Windows 10+ / Ubuntu 22.04+

## 빌드 (macOS)

```bash
cmake --preset macos
cmake --build --preset macos
```

빌드 결과물은 `build_macos/` 디렉토리에 생성됩니다.

## 설치

빌드된 `.so` (Linux) / `.dylib` (macOS) / `.dll` (Windows) 파일을 OBS 플러그인 디렉토리에 복사:

- **macOS**: `~/Library/Application Support/obs-studio/plugins/`
- **Windows**: `%APPDATA%/obs-studio/plugins/`
- **Linux**: `~/.config/obs-studio/plugins/`

## 사용법

1. OBS에서 소스 추가 → **Speechmatics Captions** 선택
2. 속성에서 API Key 입력
3. Audio Source 선택
4. **Test Connection**으로 연결 확인
5. **Start Caption**으로 자막 시작

## 라이선스

GPL-2.0 License
