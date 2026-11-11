"""Compile and run cache ownership qualification.

Uses the project's pinned host compiler and actual storage sources. Generated
headers and executables stay in out; no hardware or media writes are performed.
Run: python -m tools.qualification.cache_lease
"""
from pathlib import Path
import os
import subprocess
from tools.studio.build import ROOT, toolchain


def main():
    output = ROOT / 'out/qualification/cache-lease'
    output.mkdir(parents=True, exist_ok=True)
    (output / 'linnet_target.h').write_text(
        '#define LINNET_EXTENDED_RAM 1\n#define LINNET_IMAGE_OS 1\n'
        '#define LINNET_STORAGE_MAX_READ_SECTORS 192u\n', encoding='utf-8')
    compiler = toolchain()['compiler']['path']
    for profile in ('development', 'release'):
        executable = output / ('cache-lease-' + profile + ('.exe' if os.name == 'nt' else ''))
        command = [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(output), '-I' + str(ROOT)]
        if profile == 'release':
            command.append('-DLINNET_PROFILE_RELEASE=1')
        command += [str(ROOT / name) for name in ('tools/qualification/cache_lease.c',
                    'services/storage/resident.c', 'services/storage/fat32.c')]
        command += ['-o', str(executable)]
        subprocess.run(command, check=True, cwd=ROOT)
        subprocess.run([str(executable)], check=True, cwd=ROOT)


if __name__ == '__main__':
    main()
