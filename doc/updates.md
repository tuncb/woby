# Portable release updates

`woby update` installs the latest stable release from `tuncb/woby`. It selects
Windows x64, Linux x64, or macOS ARM64 based on the executable's compiled platform.
It updates the directory containing the resolved executable, regardless of the
terminal's current directory. It never installs an equal or older version.

```powershell
woby update --check
woby update
woby update --status
woby update --status --json
```

`--check` only queries GitHub. `--status` reads local state without using the
network. They are mutually exclusive. With no option, `update` downloads and
installs the newer release. No GitHub account, token, Git, Python, or external
archive tool is required on the user's machine.

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | Check succeeded, no newer release exists, or status confirms completion. |
| 1 | Validation, download, installation, or recovery failed. |
| 2 | Installation was handed to the helper, or status is pending/applying. |

`--json` emits a JSON object on stdout. Check results include `current`, `latest`,
`deployment`, and `updateAvailable`. Installation returns `state: "pending"`;
this is not a completion receipt. Poll `update --status --json` until its `state`
is `completed` or `failed`. A `recovery-required` state needs the recovery helper.
The original CLI cannot wait while the helper replaces its executable and loaded
libraries on Windows. The helper runs without opening an extra console window.

## Installation requirements

### Updating from Settings

Open **Settings**, then choose **Check for updates**. The dialog shows the installed
version, latest release, and any connection or validation error. Checks run in the
background and can also be used from development builds.

For a managed portable release with a newer version available, choose **Install
update and restart**. Save scene changes first; the button is disabled while the
scene is dirty. Close other viewers using the same deployment. The dialog stays
open while Woby downloads and verifies the package. Errors leave the app open;
check again to retry. Once the helper is ready, Woby closes and installation
finishes in the background. After verification, the helper releases the deployment
lock and reopens Woby. A failed installation or manual recovery does not reopen it.
If restarting fails, the installation remains completed and `woby update --status`
reports that Woby must be opened manually. Command-line installs do not restart
Woby, and there is no automatic update check.

### Deployment requirements

Install the first updater-enabled release manually, including its executable,
helper, libraries, assets, and `woby-manifest.json`. Build outputs intentionally
have no manifest and refuse installation, although `--check` works there.
Releases published before this feature cannot bootstrap themselves.

Self-update is intended for writable per-user portable deployments, not shared
system installations or installations managed by another package manager. It does
not request elevation. Close all viewers using the deployment before updating.
Per-user locks distinguish deployments by directory identity and allow multiple
viewers while excluding updates. Viewers from other deployments remain usable.
Read-only deployments can still be viewed; coordination locks live in user storage.

Files listed in the installed manifest must still match their recorded hashes.
If a shipped sample, library, or asset was edited, move that edited copy to a user
location and restore the original package file before updating. Unknown files
are preserved; an update that would overwrite an unknown file fails instead.
Settings stored in the user's preference directory and importer registrations
are untouched. Deleted package files are removed, while empty directories may remain.

## Validation and recovery

Downloads use HTTPS with certificate verification, bounded redirects, timeouts,
and size limits. GitHub's asset SHA-256 is required and checked before extraction.
The extractor rejects traversal, absolute paths, links, special files, duplicate
paths, unlisted files, and oversized packages. Manifest versions, platform,
required files, and per-file hashes are checked before installation.

Each attempt stages files beneath `.woby-update/job-ID/` in the deployment.
The existing helper and its libraries are copied into that job's `helper/`
directory so they remain usable while package files change. The helper waits
for its parent to exit, obtains the exclusive lock, revalidates both packages,
and records a journal. It backs up all affected old files before changing any
deployment files. It then replaces managed files, removes obsolete ones, and
runs the installed executable's `--version`. Failure triggers rollback.

This is a recoverable sequence of file operations, not an atomic replacement
of the entire directory. If the helper is interrupted, viewers refuse to start
until recovery finishes. Run `update --status` for the current job and helper
paths. If the main executable cannot start because installation was interrupted,
read `.woby-update/status.json` and launch the helper from that job directly:

```powershell
& 'C:\Tools\woby\.woby-update\job-ID\helper\woby-update-helper.exe' --recover 'C:\Tools\woby' 'C:\Tools\woby\.woby-update\job-ID'
```

Replace the example paths with the actual installation and full job ID. On Linux
and macOS, the helper is named `woby-update-helper`. Recovery checks the job belongs
to this deployment, restores backups, and preserves user files. It can be retried
after addressing a locked file or permission problem. A completed transaction
cannot be rolled back through an old helper.

Backups, downloaded archives, and helper files are retained in their job folders.
After an update has completed and the helper has exited, old job folders can be
removed to reclaim space. Keep the current job if recovery is required.

## Packaging and validation

CI generates the manifest only after staging and fixing runtime library paths.
The generator runs both binaries' `--version` and requires them to match CMake
and vcpkg. It then hashes the package files. Publication validates the release
tag against that version and verifies all three archives against their manifests,
including reused main-build artifacts. CI continues to use Ninja and vcpkg.

Local Debug validation uses:

```powershell
cmake --preset vs2026-vcpkg
cmake --build --preset vs2026-vcpkg
ctest --preset vs2026-vcpkg
```

CTest includes package-policy unit tests, ZIP/tar.gz validation, lock and rollback
tests, packaging-script tests, and an offline integration test using the built
helper. The integration test covers parent handoff, installation, a failed binary
version check, interrupted-update recovery, and paths with spaces and Unicode.
Headless viewer fixtures verify that UI installs restart only after completion,
with the deployment lock available, while CLI installs and failed updates stay closed.
Python is required for development/CI tests and package generation only.
