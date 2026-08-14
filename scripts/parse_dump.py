#!/usr/bin/env python3
import sys
from struct import unpack_from

path = sys.argv[1]
data = open(path, "rb").read()
print(f"file size: {len(data)} bytes")

m0, m1 = unpack_from("<2I", data, 0)
print(f"magic: {m0:#010x} {m1:#010x}")
if (m0, m1) != (0xDEADC0DE, 0xDEADCAFE):
    print("!! unexpected magic (not a Luma arm11 crash dump?)")

(version, processor, excType, _res,
 regDumpSize, codeDumpSize, stackDumpSize, otherSize) = unpack_from("<8I", data, 8)

verMaj = version & 0xFFFF
verMin = version >> 16
print(f"version: {verMaj}.{verMin}")
print(f"processor field: {processor:#x} (core {processor>>16}, id {processor & 0xFFFF})")

excNames = {0: "FIQ", 1: "undefined instruction", 2: "prefetch abort", 3: "data abort"}
print(f"exception type: {excType} ({excNames.get(excType,'?')})")
print(f"regDumpSize={regDumpSize} codeDumpSize={codeDumpSize} "
      f"stackDumpSize={stackDumpSize} otherSize={otherSize}")

off = 0x28
nregs = regDumpSize // 4
regs = list(unpack_from(f"<{nregs}I", data, off))
off += regDumpSize

core = ["r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12",
        "sp","lr","pc","cpsr","dfsr","ifsr","far","fpexc","fpinst","fpinst2"]
print("\n-- registers --")
for i, v in enumerate(regs):
    name = core[i] if i < len(core) else f"reg{i}"
    print(f"  {name:>8} = {v:#010x}")

# Code dump (bytes around PC)
code = data[off:off+codeDumpSize]
off += codeDumpSize
# Stack dump
stack = data[off:off+stackDumpSize]
off += stackDumpSize
# Additional data (process name etc.)
other = data[off:off+otherSize]

def ascii_strings(b, minlen=3):
    out, cur = [], b""
    for ch in b:
        if 32 <= ch < 127:
            cur += bytes([ch])
        else:
            if len(cur) >= minlen: out.append(cur.decode())
            cur = b""
    if len(cur) >= minlen: out.append(cur.decode())
    return out

print("\n-- additional data (strings) --")
for s in ascii_strings(other):
    print("  ", s)

if codeDumpSize:
    print(f"\ncode dump: {codeDumpSize} bytes (hex, first 64):")
    print("  ", code[:64].hex())
