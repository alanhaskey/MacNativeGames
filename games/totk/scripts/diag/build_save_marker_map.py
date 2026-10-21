#!/usr/bin/env python3
"""Build the save-key -> wiki-marker mapping for the collection sync.

Reads the extracted TOTK RomFS (Banc/*/LocationArea/*.locationarea.byml.zs), derives the save hash
keys for shrines, towers, lightroots, dragon tears, caves, chasms and wells, and joins them to the
wiki markers (data/content/map-content.json) by coordinates.  The wiki stores game_z negated
relative to the RomFS/save Z.

Usage: build_save_marker_map.py <romfs-dir> <wiki-map-content.json> <out.json>
"""
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from romfs_map_objects import RomFS, Byaml  # noqa: E402


def murmur3_32(text):
    data = text.encode()
    c1, c2 = 0xCC9E2D51, 0x1B873593
    h = 0
    n = len(data)
    i = 0
    while i + 4 <= n:
        k = struct.unpack_from('<I', data, i)[0]
        i += 4
        k = (k * c1) & 0xFFFFFFFF
        k = ((k << 15) | (k >> 17)) & 0xFFFFFFFF
        k = (k * c2) & 0xFFFFFFFF
        h ^= k
        h = ((h << 13) | (h >> 19)) & 0xFFFFFFFF
        h = (h * 5 + 0xE6546B64) & 0xFFFFFFFF
    k = 0
    tail = data[i:]
    if len(tail) >= 3:
        k ^= tail[2] << 16
    if len(tail) >= 2:
        k ^= tail[1] << 8
    if len(tail) >= 1:
        k ^= tail[0]
        k = (k * c1) & 0xFFFFFFFF
        k = ((k << 15) | (k >> 17)) & 0xFFFFFFFF
        k = (k * c2) & 0xFFFFFFFF
        h ^= k
    h ^= n
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    h ^= h >> 16
    return h


def hash_key(name):
    return '0x%08X' % murmur3_32(name)


LOCATION_FILES = (
    'Banc/MainField/LocationArea/MainField.locationarea.byml.zs',
    'Banc/MinusField/LocationArea/MinusField.locationarea.byml.zs',
    'Banc/LargeDungeon/LocationArea/LargeDungeon.locationarea.byml.zs',
    'Banc/SmallDungeon/LocationArea/SmallDungeon.locationarea.byml.zs',
    'Banc/BossVehicle/LocationArea/BossVehicle.locationarea.byml.zs',
    'Banc/NormalStage/LocationArea/NormalStage.locationarea.byml.zs',
)


def rows(root, category):
    out = []
    for item in root.get(category) or []:
        location = item.get('LocationName')
        if isinstance(location, list):
            location = location[0] if location else ''
        location = (location or '').rstrip('\x00')
        name = location.split('/')[-1].replace('.game__location__Location.gyml', '')
        ids = item.get('InstanceID')
        if isinstance(ids, str):
            ids = [ids]
        ids = [x.rstrip('\x00') for x in (ids or [])]
        translate = item.get('Trans')
        if isinstance(translate, list) and translate and isinstance(translate[0], list):
            translate = translate[0]
        out.append({'name': name, 'ids': ids, 'pos': translate})
    return out


def nearest(markers, x, z, maximum=60.0):
    best = None
    best_distance = maximum
    for marker in markers:
        mx, mz = marker.get('game_x'), marker.get('game_z')
        if mx is None or mz is None:
            continue
        distance = ((mx - x) ** 2 + (mz + z) ** 2) ** 0.5
        if distance < best_distance:
            best, best_distance = marker, distance
    return best, best_distance


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    romfs = RomFS(sys.argv[1])
    wiki = json.loads(Path(sys.argv[2]).read_text())
    out_path = Path(sys.argv[3])
    markers = wiki['markers']

    def by_subtype(name):
        return [m for m in markers if m.get('subtype') == name]

    shrine_markers = by_subtype('破魔神庙')
    tower_markers = by_subtype('鸟望台')
    root_markers = by_subtype('破魔之根')
    tear_markers = by_subtype('龙之泪')
    cave_markers = by_subtype('洞窟入口')
    chasm_markers = by_subtype('深穴')
    well_markers = by_subtype('井')

    categories = {
        'shrines_found': {},
        'shrines_status': {},
        'towers': {},
        'lightroots': {},
        'dragon_tears': {},
        'caves': {},
        'chasms': {},
        'wells': {},
    }
    details = {name: {} for name in categories}
    unmatched = []

    def add(category, key, marker, kind, name, pos, distance=0.0):
        categories[category][key] = marker['id']
        details[category][key] = {
            'markerId': marker['id'],
            'sourceId': marker.get('source_id'),
            'name': marker.get('name'),
            'entry': name,
            'position': pos,
            'distance': round(distance, 2),
        }

    seen_categories = set()
    for path in LOCATION_FILES:
        if path not in romfs.index:
            continue
        root = Byaml(romfs.decompress(romfs.read(path))).root()
        for category in root:
            seen_categories.add(category)

        for row in rows(root, 'Shrine'):
            match = re.match(r'Dungeon(\d{3})$', row['name'])
            if not match:
                continue
            index = int(match.group(1))
            x, _, z = row['pos']
            marker, distance = nearest(shrine_markers, x, z)
            if marker is None:
                unmatched.append({'category': 'shrines', 'entry': row['name'], 'pos': row['pos']})
                continue
            add('shrines_found', hash_key('IsVisitLocation.Dungeon%03d' % index), marker, 'shrine', row['name'], row['pos'], distance)
            add('shrines_status', hash_key('DungeonState.Dungeon%03d' % index), marker, 'shrine', row['name'], row['pos'], distance)

        for row in rows(root, 'Tower'):
            match = re.match(r'Tower(\d{2})$', row['name'])
            if not match:
                continue
            index = int(match.group(1))
            marker, distance = nearest(tower_markers, row['pos'][0], row['pos'][2])
            if marker:
                add('towers', hash_key('IsVisitLocation.Tower%02d' % index), marker, 'tower', row['name'], row['pos'], distance)
            else:
                unmatched.append({'category': 'towers', 'entry': row['name'], 'pos': row['pos']})

        for row in rows(root, 'CheckPoint'):
            match = re.match(r'CheckPoint(\d{3})$', row['name'])
            if not match:
                continue
            index = int(match.group(1))
            marker, distance = nearest(root_markers, row['pos'][0], row['pos'][2])
            if marker:
                add('lightroots', hash_key('IsVisitLocation.CheckPoint%03d' % index), marker, 'lightroot', row['name'], row['pos'], distance)
            else:
                unmatched.append({'category': 'lightroots', 'entry': row['name'], 'pos': row['pos']})

        for row in rows(root, 'DragonTears'):
            match = re.match(r'DragonTears(\d{2})$', row['name'])
            if not match:
                continue
            index = int(match.group(1))
            marker, distance = nearest(tear_markers, row['pos'][0], row['pos'][2])
            if marker:
                add('dragon_tears', hash_key('IsVisitLocation.DragonTears%02d' % index), marker, 'tear', row['name'], row['pos'], distance)
            else:
                unmatched.append({'category': 'dragon_tears', 'entry': row['name'], 'pos': row['pos']})

        for category, target, marker_pool in (('CaveEntranceNormal', 'caves', cave_markers),
                                              ('CaveEntranceSpecial', 'chasms', chasm_markers),
                                              ('CaveEntranceWell', 'wells', well_markers)):
            for row in rows(root, category):
                marker, distance = nearest(marker_pool, row['pos'][0], row['pos'][2])
                keys = [hash_key('IsVisitLocation.' + row['name'])]
                for instance in row['ids']:
                    if instance:
                        keys.append(hash_key('IsVisitLocationArea_CaveEntrance.' + instance))
                if marker:
                    for key in keys:
                        add(target, key, marker, target, row['name'], row['pos'], distance)
                else:
                    unmatched.append({'category': target, 'entry': row['name'], 'instances': row['ids'], 'keys': keys, 'pos': row['pos']})

    report = {
        'schema': 'macnativegames.save-map/1',
        'game': 'totk',
        'source': 'RomFS Banc/*/LocationArea/*.locationarea.byml.zs + wiki map-content.json',
        'counts': {name: len(values) for name, values in categories.items()},
        'wikiMarkers': {
            'shrines': len(shrine_markers), 'towers': len(tower_markers), 'lightroots': len(root_markers),
            'dragon_tears': len(tear_markers), 'caves': len(cave_markers), 'chasms': len(chasm_markers),
            'wells': len(well_markers),
        },
        'locationCategories': sorted(seen_categories),
        'unmatched': unmatched,
        'categories': categories,
        'details': details,
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps({k: report['counts'][k] for k in categories}, indent=1))
    print('unmatched:', len(unmatched), '->', out_path)


if __name__ == '__main__':
    main()
