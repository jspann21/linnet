"""Run native headphone-click waveform qualification; never opens a device.

Run: python -m tools.qualification.pcm_click
"""
import os
import subprocess
from tools.studio.build import ROOT, toolchain


def main():
    output = ROOT / 'out/qualification/pcm-click'
    output.mkdir(parents=True, exist_ok=True)
    executable = output / ('pcm-click.exe' if os.name == 'nt' else 'pcm-click')
    subprocess.run([toolchain()['compiler']['path'], '-std=c11', '-O2',
                    '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT), str(ROOT / 'tools/qualification/pcm_click.c'),
                    '-o', str(executable), '-lm'], check=True, cwd=ROOT)
    subprocess.run([str(executable)], check=True, cwd=ROOT)


if __name__ == '__main__':
    main()
