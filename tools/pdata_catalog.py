"""Function catalog for an x64 PE from .pdata + direct call targets (+ entry/TLS/exports).
usage: pdata_catalog.py <exe> <out.txt>   ->  lines '0xVA size name' for generate64.py"""
import sys, struct, pefile, capstone, bisect
exe, out = sys.argv[1], sys.argv[2]
pe = pefile.PE(exe)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
ex = [s for s in pe.sections if s.Characteristics & 0x20000000]   # all executable sections (.text, il2cpp, ...)
tlo = base + min(s.VirtualAddress for s in ex)
thi = base + max(s.VirtualAddress + s.Misc_VirtualSize for s in ex)
starts = set(); ranges = []
d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
n = d.Size // 12
chained = 0
for i in range(n):
    b, e, u = struct.unpack_from('<III', img, d.VirtualAddress + 12 * i)
    flags = img[u] >> 3 if u < len(img) else 0
    ranges.append((base + b, base + e))
    if flags & 4: chained += 1; continue          # UNW_FLAG_CHAININFO: continuation chunk
    starts.add(base + b)
if pe.OPTIONAL_HEADER.AddressOfEntryPoint:
    starts.add(base + pe.OPTIONAL_HEADER.AddressOfEntryPoint)
# exports: a DLL's API is reached through GetProcAddress, never by a direct call
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
nexp = 0
for e in getattr(getattr(pe, 'DIRECTORY_ENTRY_EXPORT', None), 'symbols', []) or []:
    if e.address and not e.forwarder:
        sec = pe.get_section_by_rva(e.address)
        if sec and sec.Characteristics & 0x20000000:        # executable section: a function, not data
            starts.add(base + e.address); nexp += 1
print(f'exports {nexp}')
if hasattr(pe, 'DIRECTORY_ENTRY_TLS'):
    cb = pe.DIRECTORY_ENTRY_TLS.struct.AddressOfCallBacks
    k = 0
    while cb:
        v = struct.unpack_from('<Q', img, cb - base + 8 * k)[0]
        if not v: break
        starts.add(v); k += 1
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
calls = set()
for lo, hi in ranges:
    for ins in md.disasm(img[lo - base:hi - base], lo):
        if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
            t = int(ins.op_str, 16)
            if tlo <= t < thi and ins.mnemonic == 'call': calls.add(t)
pd = len(starts)
starts |= calls
s = sorted(x for x in starts if tlo <= x < thi)
with open(out, 'w') as f:
    for i, va in enumerate(s):
        end = s[i + 1] if i + 1 < len(s) else thi
        f.write('0x%X %X sub_%X\n' % (va, min(end - va, 0x40000), va))
print(f'pdata entries {n} ({chained} chained), starts from pdata/entry/tls {pd}, call targets {len(calls)}, catalog {len(s)}')
