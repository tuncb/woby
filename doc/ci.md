# CI builds and releases

The `Build and Test` workflow runs on pushes to `main`, manual dispatch, and calls
from the release workflow. All three platforms use Ninja, vcpkg, and Release
builds, and run the application tests before uploading packages.

Windows builds use the GUI subsystem for the viewer. Launch tests check the PE
subsystem, an Explorer-style launch with no console, attachment to a hidden
parent console, redirected streams, and CLI output and exit codes.

## macOS packaging

The macOS archive contains `woby.app`, with the executable under
`Contents/MacOS` and assets under `Contents/Resources/assets`. CI installs the
`Runtime` component to stage the bundle. CMake's BundleUtilities copies required
non-system libraries into the bundle and rewrites their dependency paths, then
verifies that dependencies are self-contained. Nested binaries and the app are
ad-hoc signed after these changes; this is not Developer ID signing or notarization.

The macOS test suite builds a small bundle with an indirect shared-library
dependency, installs it, relocates it, makes the original build directory
unavailable, and checks assets, dependency resolution, signatures, and execution.
CI also runs the packaged Woby executable with `--version` and checks its font.
Interactive Finder launch and rendering should be checked on a Mac.

## Dependency caching

`cmake/ci-vcpkg-revision.txt` pins the vcpkg checkout. The CI configuration helper
copies the root `vcpkg.json` into `build/ci-vcpkg` and adds that same revision as
its `builtin-baseline`. This keeps the checkout and package baseline together
without changing the local development manifest or Debug preset.

CI uses the `woby-ci-*` triplets for both target dependencies and native host
tools. They build Release dependencies only. The Linux triplet inherits the
existing dynamic-linkage and SDL settings; Windows and macOS retain their
previous linkage choices.

The binary cache key includes the pinned revision, compiler identity (including
the Windows SDK), runner image, triplet contents, and dependency manifest. Woby's
own version fields are excluded, so a version bump alone keeps the same key.
Dependency changes get a new key and can restore compatible packages using a
prefix for the same toolchain. vcpkg still validates each package's ABI.

The workflow saves the cache immediately after successful CMake configuration,
before compiling and testing Woby. A later application build or test failure
therefore preserves the completed dependency binaries. A failed dependency
installation does not save an incomplete archive under the complete-cache key.

Changing the CI triplets or pin requires an initial cache fill. To update vcpkg,
put the desired full commit SHA in `cmake/ci-vcpkg-revision.txt`; the generated
baseline and cache identity update automatically. Confirm all three platform
jobs pass before tagging the update.

## Release package reuse

Pushing a `vMAJOR.MINOR.PATCH` tag starts the separate `Release` workflow. It
looks for a successful `Build and Test` run on `main` at the **exact tagged commit**,
and requires all three unexpired platform artifacts. It never substitutes a
build of a nearby commit or publishes packages from a failed build.

If the matching main build is queued or running, release waits for it. There is
a two-minute discovery window for a main push that arrives alongside the tag,
and a 45-minute wait limit for the build. Exceeding that limit fails the release
resolver instead of launching a competing build; rerun Release after the main
build finishes.

If no usable main build exists, or its artifacts have expired, Release invokes
the same build workflow for the tagged commit. Publication requires that build
to succeed. Package download and validation then happen before creating or
updating the GitHub release. A manual Release dispatch must target a version tag.

Every package contains `woby-update-helper` and `woby-manifest.json`. The manifest
generator verifies both binaries' versions and hashes the final staged files.
Release tags must match CMake and vcpkg versions; publication also checks each
archive's manifest version, platform, file list, and hashes. This validation applies
to reused main-build artifacts as well as newly built packages. See
[portable updates](updates.md) for installation and recovery behavior.

## Local validation

Run the CI helper tests with Node.js:

```powershell
node --test .github/scripts/ci-config.test.cjs .github/scripts/resolve-release-build.test.cjs
```

Run `actionlint` to check workflow syntax and expressions. Continue using
`cmake --build --preset vs2026-vcpkg` and `ctest --preset vs2026-vcpkg` for local
Debug validation. Runner-specific dependency builds and cross-run artifact
downloads are validated by GitHub Actions.
