#!/usr/bin/env python3
"""Read the hash table at the top of a TOTK progress.sav and diff two saves (read-only, never writes).

The table is a sequence of little-endian u32 key/value pairs after the 8-byte header, ending at the
END hash 0xA3DB7114. Keys are hashed flag/object names; this tool only compares them, it does not need
to know the hash function yet.

Usage:
  save_table_diff.py --dump A.sav                 # key/value pairs of one save
  save_table_diff.py A.sav B.sav [--limit N]      # added / removed / changed keys between two saves
  save_table_diff.py A.sav B.sav --json
"""
import argparse
import json
import struct
from pathlib import Path

MAGIC = 0x01020304
END_HASH = 0xA3DB7114


def read_table(path):
    data = Path(path).read_bytes()
    if len(data) < 0x30 or struct.unpack_from('<I', data)[0] != MAGIC:
        raise SystemExit(f'{path}: not a TOTK progress.sav (magic mismatch)')
    entries = {}
    end = None
    for offset in range(0x28, len(data) - 8, 8):
        key, value = struct.unpack_from('<II', data, offset)
        if key == END_HASH:
            end = offset
            break
        entries[key] = value
    if end is None:
        raise SystemExit(f'{path}: END hash not found')
    return entries, len(data), end


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('saves', nargs='+', help='one save to --dump, or two saves to diff')
    parser.add_argument('--dump', action='store_true', help='print the key/value table of a single save')
    parser.add_argument('--limit', type=int, default=40, help='how many entries to print per section (default 40)')
    parser.add_argument('--json', action='store_true', help='machine-readable output')
    args = parser.parse_args()

    if args.dump:
        entries, size, end = read_table(args.saves[0])
        if args.json:
            print(json.dumps({'size': size, 'endOffset': end, 'entries': len(entries),
                              'table': [[k, v] for k, v in entries.items()]}, indent=1))
            return
        print(f'{args.saves[0]}: {len(entries)} entries, table ends at 0x{end:x}, file {size} bytes')
        for key, value in list(entries.items())[:args.limit]:
            print(f'  key=0x{key:08X} value=0x{value:08X} ({value})')
        return

    if len(args.saves) != 2:
        parser.error('give one save with --dump, or two saves to diff')
    a, a_size, a_end = read_table(args.saves[0])
    b, b_size, b_end = read_table(args.saves[1])
    added = sorted(set(b) - set(a))
    removed = sorted(set(a) - set(b))
    changed = sorted(k for k in set(a) & set(b) if a[k] != b[k])
    result = {
        'a': {'file': str(args.saves[0]), 'entries': len(a), 'size': a_size, 'endOffset': a_end},
        'b': {'file': str(args.saves[1]), 'entries': len(b), 'size': b_size, 'endOffset': b_end},
        'added': [[k, b[k]] for k in added],
        'removed': [[k, a[k]] for k in removed],
        'changed': [[k, a[k], b[k]] for k in changed],
    }
    if args.json:
        print(json.dumps(result, indent=1))
        return
    print(f'A: {args.saves[0]} ({len(a)} entries)')
    print(f'B: {args.saves[1]} ({len(b)} entries)')
    print(f'added keys ({len(added)}):   ' + ', '.join(f'0x{k:08X}=0x{b[k]:08X}' for k in added[:args.limit]))
    print(f'removed keys ({len(removed)}): ' + ', '.join(f'0x{k:08X}=0x{a[k]:08X}' for k in removed[:args.limit]))
    print(f'changed keys ({len(changed)}): ' + ', '.join(f'0x{k:08X}: 0x{a[k]:08X} -> 0x{b[k]:08X}' for k in changed[:args.limit]))


if __name__ == '__main__':
    main()
