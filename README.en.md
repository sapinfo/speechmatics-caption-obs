# Speechmatics Captions for OBS

**English** | [한국어](README.md)

**Real-time speech-to-text captions + translation** for OBS Studio using the [Speechmatics](https://www.speechmatics.com/) RT API.
Speak in any language and see both the original captions and translated subtitles displayed simultaneously.

---

## What is Speechmatics?

[Speechmatics](https://www.speechmatics.com/) is a leading real-time speech-to-text (STT) API provider.

- **50+ languages supported**: Korean, English, Japanese, Chinese, Spanish, French, German, and more
- **High accuracy**: Enhanced model delivers best-in-class recognition
- **Real-time translation**: Simultaneous multi-language translation
- **Flexible configuration**: Adjustable latency vs. accuracy tradeoff (`max_delay`)

## Features

- **Real-time speech-to-text** (Speechmatics RT Enhanced model)
- **Real-time translation** — translated subtitles appear alongside original captions instantly (KO↔EN, KO↔JA, KO↔ZH, and more — 7 languages)
- Hotkey support (start/stop without opening Properties)
- Auto-reconnect on disconnect
- CJK font support
- Configurable font size

## Free Plan

Speechmatics offers a generous free tier — **no credit card required**.

| Feature | Free Allowance |
|---------|---------------|
| Real-time STT | **8 hours (480 min) per month** |
| Concurrent sessions | Up to 2 |
| Languages | 55+ |
| Real-time translation | Supported |

> The free plan is more than enough for personal streaming or small broadcasts.

## Quick Start

### 1. Get Speechmatics API Key

Sign up at [speechmatics.com](https://www.speechmatics.com/) and get your API key. (No credit card required)

### 2. Download

[**Download Latest Release**](../../releases/latest)

| Platform | File |
|----------|------|
| macOS (Apple Silicon) | `speechmatics-caption-obs-0.1.0-macos-arm64.tar.xz` |
| macOS (Intel) | Build from source (see below) |
| Windows | `speechmatics-caption-obs-0.1.0-windows-x64.zip` |
| Linux (Ubuntu) | `speechmatics-caption-obs-0.1.0-x86_64-linux-gnu.deb` |

> **Intel Mac users**: The Apple Silicon binary runs on Intel Macs via Rosetta 2. For a native build, see the Build from Source section below.

### 3. Install

<details>
<summary><b>macOS</b></summary>

1. Download and unzip `speechmatics-caption-obs-x.x.x-macos-arm64.zip`
2. In OBS, go to **File** → **Show Settings Folder**
3. Open the **plugins** folder
4. Copy `speechmatics-caption-obs.plugin` into the **plugins** folder
5. Restart OBS Studio

**Uninstall:** In OBS, go to **File** → **Show Settings Folder** → **plugins** folder → delete `speechmatics-caption-obs.plugin`
</details>

<details>
<summary><b>Windows</b></summary>

1. Download and unzip `speechmatics-caption-obs-x.x.x-windows-x64.zip`
2. Copy contents to:
   ```
   %APPDATA%\obs-studio\plugins\speechmatics-caption-obs\
   ```
3. Restart OBS Studio
</details>

<details>
<summary><b>Linux (Ubuntu)</b></summary>

```bash
sudo dpkg -i speechmatics-caption-obs-x.x.x-x86_64-linux-gnu.deb
```

Or manually copy to `~/.config/obs-studio/plugins/speechmatics-caption-obs/`
</details>

### 4. Usage

1. In OBS, click **+** in Sources → select **Speechmatics Captions**
2. Right-click the source → **Properties**:
   - Enter your **Speechmatics API Key**
   - Select **Audio Source** (e.g., Mic/Aux)
   - Choose **Language**
   - (Optional) Check **Enable Translation** and select target language
3. Click **Start Caption**
4. Speak into your microphone — captions appear in real-time!

### Hotkey

Assign a hotkey in **OBS Settings → Hotkeys → Toggle Speechmatics Captions** to start/stop without opening Properties.

---

## Build from Source

<details>
<summary>Expand</summary>

### Prerequisites

- CMake 3.28+
- Xcode 16+ (macOS) / Visual Studio 2022 (Windows) / GCC 12+ (Linux)
- OpenSSL (macOS: `brew install openssl`)

### macOS

```bash
cmake --preset macos
cmake --build --preset macos
# Output: build_macos/RelWithDebInfo/speechmatics-caption-obs.plugin
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

All dependencies (IXWebSocket, nlohmann/json, OBS SDK) are automatically downloaded via CMake FetchContent.

</details>

## Support

If you find this project helpful, buy me a coffee!

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-FFDD00?style=for-the-badge&logo=buy-me-a-coffee&logoColor=black)](https://buymeacoffee.com/inseokko)

## License

GPL-2.0 - see [LICENSE](LICENSE)
