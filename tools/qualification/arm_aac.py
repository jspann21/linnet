"""Experimental, host-only execution of linked firmware AAC instructions.

Requires optional unicorn==2.1.4. This is a qualification tool, not a media
publication format or a device playback path. TI925T supplies an ARMv4T ISA
model; this does not claim ARM7 hardware or timing equivalence.
"""
import hashlib
import json
from pathlib import Path
import struct


def adts_packets(data):
    """Read bounded, unprotected, single-block AAC-LC ADTS without decoding."""
    packets = []
    config = None
    at = 0
    while at < len(data):
        h = data[at:at + 7]
        if len(h) != 7 or h[0] != 255 or h[1] & 0xf6 != 0xf0 or not h[1] & 1:
            raise ValueError('Unsupported ADTS header')
        profile, index = h[2] >> 6, (h[2] >> 2) & 15
        channels = ((h[2] & 1) << 2) | (h[3] >> 6)
        size = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5)
        if profile != 1 or index >= 13 or channels not in (1, 2) or h[6] & 3 or size <= 7 or at + size > len(data):
            raise ValueError('Unsupported or truncated AAC-LC frame')
        asc = bytes([(2 << 3) | (index >> 1), ((index & 1) << 7) | (channels << 3)])
        if config is not None and asc != config:
            raise ValueError('AAC configuration changed')
        config = asc
        packets.append(data[at + 7:at + size])
        at += size
    if not packets:
        raise ValueError('Empty AAC stream')
    return config, packets


def elf32(data):
    h = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    if h[0][:7] != b'\x7fELF\x01\x01\x01' or h[1:4] != (2, 40, 1) or h[9] != 32 or h[11] != 40:
        raise ValueError('Expected linked little-endian ARM ELF32')
    segments = []
    for i in range(h[10]):
        kind, offset, address, _, size, memory, flags, _ = struct.unpack_from('<8I', data, h[5] + i * 32)
        if kind != 1:
            continue
        if size > memory or offset + size > len(data) or address + memory > 2**32 or memory > 64 * 1024 * 1024:
            raise ValueError('Invalid ELF load segment')
        segments.append((address, memory, flags, data[offset:offset + size]))
    sections = [struct.unpack_from('<10I', data, h[6] + i * 40) for i in range(h[12])]
    symbols = {}
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        table = data[strings[4]:strings[4] + strings[5]]
        if section[9] != 16:
            raise ValueError('Invalid ELF symbol table')
        for at in range(section[4], section[4] + section[5], 16):
            name, value, size, info, _, index = struct.unpack_from('<IIIBBH', data, at)
            if index and info & 15 == 2:
                end = table.find(b'\0', name)
                symbols[table[name:end].decode('ascii')] = (value, size)
    return segments, symbols


class ArmAAC:
    DECODER = 0x10001000
    INPUT = 0x10020000
    STACK_TOP = 0x10040000
    RETURN = 0x10050000
    PCM = 106496
    PACKET = 98304
    PCM_CURSOR = 110648
    DECODER_BYTES = 110672

    def __init__(self, report, asc, *, abi_manifest):
        import unicorn as uc
        from unicorn import arm_const as arm
        if uc.__version__ != '2.1.4':
            raise ValueError('Qualification requires unicorn==2.1.4')
        self.uc, self.arm = uc, arm
        report = json.loads(Path(report).read_text())
        image = Path(report['artifacts']['elf']).read_bytes()
        abi_path = Path(abi_manifest)
        abi_bytes = abi_path.read_bytes()
        abi = json.loads(abi_bytes)
        object_path = Path(report['artifacts']['elf']).parent / 'services_audio_aac_aac.c.o'
        object_digest = hashlib.sha256(object_path.read_bytes()).hexdigest()
        header_digest = hashlib.sha256(Path('services/audio/aac/aac.h').read_bytes()).hexdigest()
        if (abi.get('object_sha256') != object_digest or
                abi.get('dependencies', {}).get('source/services/audio/aac/aac.h') != header_digest or
                hashlib.sha256(json.dumps(abi, sort_keys=True).encode()).hexdigest() != abi_path.stem):
            raise ValueError('Verified target object/ABI dependency binding required')
        # Private workspace offsets also depend on FAAD/configuration headers.
        # A supplied older ELF must not be inspected using newer definitions.
        root = Path(__file__).resolve().parents[2]
        abi_headers = 0
        for name, digest in abi['dependencies'].items():
            if not name.startswith('source/') or not name.endswith('.h'):
                continue
            dependency = (root / name[7:]).resolve()
            if not dependency.is_relative_to(root) or not dependency.is_file() or hashlib.sha256(dependency.read_bytes()).hexdigest() != digest:
                raise ValueError('Target ABI header dependency changed: ' + name)
            abi_headers += 1
        self.binding = {'build_id': report['build_id'], 'elf_sha256': hashlib.sha256(image).hexdigest(),
                        'emulator': uc.__version__, 'isa_model': 'TI925T ARMv4T',
                        'aac_object_sha256': object_digest, 'abi_header_sha256': header_digest,
                        'abi_manifest_sha256': hashlib.sha256(abi_bytes).hexdigest(),
                        'abi_check_sha256': hashlib.sha256(Path('tools/qualification/arm_aac_abi.c').read_bytes()).hexdigest(),
                        'abi_headers_verified': abi_headers}
        segments, self.symbols = elf32(image)
        self.machine = m = uc.Uc(uc.UC_ARCH_ARM, uc.UC_MODE_ARM | uc.UC_MODE_LITTLE_ENDIAN)
        m.ctl_set_cpu_model(arm.UC_CPU_ARM_TI925T)
        pages = {}
        for address, memory, flags, _ in segments:
            permissions = (uc.UC_PROT_READ if flags & 4 else 0) | (uc.UC_PROT_WRITE if flags & 2 else 0) | (uc.UC_PROT_EXEC if flags & 1 else 0)
            for page in range(address & ~4095, (address + memory + 4095) & ~4095, 4096):
                pages[page] = pages.get(page, 0) | permissions
        ordered = sorted(pages)
        at = 0
        while at < len(ordered):
            first = ordered[at]
            permissions = pages[first]
            end = at + 1
            while end < len(ordered) and ordered[end] == ordered[end - 1] + 4096 and pages[ordered[end]] == permissions:
                end += 1
            m.mem_map(first, ordered[end - 1] + 4096 - first, permissions)
            at = end
        for address, _, _, payload in segments:
            if payload:
                m.mem_write(address, payload)
        m.mem_map(self.DECODER, 0x1c000, uc.UC_PROT_READ | uc.UC_PROT_WRITE)
        m.mem_map(self.INPUT, 0x2000, uc.UC_PROT_READ | uc.UC_PROT_WRITE)
        m.mem_map(0x10030000, 0x10000, uc.UC_PROT_READ | uc.UC_PROT_WRITE)
        m.mem_map(self.RETURN, 4096, uc.UC_PROT_READ | uc.UC_PROT_EXEC)
        m.reg_write(arm.UC_ARM_REG_CPSR, 0x13)
        for reg in range(arm.UC_ARM_REG_R0, arm.UC_ARM_REG_R12 + 1):
            m.reg_write(reg, 0)
        if not 2 <= len(asc) <= 64:
            raise ValueError('ASC bounds')
        m.mem_write(self.INPUT, asc)
        if self.call('ln_aac_raw_init', self.DECODER, self.INPUT, len(asc)) != 1:
            raise ValueError('AAC initialization rejected')

    def call(self, name, *args):
        m, arm = self.machine, self.arm
        canary = b'\xa5' * 32
        m.mem_write(self.DECODER + self.DECODER_BYTES, canary)
        m.mem_write(0x10030000, canary)
        saved = [m.reg_read(arm.UC_ARM_REG_R4 + n) for n in range(8)]
        for n, value in enumerate(args):
            m.reg_write(arm.UC_ARM_REG_R0 + n, value)
        m.reg_write(arm.UC_ARM_REG_SP, self.STACK_TOP)
        m.reg_write(arm.UC_ARM_REG_LR, self.RETURN)
        m.emu_start(self.symbols[name][0], self.RETURN, timeout=5_000_000, count=20_000_000)
        if m.reg_read(arm.UC_ARM_REG_PC) != self.RETURN or m.reg_read(arm.UC_ARM_REG_SP) != self.STACK_TOP:
            raise RuntimeError('AAC instruction/time/stack return bound failed')
        if saved != [m.reg_read(arm.UC_ARM_REG_R4 + n) for n in range(8)] or bytes(m.mem_read(self.DECODER + self.DECODER_BYTES, 32)) != canary or bytes(m.mem_read(0x10030000, 32)) != canary:
            raise RuntimeError('AAC ABI or memory canary failed')
        value = m.reg_read(arm.UC_ARM_REG_R0)
        return value if value < 2**31 else value - 2**32

    def decode(self, packet, mode='decode'):
        names = {'decode': 'ln_aac_raw_decode', 'discard': 'ln_aac_raw_discard', 'skip': 'ln_aac_raw_skip_synthesis'}
        if not 0 < len(packet) <= 8191 or mode not in names:
            raise ValueError('AAC packet bounds or mode')
        packet_address = self.DECODER + self.PACKET
        self.machine.mem_write(packet_address, packet)
        result = self.call(names[mode], self.DECODER, packet_address, len(packet))
        if result != 1:
            raise ValueError(f'AAC {mode} rejected: {result}')
        if mode != 'decode':
            return None
        pcm = bytes(self.machine.mem_read(self.DECODER + self.PCM, 4096))
        self.machine.mem_write(self.DECODER + self.PCM_CURSOR, struct.pack('<I', 1024))
        return pcm
