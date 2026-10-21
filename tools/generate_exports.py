"""Generate x64 MASM tail-forwarders from a local PE export table. Stdlib only."""
import argparse
import json
import os
from pathlib import Path
import struct


def pe_exports(path):
    data = Path(path).read_bytes()
    def u16(offset): return struct.unpack_from('<H', data, offset)[0]
    def u32(offset): return struct.unpack_from('<I', data, offset)[0]
    if data[:2] != b'MZ': raise ValueError('Not a PE file')
    pe = u32(0x3c)
    if data[pe:pe+4] != b'PE\0\0' or u16(pe+4) != 0x8664:
        raise ValueError('Only Windows x64 PE files are supported')
    optional = pe + 24
    if u16(optional) != 0x20b: raise ValueError('Expected PE32+')
    sections = optional + u16(pe+20)
    def offset(rva):
        if rva < u32(optional+60): return rva
        for i in range(u16(pe+6)):
            p = sections + i*40
            size, address, raw_size, raw = struct.unpack_from('<IIII', data, p+8)
            if address <= rva < address + max(size, raw_size):
                if rva-address >= raw_size: raise ValueError('RVA has no file data')
                return raw+rva-address
        raise ValueError(f'Invalid RVA {rva:x}')
    rva = u32(optional+112)
    if not rva: return []
    table = offset(rva)
    base, functions, names, function_rva, name_rva, ordinal_rva = struct.unpack_from('<IIIIII', data, table+16)
    if functions > 100000 or names > functions: raise ValueError('Invalid export count')
    named = {}
    for i in range(names):
        start = offset(u32(offset(name_rva)+i*4))
        name = data[start:data.index(b'\0', start)].decode('ascii')
        index = u16(offset(ordinal_rva)+i*2)
        if index >= functions: raise ValueError('Invalid export ordinal')
        named[index] = name
    result = []
    for i in range(functions):
        if not u32(offset(function_rva)+i*4): continue
        if i not in named: raise ValueError('Ordinal-only exports need an explicit forwarding implementation')
        result.append({'name': named[i], 'ordinal': base+i})
    return result


def generate(exports, output):
    output.mkdir(parents=True, exist_ok=True)
    names = [e['name'] for e in exports]
    include = 'static const char* const exportNames[]={' + ','.join(json.dumps(n) for n in names) + '};\n'
    include += f'static const unsigned exportCount={len(names)};\n'
    include += f'static const unsigned indexCreateDevice={names.index("D3D11CreateDevice")};\n'
    include += f'static const unsigned indexCreateSwap={names.index("D3D11CreateDeviceAndSwapChain")};\n'
    (output/'exports.inc').write_text(include, encoding='utf8')
    asm = ['option casemap:none', 'EXTERN ResolveRequired:PROC', '.code']
    defs = ['LIBRARY d3d11', 'EXPORTS']
    for i, entry in enumerate(exports):
        name = entry['name']
        symbol = {'D3D11CreateDevice':'ProxyCreateDevice', 'D3D11CreateDeviceAndSwapChain':'ProxyCreateDeviceAndSwapChain'}.get(name, f'Forward{i}')
        defs.append(f' {name}={symbol} @{entry["ordinal"]}')
        if symbol.startswith('Proxy'): continue
        asm += [f'{symbol} PROC FRAME', ' sub rsp, 88h', ' .allocstack 88h', ' .endprolog']
        for reg, pos in [('rcx','20h'),('rdx','28h'),('r8','30h'),('r9','38h')]:
            asm.append(f' mov [rsp+{pos}],{reg}')
        for j in range(4): asm.append(f' movdqu [rsp+{0x40+j*16:x}h],xmm{j}')
        asm += [f' mov ecx,{i}', ' call ResolveRequired']
        for reg, pos in [('rcx','20h'),('rdx','28h'),('r8','30h'),('r9','38h')]:
            asm.append(f' mov {reg},[rsp+{pos}]')
        for j in range(4): asm.append(f' movdqu xmm{j},[rsp+{0x40+j*16:x}h]')
        asm += [' add rsp,88h', ' jmp rax', f'{symbol} ENDP']
    asm.append('END')
    (output/'forward.asm').write_text('\n'.join(asm)+'\n', encoding='ascii')
    (output/'proxy.def').write_text('\n'.join(defs)+'\n', encoding='ascii')
    (output/'expected_exports.json').write_text(json.dumps(exports, indent=2), encoding='utf8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', type=Path, default=Path(os.environ['SystemRoot'])/'System32/d3d11.dll')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--verify', type=Path)
    args = parser.parse_args()
    exports = pe_exports(args.dll)
    if args.verify:
        actual = pe_exports(args.verify)
        if actual != exports: raise SystemExit('FAIL: proxy export names / ordinals differ from system runtime')
        print(f'PASS: {len(exports)} export names and ordinals match {args.dll}')
    else:
        generate(exports, args.output)
        print(f'Generated {len(exports)} exports from {args.dll}')
