# Contributing to Linnet

Start with the
[README](README.md) for setup, the [architecture](docs/architecture.md) for the source
map and [roadmap](docs/roadmap.md) for feature proposals and broader support.

## Choose a starting point

Unless otherwise stated, contributions are licensed under the project's
[GNU GPL version 3 or later](LICENSE). Preserve separate licenses and copyright
notices for third-party material.

- **New iPod model:** follow [Adding support for an iPod model](targets/README.md).
  A port needs native hardware support, a target manifest, display configuration and
  physical-device checks.
- **Apps or themes:** use the [SDK guide](sdk/README.md) and its templates.
- **Media or installation:** read [Media Manager](docs/media-manager.md),
  [storage compatibility](docs/storage.md) and [device workflows](docs/device-link.md).
- **Documentation:** update the guide that owns the topic and list new guides in
  the [documentation index](docs/README.md). Link new starting points from the
  project README when needed. Prefer links to repeating the same reference data.
  Keep [hardware support](docs/hardware.md) summaries brief and about the
  hardware configuration. Keep individual device serials, test dates, detailed
  test results and bring-up logs in local artifacts rather than documentation.

## Submit a pull request

Fork the repository, make a focused change on a branch and open a pull request
against `main`. Describe the change and the checks you ran using the PR template.
All contributions require review and approval from @jspann21 and a passing build.
New changes after approval require another review. Pull requests use squash merges.

Successful PR builds provide installer and firmware downloads for one day, so
testers can use the packages without compiling. See [Builds and downloads](docs/releases.md).

Use [Discussions](https://github.com/jspann21/linnet/discussions) for questions and
feature ideas, and the [issue forms](https://github.com/jspann21/linnet/issues/new/choose)
for reproducible bugs or hardware problems. Report security vulnerabilities
[privately](SECURITY.md).

## Check your changes

Run the relevant existing commands from the repository root. Setup and pinned
LLVM requirements are in the README. For firmware, run `python linnet doctor`
and build the affected target/profile with `--verify`. Development
builds require the observed device serial. For UI changes, compile content and
inspect native simulator output using the SDK workflow. For model additions,
use the fuller validation sequence in the porting guide.

Report exactly what was checked and what remains unverified. A passing compiler,
simulator or image check does not establish physical-device behavior. Changes
to boot, storage, power or installation need matching device and recovery checks.
Documentation-only changes should verify commands against CLI help and ensure
relative links resolve.

M25 OS compiler experiments can opt into `--link-optimization services-size`
or `--link-optimization services-thin-lto`; the default is `none`. Both use
`-Oz` for an explicit music database, library, browser, Search and Queue source
list. The latter also uses ThinLTO with bounded linker jobs. The build report
records the source list and compiler/linker flags in its identity. Hardware,
startup, codecs and generated presentation retain their existing compilation.
Compare final image size, memory sections, symbols and native timing before
using an experimental image; source tuning alone does not establish a speed
or battery improvement.

## Describe the contribution or issue

Explain the problem, the resulting behavior and any remaining limitations.
For device issues, include the target ID, hardware/storage configuration,
build profile and ID, reproduction steps, expected result and observed result.
Include a small relevant log excerpt with personal identifiers removed.

Keep contributions focused and update affected documentation. Keep generated
builds, simulator output, media, firmware backups and device-bound installation
plans local; see [local artifacts](docs/local-artifacts.md). Preserve third-party
license and provenance files when changing vendored code.
