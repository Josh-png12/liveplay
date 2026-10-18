# LivePlay+ Phase 1.9 — Operator Reliability & Windows Release

## Runtime profiles

The desktop launcher owns the selected profile in Electron `userData/liveplay-server.json`.

- **Local Mode** is the default. It binds the bundled server to `127.0.0.1`, limits filesystem APIs to the configured Allowed Media Folders (or the user's Music and Documents folders), and restricts CORS to the local client origin.
- **Event LAN Mode** is explicit. It binds to `0.0.0.0` only while Remote Control is enabled, keeps the same filesystem roots, and starts the server with `--require-auth`. The client refuses the switch before an administrator exists; the server independently refuses startup if login or an admin is missing.
- Turning Remote Control off returns the listener to loopback after restart.

The server's direct-launch default is also loopback. Existing `--bind`, `--cors-origin`, and repeatable `--fs-root` options remain supported.

## Machine data and logs

Packaged launches set `LIVEPLAY_MACHINE_DIR` and `LIVEPLAY_LOG_DIR` to writable directories below Electron `userData`. Accounts, output mappings, preferences, configuration, launcher output, session logs, and crash logs therefore do not depend on Program Files being writable. Passwords remain in the existing libsodium-backed user store; diagnostics never include tokens or passwords.

Use **Settings → Server → Open Logs Folder** to open the directory. **Copy Diagnostics** copies a safe runtime summary.

## Show Board persistence

Event state is an optional `settings.eventShowBoard` object in the existing project document. It stores ordered teams, country, grade, entry/exit cue IDs, selected team, board settings, and the Speech Mode duck amount. Existing `.liveplay` files omit the object and load unchanged. Event Loudness and Video Output continue in their established optional settings objects.

## Outputs

`Main Out` and `Preview Out` remain built-in logical outputs in the existing output map. Mapping Preview rewires only affected buses. The Hardware Test can send stereo, Left, or Right tones through Main or the existing Preview bus; an absent Preview device yields a recoverable 409 while Main remains active.

## Windows package

`npm run build:electron` produces `client/dist-electron/LivePlay-Setup-2.5.0.exe`. The package includes the generated renderer, Electron runtime, server executable and DLLs, worker executables, locales/resources, and the npm-provided FFmpeg/FFprobe binaries. Runtime lookup remains bundled binary, environment override, then PATH. Binary licensing/provenance is documented in `FFMPEG_DISTRIBUTION.md`.

The normal launcher starts the server hidden, waits for `/api/health`, writes output to persistent logs, and stops a server child it owns on normal exit. Diagnostic Console is opt-in.
