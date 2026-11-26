import struct
import sys
from pathlib import Path
out = ['#pragma once', '#include <cstdint>']
for name, path in zip(('items_vert', 'items_frag'), sys.argv[1:3]):
    data = Path(path).read_bytes()
    if len(data) % 4 or len(data) < 20 or data[:4] != b'\x03\x02\x23\x07':
        raise ValueError('Invalid SPIR-V')
    words = struct.unpack('<' + 'I' * (len(data)//4), data)
    out.append('static const uint32_t '+name+'[] = {'+', '.join(hex(w) for w in words)+'};')
Path(sys.argv[3]).write_text('\n'.join(out)+'\n')
