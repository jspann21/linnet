# Local outputs and recovery artifacts

`out/` is ignored scratch space for builds, simulator frames, and reports.
Each build writes to
`out/<profile>/<target>/<kind>/runs/<invocation>/`; use its report to find the
matching images and generated inputs.

Compilation uses two workers by default; `--jobs 1` through `--jobs 4` changes
the limit. Unchanged objects can be reused from `out/object-cache-v1/`.
`LINNET_OBJECT_CACHE=0` disables that cache. Object reuse does not replace
per-build identity and verification.

## Before cleaning

```console
python -m tools.studio.artifact_inventory > out/artifact-inventory.json
```

This read-only inventory lists recognized builds and recovery references.
It neither deletes files nor proves that unclassified files are disposable.
Review scan errors before relying on it.

Keep firmware backups, installation plans, predecessor files, and recovery
receipts together in a separate local backup. They belong to the installation
that produced them and may depend on absolute paths and pinned hashes.
Relocation may require restoring those dependencies; do not edit hashes to make
a different artifact fit a plan.
