#!/usr/bin/env python3
"""Read TOTK RomFS map-object placements (BYAML v4) for the map-link collection sync.

The save carries object instance GUIDs, but not their coordinates.  The game's RomFS stores the
placements in Bake/Scene/*.bkres.zs -> SARC -> bkdat.byaml (BYAML v4, big-endian).  This script
reads those files straight out of the extracted RomFS image (index.json + romfs.bin), decompresses
with the game's zstd dictionary, parses BYAML and prints/dumps the placement records.

Usage:
  romfs_map_objects.py probe <romfs-dir> [scene-substring]
  romfs_map_objects.py dump  <romfs-dir> <out.json> [scene-prefix ...]

Read-only; no game data is modified.
"""
import json
import struct
import sys
from pathlib import Path

try:
    import zstandard as zstd
except ImportError:  # pragma: no cover - helper for a dev machine
    sys.exit('pip install zstandard')


# --------------------------------------------------------------------------------------------------
# RomFS image + zstd dictionary

class RomFS:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.index = {item['path']: item for item in json.loads((self.directory / 'index.json').read_text())['files']}
        self.image = open(self.directory / 'romfs.bin', 'rb')
        self._dicts = None

    def read(self, path):
        item = self.index[path]
        self.image.seek(item['offset'])
        return self.image.read(item['size'])

    def dictionaries(self):
        if self._dicts is not None:
            return self._dicts
        if 'Pack/ZsDic.pack.zs' not in self.index:
            raise SystemExit('Pack/ZsDic.pack.zs missing from the RomFS image')
        pack = zstd.ZstdDecompressor().decompress(self.read('Pack/ZsDic.pack.zs'))
        entries = sarc_entries(pack)
        by_name = {name: blob for name, blob in entries}
        self._dicts = [zstd.ZstdCompressionDict(by_name[name])
                       for name in ('zs.zsdic', 'bcett.byml.zsdic', 'pack.zsdic') if name in by_name]
        return self._dicts

    def decompress(self, frame):
        last = None
        for dictionary in self.dictionaries():
            try:
                return zstd.ZstdDecompressor(dict_data=dictionary).decompress(frame, max_output_size=256 << 20)
            except Exception as error:  # wrong dictionary
                last = error
        raise SystemExit(f'cannot decompress frame: {last}')


def sarc_entries(data):
    """Nintendo SARC (little-endian in TOTK): returns [(name, bytes)]."""
    if data[:4] != b'SARC':
        raise SystemExit('not a SARC container')
    data_offset = struct.unpack_from('<I', data, 0x0C)[0]
    sfat = 0x14
    count = struct.unpack_from('<H', data, sfat + 6)[0]
    base = sfat + struct.unpack_from('<H', data, sfat + 4)[0]
    sfnt = base + count * 16
    name_base = sfnt + 8
    out = []
    for i in range(count):
        _, attr, start, end = struct.unpack_from('<IIII', data, base + i * 16)
        offset = (attr & 0xFFFFFF) * 4
        name = data[name_base + offset:data.index(b'\0', name_base + offset)].decode()
        out.append((name, data[data_offset + start:data_offset + end]))
    return out


# --------------------------------------------------------------------------------------------------
# BYAML v4 (big-endian, "BY" magic)

class Byaml:
    TYPE_STRING_INDEX = 0xA0
    TYPE_BINARY = 0xA1
    TYPE_BINARY_ALIGNED = 0xA2
    TYPE_ARRAY = 0xC0
    TYPE_DICT = 0xC1
    TYPE_STRING_TABLE = 0xC2
    TYPE_DICT_REMAP = 0xC4
    TYPE_STRING_TABLE_RELOC = 0xC5
    TYPE_MONO_ARRAY = 0xC8
    TYPE_BOOL = 0xD0
    TYPE_S32 = 0xD1
    TYPE_F32 = 0xD2
    TYPE_U32 = 0xD3
    TYPE_S64 = 0xD4
    TYPE_U64 = 0xD5
    TYPE_F64 = 0xD6
    TYPE_NULL = 0xFF
    TYPE_HASH_ARRAY = 0x20
    TYPE_HASH_ARRAY_REMAP = 0x30

    def __init__(self, data):
        magic = data[0:2]
        if magic == b'BY':
            self.endian = '>'
        elif magic == b'YB':
            self.endian = '<'
        else:
            raise SystemExit(f'not a BYAML (magic {magic!r})')
        self.data = data
        self.version = struct.unpack_from(self.endian + 'H', data, 2)[0]
        self.key_offset = struct.unpack_from(self.endian + 'I', data, 4)[0]
        self.string_offset = struct.unpack_from(self.endian + 'I', data, 8)[0]
        self.root_offset = struct.unpack_from(self.endian + 'I', data, 0x0C)[0]
        self.keys = self._string_table(self.key_offset) if self.key_offset else []
        self.strings = self._string_table(self.string_offset) if self.string_offset else []

    def _u8(self, offset):
        return self.data[offset]

    def _u16(self, offset):
        return struct.unpack_from(self.endian + 'H', self.data, offset)[0]

    def _u24(self, offset):
        return int.from_bytes(self.data[offset:offset + 3], 'big' if self.endian == '>' else 'little')

    def _u32(self, offset):
        return struct.unpack_from(self.endian + 'I', self.data, offset)[0]

    def _u64(self, offset):
        return struct.unpack_from(self.endian + 'Q', self.data, offset)[0]

    def _f32(self, offset):
        return struct.unpack_from(self.endian + 'f', self.data, offset)[0]

    def _f64(self, offset):
        return struct.unpack_from(self.endian + 'd', self.data, offset)[0]

    def _string_table(self, offset):
        node_type = self._u8(offset)
        if node_type == self.TYPE_STRING_TABLE:
            count = self._u24(offset + 1)
            out = []
            for i in range(count):
                start = offset + self._u32(offset + 4 + i * 4)
                end = offset + self._u32(offset + 4 + (i + 1) * 4)
                out.append(self.data[start:end].decode('utf-8', 'replace'))
            return out
        if node_type == self.TYPE_STRING_TABLE_RELOC:
            count = self._u24(offset + 1)
            base_offset = self._u32(offset + 4)
            add = self._u64(offset + base_offset)
            out = []
            for i in range(count):
                start = offset + add + self._u32(offset + 8 + i * 4)
                end = offset + add + self._u32(offset + 8 + (i + 1) * 4)
                out.append(self.data[start:end].decode('utf-8', 'replace'))
            return out
        raise SystemExit(f'expected string table at 0x{offset:X}, got 0x{node_type:02X}')

    def node(self, offset):
        node_type = self._u8(offset)
        if node_type == self.TYPE_ARRAY:
            return self._array(offset)
        if node_type == self.TYPE_MONO_ARRAY:
            return self._mono_array(offset)
        if node_type == self.TYPE_DICT or node_type == self.TYPE_DICT_REMAP:
            return self._dict(offset)
        if self.TYPE_HASH_ARRAY <= node_type < self.TYPE_HASH_ARRAY + 0x10:
            return self._hash_array(offset)
        return self.value(node_type, offset)

    def value(self, node_type, offset):
        if node_type == self.TYPE_STRING_INDEX:
            return self.strings[self._u32(offset)]
        if node_type == self.TYPE_BOOL:
            return bool(self._u32(offset))
        if node_type == self.TYPE_S32:
            return struct.unpack_from(self.endian + 'i', self.data, offset)[0]
        if node_type == self.TYPE_U32:
            return self._u32(offset)
        if node_type == self.TYPE_F32:
            return self._f32(offset)
        if node_type == self.TYPE_S64:
            return struct.unpack_from(self.endian + 'q', self.data, self._u32(offset))[0]
        if node_type == self.TYPE_U64:
            return self._u64(self._u32(offset))
        if node_type == self.TYPE_F64:
            return self._f64(self._u32(offset))
        if node_type == self.TYPE_NULL:
            return None
        return self.node(self._u32(offset))

    def _array(self, offset):
        count = self._u24(offset + 1)
        types = self.data[offset + 4:offset + 4 + count]
        values = offset + 4 + ((count + 3) & ~3)
        return [self.value(types[i], values + i * 4) for i in range(count)]

    def _mono_array(self, offset):
        count = self._u24(offset + 1)
        node_type = self._u8(offset + 4)
        values = offset + 8
        return [self.value(node_type, values + i * 4) for i in range(count)]

    def _dict(self, offset):
        count = self._u24(offset + 1)
        out = {}
        for i in range(count):
            pair = offset + 4 + i * 8
            key = self.keys[self._u24(pair)]
            out[key.rstrip('\x00')] = self.value(self._u8(pair + 3), pair + 4)
        return out

    def _hash_array(self, offset):
        node_type = self._u8(offset)
        count = self._u24(offset + 1)
        value_size = ((node_type & 0x0F) or 1) * 4
        out = {}
        for i in range(count):
            pair = offset + 4 + i * (value_size + 4)
            if value_size == 4:
                key = self._u32(pair)
            elif value_size == 8:
                key = self._u64(pair)
            else:
                raise SystemExit(f'unsupported hash size {value_size}')
            out[key] = self._u32(pair + value_size)
        return out

    def root(self):
        return self.node(self.root_offset)


# --------------------------------------------------------------------------------------------------
# Scenes

def scene_records(romfs, path):
    """Return the decoded bkdat.byaml object of one Bake/Scene/*.bkres.zs file."""
    frame = romfs.read(path)
    bkres = romfs.decompress(frame)
    if bkres[:4] != b'SARC':
        return None
    for name, blob in sarc_entries(bkres):
        if name.endswith('.byaml') or name.endswith('.byml'):
            return Byaml(blob).root()
    return None


def probe_location_area(romfs):
    for path in ('Banc/MainField/LocationArea/MainField.locationarea.byml.zs',
                 'Banc/MinusField/LocationArea/MinusField.locationarea.byml.zs'):
        data = romfs.decompress(romfs.read(path))
        byaml = Byaml(data)
        root = byaml.root()
        print('==', path, 'version', byaml.version, 'root', type(root).__name__)
        if isinstance(root, dict):
            print('keys:', list(root.keys()))
            for key, value in root.items():
                print(' ', key, type(value).__name__, len(value) if hasattr(value, '__len__') else '')
                if isinstance(value, list) and value:
                    print('   sample:', json.dumps(value[0], ensure_ascii=False)[:300])


def probe(romfs, needle):
    names = [p for p in romfs.index if p.startswith('Bake/Scene/') and needle in p]
    if not names:
        sys.exit(f'no Bake/Scene file matching {needle!r}')
    path = names[0]
    print('scene:', path)
    root = scene_records(romfs, path)
    print('root type:', type(root).__name__)
    if isinstance(root, list):
        print('entries:', len(root))
        if root:
            print('first entry keys:', list(root[0].keys()) if isinstance(root[0], dict) else type(root[0]))
            print(json.dumps(root[0], ensure_ascii=False, default=str)[:2000])
    elif isinstance(root, dict):
        print('keys:', list(root.keys())[:30])
        print(json.dumps(root, ensure_ascii=False, default=str)[:2000])


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    command = sys.argv[1]
    romfs = RomFS(sys.argv[2])
    if command == 'location':
        probe_location_area(romfs)
    elif command == 'probe':
        probe(romfs, sys.argv[3] if len(sys.argv) > 3 else 'Dungeon060')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
