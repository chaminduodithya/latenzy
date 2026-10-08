# Latenzy

A tiny, modern Windows app that watches your internet connection. Latenzy sends a lightweight HTTP `HEAD` request to a URL of your choice at a regular interval and shows latency, success rate and history in a clean dark UI. It lives in the system tray, and the tray icon changes colour with your connection status.

## Features

- Live status pill: Online / Offline / No network / Paused
- Current and average latency, packets sent, success rate
- Latency history graph (last 60 pings)
- Colour-coded activity log
- Tray icon that turns green, red or amber, with the latency in the tooltip
- Single self-contained exe, no runtime or DLLs needed
- Per-monitor high-DPI support

## Install

Download `Latenzy-Setup-x.y.z.exe` from the [Releases](../../releases) page and run it. Windows 10 or 11 (64-bit) is recommended.

You can also grab the standalone `latenzy.exe`. Put a `latenzy.ini` next to it to run in portable mode.

## Configuration

Set the URL and interval in the app and press **Apply**. Settings are stored in `%APPDATA%\Latenzy\latenzy.ini`, or next to the exe if a `latenzy.ini` is already there.

```ini
target=https://www.google.com
interval=10
```

Launch with `--tray` to start hidden in the system tray.

## Build from source

Requires mingw-w64 GCC 10+ (for example [WinLibs](https://winlibs.com) or MSYS2 UCRT64).

```
build.bat
```

This produces `build\latenzy.exe`. To build the installer, install [Inno Setup](https://jrsoftware.org/isinfo.php) and run:

```
iscc installer\latenzy_setup.iss
```

## Releases

Pushing a tag such as `v1.0.0` triggers the GitHub Actions workflow, which builds the exe and installer and attaches them to a GitHub Release.

## License

MIT, see [LICENSE](LICENSE).
