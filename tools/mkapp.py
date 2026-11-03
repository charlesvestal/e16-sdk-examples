#!/usr/bin/env python3
"""Turn a linked app ELF into an .e16app image.

  mkapp.py [--name NAME] <app.elf> <out.e16app>

The ELF must come from apps/sdk/app.ld with -Wl,--emit-relocs (that's what
e16_add_app() in apps/sdk/cmake/e16app.cmake does). The format is in
src/apps/e16_app_format.h; this file and that header must agree.

Besides converting, this is the build-time half of the loader's safety
check. The loader can only rebase two things: the GOT and the data
fixup table (words in .data holding a link-time address). So every
relocation the linker resolved is checked to be one whose result stays
right when text and data move independently:

  in .text   PC-relative branches/loads to text (calls, literal pools);
             GOT-relative offsets (GOT_BREL: the code's only way to
             reach a global; GOTOFF32 only to data); absolute values
             only for absolute symbols (no addresses).
  in .data   absolute addresses of text or data (-> fixup table);
             PC-relative only to data.

Anything else -- an absolute address baked into the code, code reaching
data PC-relatively, an undefined weak symbol (its GOT entry would be 0,
which isn't "absent" after rebasing), C++ constructors, an unknown
section -- is refused with a message naming it.

No dependencies beyond the Python standard library (ELF parsed with
struct).
"""
import argparse
import struct
import sys
import zlib

# ---- format constants (src/apps/e16_app_format.h) ----
MAGIC = b"E16A"
ABI_VERSION = 1
HEADER_SIZE = 76
NAME_MAX = 24
HEADER_FMT = "<4sHHIIIIIIIIIII24s"
assert struct.calcsize(HEADER_FMT) == HEADER_SIZE

# ---- ELF ----
SHT_SYMTAB, SHT_RELA, SHT_NOBITS, SHT_REL = 2, 4, 8, 9
SHF_ALLOC = 0x2
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2
EM_ARM = 40
ET_EXEC = 2

# ARM relocation types (ELF for the Arm Architecture, AAELF32).
R = dict(
    NONE=0, PC24=1, ABS32=2, REL32=3, THM_CALL=10, THM_PC8=11, GOTOFF32=24,
    BASE_PREL=25, GOT_BREL=26, PLT32=27, CALL=28, JUMP24=29, THM_JUMP24=30,
    TARGET1=38, V4BX=40, TARGET2=41, PREL31=42, MOVW_ABS_NC=43, MOVT_ABS=44,
    MOVW_PREL_NC=45, MOVT_PREL=46, THM_MOVW_ABS_NC=47, THM_MOVT_ABS=48,
    THM_MOVW_PREL_NC=49, THM_MOVT_PREL=50, THM_JUMP19=51, THM_ALU_PREL_11_0=53,
    THM_PC12=54, GOT_PREL=96, THM_JUMP11=102, THM_JUMP8=103,
)
RNAME = {v: "R_ARM_" + k for k, v in R.items()}

# PC-relative: fine when source and target move together (same region).
PC_RELATIVE = {R[n] for n in (
    "PC24", "REL32", "THM_CALL", "THM_PC8", "CALL", "JUMP24", "THM_JUMP24",
    "PREL31", "MOVW_PREL_NC", "MOVT_PREL", "THM_MOVW_PREL_NC", "THM_MOVT_PREL",
    "THM_JUMP19", "THM_ALU_PREL_11_0", "THM_PC12", "THM_JUMP11", "THM_JUMP8",
    "PLT32")}
# Absolute: a link-time address (or an absolute constant).
ABSOLUTE = {R[n] for n in (
    "ABS32", "TARGET1", "MOVW_ABS_NC", "MOVT_ABS", "THM_MOVW_ABS_NC", "THM_MOVT_ABS")}

TEXT_SECTIONS = {".text"}
DATA_SECTIONS = {".got", ".data", ".bss"}
EMPTY_ONLY = {".init_array", ".fini_array"}


class AppError(Exception):
    pass


class Elf:
    def __init__(self, blob):
        self.b = blob
        if blob[:4] != b"\x7fELF":
            raise AppError("not an ELF file")
        if blob[4] != 1 or blob[5] != 1:
            raise AppError("not a 32-bit little-endian ELF")
        (self.e_type, self.e_machine, _, self.e_entry, _, e_shoff, _, _, _, _,
         e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", blob, 16)
        if self.e_machine != EM_ARM or self.e_type != ET_EXEC:
            raise AppError("not an ARM executable")
        self.sections = []
        for i in range(e_shnum):
            f = struct.unpack_from("<IIIIIIIIII", blob, e_shoff + i * e_shentsize)
            self.sections.append(dict(
                index=i, name_off=f[0], type=f[1], flags=f[2], addr=f[3], offset=f[4],
                size=f[5], link=f[6], info=f[7], addralign=f[8], entsize=f[9]))
        strtab = self.sections[e_shstrndx]
        for s in self.sections:
            s["name"] = self.cstr(strtab["offset"] + s["name_off"])
        self.by_name = {s["name"]: s for s in self.sections}

        self.symbols = []
        symtab = next((s for s in self.sections if s["type"] == SHT_SYMTAB), None)
        if symtab is None:
            raise AppError("no symbol table (was the ELF stripped?)")
        names = self.sections[symtab["link"]]
        for i in range(symtab["size"] // 16):
            st_name, value, size, info, other, shndx = struct.unpack_from(
                "<IIIBBH", blob, symtab["offset"] + 16 * i)
            self.symbols.append(dict(
                name=self.cstr(names["offset"] + st_name), value=value, size=size,
                bind=info >> 4, type=info & 15, shndx=shndx))

    def cstr(self, off):
        end = self.b.index(b"\0", off)
        return self.b[off:end].decode("ascii", "replace")

    def data(self, s):
        if s["type"] == SHT_NOBITS:
            return bytes(s["size"])  # no file contents: zeros
        return self.b[s["offset"]:s["offset"] + s["size"]]

    def rels(self, s):
        for i in range(s["size"] // 8):
            r_offset, r_info = struct.unpack_from("<II", self.b, s["offset"] + 8 * i)
            yield r_offset, r_info & 0xFF, r_info >> 8

    def symbol(self, name):
        return next((s for s in self.symbols if s["name"] == name), None)


def align4(n):
    return (n + 3) & ~3


def build(elf_bytes, name):
    elf = Elf(elf_bytes)
    secs = elf.by_name

    # ---- sections ----
    for s in elf.sections:
        if not s["flags"] & SHF_ALLOC:
            continue
        n = s["name"]
        if n in EMPTY_ONLY:
            if s["size"]:
                raise AppError("%s is not empty: global constructors/destructors "
                               "aren't supported" % n)
            continue
        if n not in TEXT_SECTIONS | DATA_SECTIONS:
            raise AppError("unexpected section %s (size %d): not placed by app.ld"
                           % (n, s["size"]))
        if s["addralign"] > 8:
            raise AppError("section %s needs %d-byte alignment; at most 8 is supported"
                           % (n, s["addralign"]))
    for n in (".text", ".got", ".data", ".bss"):
        if n not in secs:
            raise AppError("missing section %s (link with apps/sdk/app.ld)" % n)
    text, got, data, bss = secs[".text"], secs[".got"], secs[".data"], secs[".bss"]
    # .data can be NOBITS when it holds nothing but alignment padding
    # (an app without initialized data): its bytes are then zeros.
    if bss["type"] != SHT_NOBITS:
        raise AppError(".bss has the wrong section type")

    text_base = text["addr"]
    text_bytes = elf.data(text)
    text_size = align4(len(text_bytes))
    text_bytes += b"\0" * (text_size - len(text_bytes))

    data_base = got["addr"]
    if data["addr"] < got["addr"] + got["size"]:
        raise AppError(".data overlaps the GOT")
    data_end = data["addr"] + data["size"]
    data_size = align4(data_end - data_base)
    image = bytearray(data_size)
    for s in (got, data):
        off = s["addr"] - data_base
        image[off:off + s["size"]] = elf.data(s)
    if bss["addr"] < data_end:
        raise AppError(".bss overlaps .data")
    bss_end = bss["addr"] + bss["size"]
    bss_size = align4(bss_end - data_base) - data_size if bss["size"] else 0

    # ---- ranges, as the loader sees them ----
    data_total = data_size + bss_size

    def region(v):
        if text_base <= v <= text_base + text_size:
            return "text"
        if data_base <= v <= data_base + data_total:
            return "data"
        return None

    if not (text_base + text_size < data_base or data_base + data_total < text_base):
        raise AppError("text and data link ranges overlap")

    # ---- GOT ----
    gsym = elf.symbol("_GLOBAL_OFFSET_TABLE_")
    if gsym is not None and gsym["value"] != got["addr"]:
        raise AppError("_GLOBAL_OFFSET_TABLE_ isn't at the start of .got")
    got_offset = got["addr"] - data_base
    got_count = got["size"] // 4
    for i in range(got_count):
        (v,) = struct.unpack_from("<I", image, got_offset + 4 * i)
        if region(v) is None:
            raise AppError("GOT entry %d = 0x%08x points outside the app" % (i, v))

    # ---- symbols: nothing may stay undefined ----
    for sym in elf.symbols[1:]:
        if sym["shndx"] == SHN_UNDEF and sym["name"]:
            raise AppError("undefined symbol %s (weak references aren't supported: "
                           "a missing one would look present after loading)" % sym["name"])

    def sym_region(sym):
        if sym["shndx"] == SHN_ABS:
            return "abs"
        if sym["shndx"] in (SHN_UNDEF, SHN_COMMON) or sym["shndx"] >= len(elf.sections):
            return None
        n = elf.sections[sym["shndx"]]["name"]
        if n in TEXT_SECTIONS:
            return "text"
        if n in DATA_SECTIONS:
            return "data"
        return None

    # ---- relocations (kept by --emit-relocs) ----
    fixups = set()
    rel_sections = [s for s in elf.sections if s["type"] in (SHT_REL, SHT_RELA)]
    if not any(elf.sections[s["info"]]["name"] == ".text" for s in rel_sections
               if s["info"] < len(elf.sections)):
        raise AppError("no relocations for .text: link with -Wl,--emit-relocs")
    for rs in rel_sections:
        target = elf.sections[rs["info"]]
        tname = target["name"]
        if not target["flags"] & SHF_ALLOC:
            continue
        if rs["type"] == SHT_RELA:
            raise AppError("RELA relocations in %s aren't expected" % tname)
        where = "text" if tname in TEXT_SECTIONS else "data" if tname in DATA_SECTIONS else None
        if where is None:
            raise AppError("relocations against unexpected section %s" % tname)
        for r_offset, rtype, symi in elf.rels(rs):
            sym = elf.symbols[symi]
            if sym["name"]:
                sname = sym["name"]
            elif sym["shndx"] < len(elf.sections):
                sname = elf.sections[sym["shndx"]]["name"]
            else:
                sname = "?"
            to = sym_region(sym) if symi else "abs"
            what = "%s at 0x%08x (%s -> %s)" % (RNAME.get(rtype, "type %d" % rtype),
                                                 r_offset, tname, sname)
            if rtype in (R["NONE"], R["V4BX"]):
                continue
            if to is None:
                raise AppError("relocation to an unplaced symbol: " + what)
            if rtype in PC_RELATIVE:
                if to != where:
                    raise AppError("PC-relative reference between text and data "
                                   "(they load independently): " + what)
                continue
            if rtype == R["GOT_BREL"]:
                continue  # an offset into the GOT, which the loader rebases
            if rtype == R["GOTOFF32"]:
                if to != "data":
                    raise AppError("GOT-relative reference to text (build with "
                                   "-mno-pic-data-is-text-relative): " + what)
                continue
            if rtype in ABSOLUTE:
                if to == "abs":
                    continue
                if where == "text" or rtype not in (R["ABS32"], R["TARGET1"]):
                    raise AppError("absolute address in code (needs runtime patching "
                                   "outside the GOT; is everything built with -fpic?): "
                                   + what)
                if tname == ".got":
                    continue  # the whole GOT is rebased anyway
                off = r_offset - data_base
                if off & 3 or not 0 <= off < data_size:
                    raise AppError("misplaced data relocation: " + what)
                (v,) = struct.unpack_from("<I", image, off)
                if region(v) is None:
                    raise AppError("pointer 0x%08x outside the app: %s" % (v, what))
                fixups.add(off)
                continue
            raise AppError("unsupported relocation " + what)
    if fixups & set(range(got_offset, got_offset + 4 * got_count)):
        raise AppError("data fixup inside the GOT")
    fixups = sorted(fixups)

    # ---- entry ----
    if not elf.e_entry & 1:
        raise AppError("entry point 0x%08x isn't Thumb code" % elf.e_entry)
    entry_offset = (elf.e_entry & ~1) - text_base
    if not 0 <= entry_offset < text_size:
        raise AppError("entry point outside .text")

    # ---- header ----
    nb = name.encode("ascii")
    if not nb or len(nb) >= NAME_MAX or not all(32 <= c < 127 for c in nb):
        raise AppError("app name must be 1-%d printable ASCII characters" % (NAME_MAX - 1))
    payload = bytes(text_bytes) + bytes(image) + struct.pack("<%dI" % len(fixups), *fixups)

    def header(crc):
        return struct.pack(HEADER_FMT, MAGIC, ABI_VERSION, HEADER_SIZE, 0,
                           text_size, data_size, bss_size, got_offset, got_count,
                           len(fixups), entry_offset, text_base, data_base, crc,
                           nb.ljust(NAME_MAX, b"\0"))

    crc = zlib.crc32(payload, zlib.crc32(header(0))) & 0xFFFFFFFF
    out = header(crc) + payload
    info = dict(text=text_size, data=data_size, bss=bss_size, got=got_count,
                fixups=len(fixups), file=len(out))
    return out, info


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--name", help="app name (default: the output's base name)")
    ap.add_argument("elf")
    ap.add_argument("out")
    a = ap.parse_args()
    name = a.name
    if not name:
        import os
        name = os.path.splitext(os.path.basename(a.out))[0]
    with open(a.elf, "rb") as f:
        blob = f.read()
    try:
        out, info = build(blob, name)
    except AppError as e:
        sys.stderr.write("mkapp: %s: %s\n" % (a.elf, e))
        sys.exit(1)
    with open(a.out, "wb") as f:
        f.write(out)
    print("mkapp: %s: text %d data %d (GOT %d entries) bss %d fixups %d -> %d bytes"
          % (a.out, info["text"], info["data"], info["got"], info["bss"],
             info["fixups"], info["file"]))


if __name__ == "__main__":
    main()
