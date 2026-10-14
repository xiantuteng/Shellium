<div align="center">

<img src="resources/icons/shellium-app-icon.png" alt="Shellium" width="128">

# Shellium

**A cross-platform SSH terminal whose file manager follows you through every hop.**

Run `ssh` inside a session and the SFTP pane moves to the server you're actually on, routed through the correct jump hosts.

[![Release](https://img.shields.io/github/v/release/xiantuteng/Shellium?include_prereleases&sort=semver)](https://github.com/xiantuteng/Shellium/releases)
[![Build](https://github.com/xiantuteng/Shellium/actions/workflows/release.yml/badge.svg)](https://github.com/xiantuteng/Shellium/actions/workflows/release.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platform-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey)
![Qt](https://img.shields.io/badge/Qt-6.8%2B-41CD52?logo=qt&logoColor=white)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)

English · [简体中文](README.zh-CN.md) · [Website](https://shellium.net)

</div>

---

## Why Shellium?

Most SSH clients tie the file browser to the host you first connected to. Once you `ssh` from a bastion into an internal machine, the file browser is still showing the bastion.

Shellium keeps track of the whole chain of servers in each tab, from the outermost to the innermost.

```text
local ──ssh──▶ bastion ──ssh──▶ app-server ──ssh──▶ db-server
                                                       ▲
                       SFTP pane follows here, tunnelled through bastion → app-server
```

- When you run `ssh user@host` in the terminal, Shellium takes over the hop and opens it as its own session. Entering and leaving a hop are definite events, not guesses based on the window title.
- If the remote shell can't be integrated, Shellium falls back to parsing the `ssh` command. Hops found this way are marked as **inferred** in the UI and in risky operations.
- The file manager targets the innermost server by default. You can pin it to any outer hop. Inner SFTP connections always go through the right tunnel, so Shellium never connects directly to an internal address that happens to share a name.
- Each tab is fully isolated: its terminal, server chain, SFTP path, transfer queue and session credentials are not shared with other tabs.

## Features

**Terminal**
- libvterm-based VT/xterm emulation: 256 colours and true colour, alternate screen, wide (CJK) characters, configurable scrollback
- Local shell via PTY (macOS/Linux) or ConPTY (Windows), multiple tabs, broadcast input across a workspace
- Themes (system / light / dark), bundled monospace fonts, cursor, bell and scrolling settings
- One-key terminal reset (<kbd>⌘⇧R</kbd> / <kbd>Ctrl+Shift+R</kbd>) to recover after `cat`-ing a binary file

**SSH and connections**
- Host tree with nested groups, inherited settings, tags, filtering, recent connections and history
- Visual host chains (jump hosts) that reuse saved credentials, plus a manual ProxyJump option
- Local, remote and dynamic port forwarding (`-L` / `-R` / `-D`) with saved rules, auto-start and traffic stats
- Limited auto-reconnect with a fallback when the connection drops
- SHA-256 host key verification on first connection. Legacy algorithms such as `hmac-sha1` are enabled only after you confirm, and only for that host.
- Import from OpenSSH `config`; Shellium JSON import/export (credentials are never exported)

**Remote files (SFTP)**
- Local and remote panes side by side, jump-to-path, drag and drop in both directions
- Recursive transfers, resumable uploads/downloads, a queue you can pause, resume or cancel
- Text and image preview, `chmod`, rename and delete
- Right-click in the terminal to upload straight to the current remote directory

**Security**
- Optional master-password vault: Argon2id + XChaCha20-Poly1305 (libsodium), with idle auto-lock
- Ed25519 / RSA / ECDSA key generation, private key inspection and public key copy
- Private keys created with restrictive permissions (POSIX modes, protected ACLs on Windows)

**Productivity**
- Snippets, command history with completion, multi-host execution
- Quick palettes in the terminal: <kbd>⌘K</kbd> for snippets, <kbd>⌘J</kbd> for history
- Command palette; save, validate and restore workspaces
- AI assistant (optional, bring your own OpenAI-compatible endpoint). Every command that changes the system requires your explicit confirmation.

**Localisation**: 14 UI languages: English, 简体中文, 繁體中文, Français, Deutsch, Italiano, Español, Português (Brasil), Română, Русский, Українська, 日本語, 한국어 and العربية.

<details>
<summary><b>Keyboard shortcuts</b> (macOS; use <kbd>Ctrl</kbd> in place of <kbd>⌘</kbd> on Windows/Linux)</summary>

| Shortcut | Action |
| --- | --- |
| <kbd>⌘K</kbd> | Run a snippet in the current terminal |
| <kbd>⌘J</kbd> | Run a command from history (<kbd>⌘1</kbd>–<kbd>⌘9</kbd> picks an entry) |
| <kbd>⇧⌘F</kbd> | Search hosts in the sidebar. <kbd>Enter</kbd> connects, <kbd>⇧Enter</kbd> connects through the host chain |
| <kbd>⇧⌘O</kbd> | Show or hide the remote file manager |
| <kbd>⇧⌘R</kbd> | Reset the terminal locally (no remote command is sent) |

</details>

## Installation

Download the latest build from **[Releases](https://github.com/xiantuteng/Shellium/releases)**.

| Platform | Architectures | Package | Minimum OS |
| --- | --- | --- | --- |
| macOS | Apple Silicon, Intel | `.dmg` | macOS 26 (arm64), macOS 15 (x86_64) |
| Windows | x64, ARM64 | NSIS installer, `.zip` | Windows 10 1809 / Windows 11 |
| Linux | x86_64, aarch64 | AppImage | glibc 2.39 (e.g. Ubuntu 24.04+) |

> [!NOTE]
> Release builds are not yet code-signed. CI builds for macOS are ad-hoc signed, so Gatekeeper will block the first launch: right-click the app and choose **Open**, or run `xattr -dr com.apple.quarantine /Applications/Shellium.app`. On Windows, SmartScreen may ask for confirmation. Each release ships with a `SHA256SUMS.txt` you can check the download against.

## Building from source

### Requirements

- CMake 3.21+, Ninja, a C++20 compiler
- Qt 6.8+ (Core, Widgets, Concurrent, Network, Sql, Svg, LinguistTools; Test for the test suite)
- libssh 0.11+, libvterm 0.3+, libsodium 1.0.18+, spdlog 1.12+, minizip

<details>
<summary><b>macOS</b> (Homebrew)</summary>

```bash
brew install qtbase qtsvg qttools libssh libvterm libsodium spdlog minizip ninja pkg-config
```

</details>

<details>
<summary><b>Linux</b> (Ubuntu 24.04+)</summary>

Ubuntu's own packages ship Qt 6.4 and libssh 0.10, both older than Shellium needs. Install Qt 6.8+ with the [Qt online installer](https://www.qt.io/download-qt-installer-oss) or [aqtinstall](https://github.com/miurahr/aqtinstall), then:

```bash
sudo apt-get install ninja-build pkg-config libssl-dev libvterm-dev libsodium-dev \
    libspdlog-dev libminizip-dev zlib1g-dev libgl1-mesa-dev libxkbcommon-x11-0 libxcb-cursor0
sudo ./scripts/install_libssh_from_source.sh /usr/local   # libssh 0.11
```

</details>

<details>
<summary><b>Windows</b> (MSVC 2022 + vcpkg)</summary>

Release builds use MSVC 2022, the official Qt MSVC binaries and the vcpkg manifest in [`packaging/windows/vcpkg`](packaging/windows/vcpkg). See the [Windows build guide](packaging/windows/README.md).

</details>

### Build and test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On a headless machine, run the GUI tests with `QT_QPA_PLATFORM=offscreen`. Don't run them with `ctest -j`, because they share a display.

The macOS app bundle ends up at `build/Shellium.app`.

`ShelliumSshIntegrationTests` connects to a real test server configured through `SHELLIUM_IT_*` environment variables, and is skipped when they are not set. It deliberately triggers failed logins, so never point it at a production server (it can trip `MaxStartups` or fail2ban).

### Packaging

| Target | Script |
| --- | --- |
| macOS DMG (Developer ID signing + notarisation) | [`scripts/package_macos_dmg.sh`](scripts/package_macos_dmg.sh), see [the guide](docs/MACOS_DMG_RELEASE.md) |
| Windows NSIS / ZIP | [`scripts/package_windows_native.ps1`](scripts/package_windows_native.ps1), see [the guide](packaging/windows/README.md) |
| Linux AppImage | [`scripts/package_linux_appimage.sh`](scripts/package_linux_appimage.sh) |

The [release workflow](.github/workflows/release.yml) builds all six packages on native runners and smoke-tests each one at launch. Pushing a `v*` tag that matches `project(VERSION)` in `CMakeLists.txt` creates a **draft** release with `SHA256SUMS.txt`.

## Roadmap

Not implemented yet:

- Terminal mouse reporting, block selection, clickable URLs, automatic session logging
- Recursive directory deletion over SFTP
- Connecting to SSH through an external SOCKS5/HTTP proxy (this is separate from `-D` dynamic forwarding, which is already supported)
- Importing from Termius / PuTTY / MobaXterm, system keychain integration, cloud sync

macOS arm64 is the primary development platform. The other platforms are built and smoke-tested in CI but have had less hands-on testing, so [bug reports](https://github.com/xiantuteng/Shellium/issues) are especially welcome.

## Contributing

Issues and pull requests are welcome. Please read [`AGENTS.md`](AGENTS.md) first. It covers the rules every change has to follow:

- **Tests are mandatory.** Every feature and bug fix comes with an automated test. For a bug fix, temporarily revert the fix and confirm the test fails without it.
- **Translations stay in sync.** If you change any user-visible string (`tr()` / `QT_TRANSLATE_NOOP`), update all 13 translations in the same change. See [`docs/I18N_DESIGN.md`](docs/I18N_DESIGN.md).
- **Third-party licences.** Any new bundled dependency must be added to [`resources/licenses/`](resources/licenses/README.md). LGPL components must stay dynamically linked.

Before touching the session stack, libssh, or terminal rendering, read [`docs/ENGINEERING_NOTES.md`](docs/ENGINEERING_NOTES.md). It records the architectural constraints and the bugs that only reproduce at the second nesting level or deeper.

## Acknowledgements

Shellium is built on [Qt](https://www.qt.io/), [libssh](https://www.libssh.org/), [libvterm](https://www.leonerd.org.uk/code/libvterm/), [libsodium](https://libsodium.org/) and [spdlog](https://github.com/gabime/spdlog), and bundles a number of open-source monospace fonts. The full list of third-party components and their licences is under **Settings → About → View licenses** and in [`resources/licenses/`](resources/licenses).

## License

Copyright © 2026 Shellium contributors.

Shellium is licensed under the [Apache License, Version 2.0](LICENSE). You may use, modify, and redistribute it, including commercially, under the terms of that licence. It is provided "AS IS", without warranties or conditions of any kind.

Third-party components bundled with Shellium keep their own licences, listed in [`resources/licenses/`](resources/licenses).
