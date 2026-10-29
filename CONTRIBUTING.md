# Contributing To Mobile Game Streamer Toolkit

Thanks for helping make Mobile Game Streamer Toolkit better. This project is in active public development toward `1.0.0`, so the most useful contributions are focused, reproducible, and easy to verify.

## Development Setup

Verify your Visual Studio 2022/2026 C++ toolchain and local SDK dependencies:

```powershell
.\scripts\configure-build.ps1 -Build -Configuration RelWithDebInfo
```

Run the CTest unit test suite before opening a pull request:

```powershell
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Validate PowerShell helper and packaging scripts:

```powershell
Get-ChildItem scripts\*.ps1, installer\*.ps1 | ForEach-Object {
    $err = $null; $tok = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($_.FullName, [ref]$tok, [ref]$err)
    if ($err) { throw $err }
}
```

Build the standalone Windows installer and portable ZIP archive (when full `vendor/` runtime bundles are staged locally):

```powershell
.\scripts\build-installer.ps1
```

## Contribution Workflow

1. Open or find an issue that describes the bug or enhancement.
2. Keep changes scoped to one topic (e.g., `scrcpy` transport, AirPlay H.264 decoding, iPhone USB reverse tethering, or Inno Setup packaging).
3. Preserve existing C++17 / OBS Studio module patterns in `src/` and PowerShell packaging structure in `scripts/` and `installer/`.
4. Add or update unit tests in `tests/` when command generation, packet framing, log filtering, or low-latency timing logic changes.
5. Update documentation in `README.md` and `docs/` when user-facing settings, driver behavior, or build steps change.
6. Open a pull request with a clear summary and verification notes.

## Driver & System Safety Boundaries

- **Stock OS behavior when idle**: Any USB filter or network sharing state (`libusb0`, ICS) must ONLY activate while the user explicitly enables USB Reverse Tethering in OBS, and must cleanly revert to 100% stock Apple/Windows drivers on stop, OBS exit, crash, or uninstall.
- **Read-only device capture**: Do not add unauthorized remote input injection, passcode/trust bypasses, or DRM circumvention.
- **Clean repository history**: Keep large SDK downloads, third-party binary bundles, and compiled installers inside the gitignored `vendor/`, `build/`, and `release/` directories.

## Pull Request Checklist

- [ ] The change has a clear user or maintainer benefit.
- [ ] `.\scripts\configure-build.ps1 -Build -Configuration RelWithDebInfo` succeeds without errors.
- [ ] `ctest --test-dir build -C RelWithDebInfo --output-on-failure` passes (`100% tests passed`).
- [ ] Docs are updated when behavior, properties, or setup steps change.
- [ ] No secrets, tokens, private device logs, or large binary blobs are committed.
- [ ] Stock Windows / Apple USB driver behavior remains completely untouched when OBS tethering is inactive.
