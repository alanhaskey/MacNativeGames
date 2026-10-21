#!/usr/bin/env python3
"""Collectible sync probe: snapshot a save, then diff it against the previous snapshot.

Read-only. Snapshots copy progress.sav (and record the player position from maplink.json when the game is
running) into a probe folder; `diff` prints the few hash-table keys that changed, with int/float decoding so a
position or a counter is recognisable. Use it to learn how a completed object is recorded before building the
game-side interface.

  python3 games/totk/scripts/diag/collection_probe.py snapshot --label before
  # play, do ONE thing (open one chest / collect one Korok / ...), then save the game
  python3 games/totk/scripts/diag/collection_probe.py after --label after
  python3 games/totk/scripts/diag/collection_probe.py list
  python3 games/totk/scripts/diag/collection_probe.py diff [A B]
"""
import argparse
import hashlib
import json
import shutil
import struct
import time
from pathlib import Path

DATA = Path.home() / 'Library/Application Support/MacNativeGames/TOTK'
PROFILE = DATA / 'profile'
PROBES = DATA / 'collection-probes'
MAPLINK = DATA / 'maplink.json'
GATEWAY = DATA / 'gateway.json'


def latest_save(profile):
    saves = sorted(profile.glob('save/*/slot_*/progress.sav'), key=lambda p: p.stat().st_mtime, reverse=True)
    if not saves:
        raise SystemExit(f'no progress.sav under {profile}')
    return saves[0]


def recent_json(path, max_age_seconds):
    try:
        stat = path.stat()
    except FileNotFoundError:
        return None
    if time.time() - stat.st_mtime > max_age_seconds:
        return None
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return None


def read_table(path):
    data = Path(path).read_bytes()
    if len(data) < 0x30 or struct.unpack_from('<I', data)[0] != 0x01020304:
        raise SystemExit(f'{path}: not a TOTK progress.sav')
    entries, end = {}, None
    for offset in range(0x28, len(data) - 8, 8):
        key, value = struct.unpack_from('<II', data, offset)
        if key == 0xA3DB7114:
            end = offset
            break
        entries[key] = value
    if end is None:
        raise SystemExit(f'{path}: END hash not found')
    return entries


def decode(value):
    """Readable forms of a raw u32 value: int, float32 (when plausible), and small flag. """
    signed = value - 0x100000000 if value & 0x80000000 else value
    float32 = struct.unpack('<f', struct.pack('<I', value))[0]
    parts = [f'int={signed}']
    try:
        if float32 == float32 and abs(float32) < 1e7:
            parts.append(f'f32={float32:.4f}')
    except Exception:
        pass
    if value in (0, 1, 0xFFFFFFFF):
        parts.append('flag' if value != 0xFFFFFFFF else 'unset')
    return ' '.join(parts)


def snapshot(args):
    source = Path(args.sav) if args.sav else latest_save(Path(args.profile))
    slot = source.parent.name.replace('slot_', '')
    stamp = time.strftime('%Y%m%d-%H%M%S')
    folder = Path(args.out) / f'{stamp}-{args.label}'
    folder.mkdir(parents=True, exist_ok=False)
    shutil.copy2(source, folder / 'progress.sav')
    data = (folder / 'progress.sav').read_bytes()
    meta = {
        'label': args.label,
        'slot': slot,
        'source': str(source),
        'savedAt': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(source.stat().st_mtime)),
        'size': len(data),
        'sha256': hashlib.sha256(data).hexdigest(),
        'player': (recent_json(MAPLINK, 60) or {}).get('x') is not None and {
            'x': recent_json(MAPLINK, 60).get('x'), 'y': recent_json(MAPLINK, 60).get('y'),
            'z': recent_json(MAPLINK, 60).get('z'), 'layer': recent_json(MAPLINK, 60).get('layer'),
        } or None,
        'gateway': recent_json(GATEWAY, 3600),
    }
    (folder / 'metadata.json').write_text(json.dumps(meta, indent=1) + '\n')
    print(f'snapshot {args.label}: {source} ({meta["size"]} bytes, slot {slot}) -> {folder}')
    return folder


def find_snapshots(root):
    return sorted(root.glob('*/progress.sav'), key=lambda p: p.stat().st_mtime)


def diff(args):
    root = Path(args.out)
    snaps = find_snapshots(root)
    if args.files:
        a, b = Path(args.files[0]), Path(args.files[1])
        a_meta = json.loads((a.parent / 'metadata.json').read_text()) if (a.parent / 'metadata.json').exists() else {}
        b_meta = json.loads((b.parent / 'metadata.json').read_text()) if (b.parent / 'metadata.json').exists() else {}
    else:
        if len(snaps) < 2:
            raise SystemExit('need at least two snapshots (snapshot --label before, play, after --label after)')
        a, b = snaps[-2], snaps[-1]
        a_meta = json.loads((a.parent / 'metadata.json').read_text())
        b_meta = json.loads((b.parent / 'metadata.json').read_text())
    table_a, table_b = read_table(a), read_table(b)
    added = sorted(set(table_b) - set(table_a))
    removed = sorted(set(table_a) - set(table_b))
    changed = sorted(k for k in set(table_a) & set(table_b) if table_a[k] != table_b[k])

    result = {'a': {'file': str(a), 'label': a_meta.get('label'), 'entries': len(table_a)},
              'b': {'file': str(b), 'label': b_meta.get('label'), 'entries': len(table_b)},
              'playerA': a_meta.get('player'), 'playerB': b_meta.get('player'),
              'added': [[k, table_b[k]] for k in added],
              'removed': [[k, table_a[k]] for k in removed],
              'changed': [[k, table_a[k], table_b[k]] for k in changed]}
    if args.json:
        print(json.dumps(result, indent=1))
        return
    print(f'A: {a} ({len(table_a)} entries, label {a_meta.get("label")})')
    print(f'B: {b} ({len(table_b)} entries, label {b_meta.get("label")})')
    if a_meta.get('player') or b_meta.get('player'):
        print(f'player A: {a_meta.get("player")}  player B: {b_meta.get("player")}')
    print(f'\nadded keys ({len(added)}):')
    for key in added:
        print(f'  0x{key:08X} = 0x{table_b[key]:08X}  {decode(table_b[key])}')
    print(f'\nremoved keys ({len(removed)}):')
    for key in removed:
        print(f'  0x{key:08X} = 0x{table_a[key]:08X}  {decode(table_a[key])}')
    print(f'\nchanged keys ({len(changed)}):')
    for key in changed:
        print(f'  0x{key:08X}: 0x{table_a[key]:08X} -> 0x{table_b[key]:08X}  [{decode(table_a[key])}] -> [{decode(table_b[key])}]')


def list_(args):
    root = Path(args.out)
    for save in find_snapshots(root):
        meta_file = save.parent / 'metadata.json'
        meta = json.loads(meta_file.read_text()) if meta_file.exists() else {}
        print(f'{save.parent.name:30s} slot={meta.get("slot","?"):2s} size={meta.get("size",0)} '
              f'player={meta.get("player")}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', default=str(PROBES), help='snapshot folder (default: %(default)s)')
    sub = parser.add_subparsers(dest='command', required=True)

    snap = sub.add_parser('snapshot', help='copy the newest save into a snapshot')
    snap.add_argument('--label', required=True)
    snap.add_argument('--sav', help='explicit progress.sav (default: newest under the profile)')
    snap.add_argument('--profile', default=str(PROFILE))
    snap.set_defaults(func=snapshot)

    after = sub.add_parser('after', help='snapshot, then diff the previous snapshot against this one')
    after.add_argument('--label', default='after')
    after.add_argument('--sav')
    after.add_argument('--profile', default=str(PROFILE))
    after.add_argument('--json', action='store_true')
    after.set_defaults(func=lambda a: (snapshot(a), diff(a)))

    d = sub.add_parser('diff', help='diff two snapshots (default: the last two)')
    d.add_argument('files', nargs='*', help='two progress.sav files (default: last two snapshots)')
    d.add_argument('--json', action='store_true')
    d.set_defaults(func=diff)

    l = sub.add_parser('list', help='list snapshots')
    l.set_defaults(func=list_)

    args = parser.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
