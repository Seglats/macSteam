#!/usr/bin/env python3
"""wrap_raw.py — wrap a raw arm64 code dump in a Mach-O so otool can disassemble it.

usage: wrap_raw.py dump.bin base_addr   (base as hex, from jit-N.txt)
writes dump.bin.macho. vmaddr = base so disassembly addresses match the live process.
"""
import struct, sys

def main():
    data = open(sys.argv[1], "rb").read()
    base = int(sys.argv[2], 16)
    fileoff = 32 + 152  # mach_header_64 + LC_SEGMENT_64 with one 80-byte section
    # section_64: sectname,segname,addr,size,offset,align,reloff,nreloc,flags,res1,res2,res3
    sect = struct.pack(
        "<16s16sQQIIIIIIII",
        b"__text", b"__TEXT", base, len(data), fileoff, 2, 0, 0, 0, 0, 0, 0)
    assert len(sect) == 80, len(sect)
    # LC_SEGMENT_64: cmd,cmdsize,segname[16],vmaddr,vmsize,fileoff,filesize,
    #                maxprot,initprot,nsects,flags
    seg = struct.pack(
        "<II16sQQQQiiII",
        0x19, 152, b"__TEXT", base, len(data), 0, fileoff + len(data), 7, 5, 1, 0)
    header = struct.pack(
        "<IiiIIIII",
        0xFEEDFACF, 0x0100000C, 0, 2, 1, len(seg) + len(sect), 0, 0)
    out = sys.argv[1] + ".macho"
    with open(out, "wb") as f:
        f.write(header + seg + sect + data)
    print(out)

if __name__ == "__main__":
    main()
