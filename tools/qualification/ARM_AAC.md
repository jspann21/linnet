# Linked ARM AAC experiment

This host-only qualification tool runs the AAC functions from a supplied
firmware ELF at their linked addresses. It uses Unicorn 2.1.4's TI925T ARMv4T
instruction model. It does not model the iPod's hardware or measure device
timing, power use, or end-to-end video startup.

Install the optional dependency in an experiment directory, then run from the
repository root in PowerShell:

```powershell
.venv/Scripts/python.exe -m pip install --target out/experiments/arm-aac-deps unicorn==2.1.4
$env:PYTHONPATH='out/experiments/arm-aac-deps;.'
$env:ARM_AAC_REPORT='path/to/verified-build-report.json'
$env:ARM_AAC_ADTS='path/to/extracted-aac-lc.aac'
$env:ARM_AAC_ABI_MANIFEST='out/object-cache-v1/.../matching-aac-object-manifest.json'
.venv/Scripts/python.exe -m tools.qualification.test_arm_aac
```

The ADTS stream must contain at least 66 access units. Extract it with stream
copy from the same media under investigation; do not substitute a re-encoded
copy for a device comparison. Generated streams can exercise additional codec
inputs. Existing Media Manager dependencies and media publication stay intact.
The ABI manifest must be the compiler-cache dependency manifest matching the
AAC object retained beside the verified ELF. Every retained header dependency,
including the private workspace and FAAD configuration, must match the current
definitions compiled by the ABI check. The tests emit the build, ELF, model, object,
header, manifest and ABI-check hashes. The ELF/object link relationship requires
the supplied build report and artifacts to have been independently verified.

The focused tests compare fresh decoding and uninterrupted decoding, then
check continuation at AU boundaries 1, 2, 16, 32, 33, 49 and 64. Intermediate AUs skip
synthesis; exactly one full discarded predecessor repairs overlap before PCM
is exposed. Invalid overlap must be rejected. A malformed AU must latch an
error until fresh initialization. The target compiler checks the structure
offsets used by the experiment. Execution has instruction/time limits, mapped
memory permissions, stack/decoder canaries and callee-saved register checks.
These are limited corruption checks, not a proof of memory safety or device
stack use. The emulator uses a separate 64 KiB host scratch stack.

These checks establish consistency within the host execution model. Changes to
prepared audio also need physical-device PCM parity, container lead/duration
trimming, source/build binding, handoff integrity, cache behavior and startup
measurements. Production seek preparation is owned by the
[AAC preparation tool](../content/aac_index.py); this experiment executes a
supplied firmware ELF and does not publish media.

The optional corpus tests also exercise the existing three-word recurrence
checkpoint followed by one discarded predecessor. They reject malformed
checkpoint words and check that fresh decoding remains usable. Passing these
samples does not establish generic checkpoint eligibility: retained dynamic
range extensions, for example, need state beyond those three words. A prepared
prefix must prove its own continuation or use ordinary entropy advancement.

API references: [Unicorn tutorial](https://www.unicorn-engine.org/docs/tutorial.html)
and [official ARM sample](https://github.com/unicorn-engine/unicorn/blob/master/samples/sample_arm.c).
