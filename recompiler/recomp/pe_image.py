"""Flat view of a PE32 image as it would be mapped in memory."""
import hashlib
import struct

import pefile

GOG_GAME_EXE_SHA256 = "b2f921098f9ba59b076b3dd0bbd013e40b06b8481651b4a7a774757c95cb2187"


class Section:
    def __init__(self, name, va, vsize, raw_size, characteristics):
        self.name = name
        self.va = va
        self.vsize = vsize
        self.raw_size = raw_size
        self.characteristics = characteristics

    @property
    def end(self):
        return self.va + self.vsize

    @property
    def executable(self):
        return bool(self.characteristics & 0x20000000)

    def __repr__(self):
        return f"Section({self.name} {self.va:#x}-{self.end:#x})"


class Image:
    def __init__(self, path, require_gog=True):
        raw = open(path, "rb").read()
        self.sha256 = hashlib.sha256(raw).hexdigest()
        if require_gog and self.sha256 != GOG_GAME_EXE_SHA256:
            raise SystemExit(
                f"{path}: SHA-256 {self.sha256} is not the GOG game.EXE ({GOG_GAME_EXE_SHA256})")

        pe = pefile.PE(data=raw)
        oh = pe.OPTIONAL_HEADER
        self.base = oh.ImageBase
        self.entry = oh.ImageBase + oh.AddressOfEntryPoint
        self.size = oh.SizeOfImage

        mapped = pe.get_memory_mapped_image()
        self.mem = bytearray(self.size)
        self.mem[:len(mapped)] = mapped[:self.size]

        self.sections = [
            Section(s.Name.rstrip(b"\0").decode(), self.base + s.VirtualAddress,
                    s.Misc_VirtualSize, s.SizeOfRawData, s.Characteristics)
            for s in pe.sections
        ]
        self.text = next(s for s in self.sections if s.name == ".text")

        # VAs of every 32-bit slot the loader would fix up (IMAGE_REL_BASED_HIGHLOW).
        self.relocs = set()
        for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
            for e in block.entries:
                if e.type == pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"]:
                    self.relocs.add(self.base + e.rva)

        # IAT slot VA -> (dll, name or "#ordinal")
        self.imports = {}
        for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll = d.dll.decode()
            for imp in d.imports:
                name = imp.name.decode() if imp.name else f"#{imp.ordinal}"
                self.imports[imp.address] = (dll, name)

    def contains(self, va):
        return self.base <= va < self.base + self.size

    def in_text(self, va):
        return self.text.va <= va < self.text.end

    def section_of(self, va):
        for s in self.sections:
            if s.va <= va < s.end:
                return s
        return None

    def u8(self, va):
        return self.mem[va - self.base]

    def u32(self, va):
        return struct.unpack_from("<I", self.mem, va - self.base)[0]

    def bytes(self, va, n):
        o = va - self.base
        return bytes(self.mem[o:o + n])
