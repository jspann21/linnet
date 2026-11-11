"""Qualify real Video/Classic sink sequencing with mocked MMIO/I2C/DMA.

Run: python -m tools.qualification.audio_sink
Pass --baseline COMMIT to demonstrate the checks against an earlier HAL.
No device is opened and no firmware is installed.
"""
import argparse,json,subprocess
from pathlib import Path
from tools.studio.build import ROOT,toolchain


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline')
    args=parser.parse_args()
    output=ROOT/'out/qualification/audio-sink'
    output.mkdir(parents=True,exist_ok=True)
    compiler=toolchain()['compiler']['path']
    for family,codec,target in [('pp502x','wm8758','video5g-m25'),('s5l8702','cs42l55','classic6g-n25')]:
        file=f'hal/{family}/audio.c'
        source=subprocess.check_output(['git','show',f'{args.baseline}:{file}'],cwd=ROOT,text=True) if args.baseline else (ROOT/file).read_text()
        prefix='#include <stdint.h>\n#include <stdbool.h>\n'
        if args.baseline:prefix+='#define AUDIT_BASELINE 1\n'
        if family=='pp502x':
            prefix+='#define AUDIT_PP 1\n'
            source=source.replace('#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))','#define REG(a) (*audit_register(a))')
            source=source.replace('return (volatile uint32_t *)((uintptr_t)p|0x10000000u);','return p;')
            source=source.replace('REG(0x6000b010u)=(uint32_t)(uintptr_t)source;', 'audit_samples=(const uint32_t *)source;REG(0x6000b010u)=(uint32_t)(uintptr_t)source;')
        src=output/f'{codec}.c'
        src.write_text(prefix+'#include "tools/qualification/audio_sink_mocks.inc"\n'+source+'\n#include "tools/qualification/audio_sink_checks.inc"\n')
        headers=sorted((ROOT/f'out/dev/{target}/os/runs').glob('*/generated/linnet_target.h'),key=lambda p:p.stat().st_mtime)
        if not headers:raise RuntimeError(f'Build {target} once to generate target headers')
        codec_source=ROOT/f'hal/codecs/{codec}.c'
        if args.baseline:
            codec_source=output/f'{codec}-baseline.c'
            codec_source.write_text(subprocess.check_output(['git','show',f'{args.baseline}:hal/codecs/{codec}.c'],cwd=ROOT,text=True))
        exe=output/f'{codec}.exe'
        subprocess.run([compiler,'-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(ROOT),'-I'+str(headers[-1].parent),f'-I{ROOT}/hal/{family}',f'-I{ROOT}/hal/codecs','-DLINNET_PROFILE_RELEASE=1','-DLINNET_IMAGE_OS=1',str(src),str(codec_source),'-o',str(exe)],cwd=ROOT,check=True)
        result=subprocess.run([str(exe)],cwd=ROOT,text=True,capture_output=True)
        print(codec,result.stdout,result.stderr,flush=True)
        (output/f'{codec}-{args.baseline or "current"}.json').write_text(json.dumps(dict(returncode=result.returncode,stdout=result.stdout,stderr=result.stderr),indent=2))
        if result.returncode and not args.baseline:raise RuntimeError(f'{codec}: qualification failed')
        if args.baseline and not result.returncode:raise RuntimeError(f'{codec}: negative control unexpectedly passed')


if __name__=='__main__':main()
