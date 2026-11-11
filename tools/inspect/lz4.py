"""LZ4 block packing and independent unpacking for the native Boot archive."""
import struct
import sys


def pack_hc(data: bytes) -> bytes:
    """Compress a raw Boot block on any host and independently verify it."""
    try:
        from lz4.block import compress
    except ImportError as error:
        raise RuntimeError("Install Boot compression with: python -m pip install lz4==4.4.5") from error
    compressed = compress(data, mode="high_compression", compression=12, store_size=False)
    if unpack(compressed, len(data)) != data:
        raise ValueError('Boot compression readback mismatch')
    return compressed


def unpack(data: bytes, size: int) -> bytes:
    output=bytearray();cursor=0
    def length(value):
        nonlocal cursor
        if value==15:
            while True:
                if cursor>=len(data):raise ValueError('Truncated LZ4 length')
                extra=data[cursor];cursor+=1;value+=extra
                if value>size:raise ValueError('LZ4 length exceeds output')
                if extra!=255:break
        return value
    while cursor<len(data):
        token=data[cursor];cursor+=1;literals=length(token>>4)
        if cursor+literals>len(data) or len(output)+literals>size:raise ValueError('LZ4 literal bounds')
        output.extend(data[cursor:cursor+literals]);cursor+=literals
        if cursor==len(data):
            if len(output)!=size:raise ValueError('LZ4 output size mismatch')
            return bytes(output)
        if cursor+2>len(data):raise ValueError('LZ4 offset truncated')
        offset=struct.unpack_from('<H',data,cursor)[0];cursor+=2
        count=length(token&15)+4
        if not offset or offset>len(output) or len(output)+count>size:raise ValueError('LZ4 match bounds')
        for _ in range(count):output.append(output[-offset])
    raise ValueError('LZ4 final literal sequence missing')


if __name__ == '__main__':
    if sys.argv[1:] != ['--hc-stdin']:
        raise SystemExit('usage: python3 -m tools.inspect.lz4 --hc-stdin')
    sys.stdout.buffer.write(pack_hc(sys.stdin.buffer.read()))
