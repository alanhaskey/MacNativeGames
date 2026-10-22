#!/usr/bin/env python3
"""Build the save-key -> wiki-marker mapping for the collection sync.

Reads the extracted TOTK RomFS (Banc/*/LocationArea/*.locationarea.byml.zs), derives the save hash
keys for shrines, towers, lightroots, dragon tears, caves, chasms and wells, and joins them to the
wiki markers (data/content/map-content.json) by coordinates.  The wiki stores game_z negated
relative to the RomFS/save Z.

Usage: build_save_marker_map.py <romfs-dir> <wiki-map-content.json> <out.json> [reference-hashes.js]
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


ACTOR_UNIT_SUFFIXES = ('_Static.bcett.byml.zs', '_Dynamic.bcett.byml.zs')
ACTOR_CATEGORIES = (
    # (category, predicate, wiki subtype, key kind)
    ('chests', lambda name: name.startswith('TBox_Field') or name.startswith('TBox_Dungeon'), '宝箱', 'guid'),
    ('bubbuls', lambda name: name in ('Enemy_CaveMaster_000', 'Enemy_CaveMaster_Middle', 'Enemy_CaveMaster_Senior'), '魔犹伊', 'guid'),
    ('addison', lambda name: name == 'Npc_CraftSignboard001', '卡邦达', 'guid'),
)


def scan_actors(romfs):
    """Yield (guid, gyaml, (x, y, z), unit path) for every actor in the map unit files."""
    paths = [p for p in romfs.index if p.startswith('Banc/') and p.endswith(ACTOR_UNIT_SUFFIXES)]
    for path in paths:
        try:
            root = Byaml(romfs.decompress(romfs.read(path))).root()
        except Exception:
            continue
        for actor in (root.get('Actors') or []):
            guid = actor.get('Hash')
            name = (actor.get('Gyaml') or '').rstrip('\x00')
            translate = actor.get('Translate')
            if not isinstance(guid, int) or not isinstance(translate, list) or len(translate) != 3:
                continue
            yield guid, name, translate, path


def guid_key(value):
    return '0x%016X' % value


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

    # Koroks and travel koroks: the save keys are hash('IsAppearKorok.' + InstanceID) and
    # hash('KorokCarryProgress.' + InstanceID); the placement file gives InstanceID -> coordinates.
    korok_location = 'Banc/MainField/HiddenKorok/MainField.hiddenkorok.byml.zs'
    if korok_location in romfs.index:
        korok_root = Byaml(romfs.decompress(romfs.read(korok_location))).root()
        # Hidden koroks: coordinates match the wiki markers exactly (z negated on the wiki side).
        categories['koroks'] = {}
        details['koroks'] = {}
        for instance, position in (korok_root.get('HiddenKorok') or {}).items():
            marker, distance = nearest(by_subtype('隐藏克洛格'), position[0], position[2])
            if marker is None:
                unmatched.append({'category': 'koroks', 'instance': instance, 'pos': position})
                continue
            add('koroks', hash_key('IsAppearKorok.' + instance), marker, 'korok', instance, position, distance)
        # Travel koroks: the wiki markers sit at the delivery point, not the carry start, so pair by order
        # (the placement dict order equals the game's internal order; the wiki numbers Korok0000..Korok0099).
        travel_markers = sorted(by_subtype('旅行克洛格'),
                                key=lambda m: int((m.get('source_id') or 'Korok0').split('Korok')[-1]))
        categories['travel_koroks'] = {}
        details['travel_koroks'] = {}
        for index, (instance, position) in enumerate((korok_root.get('CarryKorok') or {}).items()):
            if index >= len(travel_markers):
                unmatched.append({'category': 'travel_koroks', 'instance': instance, 'pos': position})
                continue
            marker = travel_markers[index]
            add('travel_koroks', hash_key('KorokCarryProgress.' + instance), marker, 'travel_korok',
                instance, position, -1.0)

    # Sage wills: the save stores GUIDs (decimal strings in the reference table).
    # Optional reference table (community project, used only for the "which chest GUIDs hold a sage's will"
    # list). Pass it as the 4th argument; without it the sage_wills category is skipped.
    will_path = Path(sys.argv[4]) if len(sys.argv) > 4 else Path('/tmp/totkmap-ref/app/data/totk_save_hashes.js')
    will_guids = set()
    if will_path.exists():
        import re as _re
        match = _re.search(r'SAGE_WILLS_FOUND:\s*\[(.*?)\n\t\]', will_path.read_text(), _re.S)
        if match:
            will_guids = {int(x) for x in _re.findall(r"'([0-9]{10,})'", match.group(1))}

    # GUID-keyed actor categories (chests, bubbuls, Addison signs): the save stores the actor
    # instance GUID in its GUID array; the placement files carry Hash -> Translate.
    actor_lists = {category: [] for category, _, _, _ in ACTOR_CATEGORIES}
    for guid, name, position, unit in scan_actors(romfs):
        for category, predicate, _, _ in ACTOR_CATEGORIES:
            if predicate(name):
                actor_lists[category].append((guid, name, position, unit))
    for category, predicate, subtype, key_kind in ACTOR_CATEGORIES:
        pool = by_subtype(subtype)
        categories[category] = {}
        details[category] = {}
        for guid, name, position, unit in actor_lists[category]:
            marker, distance = nearest(pool, position[0], position[2])
            if marker is None:
                unmatched.append({'category': category, 'entry': name, 'pos': position, 'unit': unit})
                continue
            add(category, guid_key(guid), marker, category, name, position, distance)

    if will_guids:
        pool = by_subtype('贤者的遗志')
        categories['sage_wills'] = {}
        details['sage_wills'] = {}
        for guid, name, position, unit in scan_actors(romfs):
            if guid not in will_guids:
                continue
            marker, distance = nearest(pool, position[0], position[2])
            if marker is None:
                unmatched.append({'category': 'sage_wills', 'entry': name, 'pos': position, 'unit': unit})
                continue
            add('sage_wills', guid_key(guid), marker, 'sage_will', name, position, distance)

    # How a consumer decides "done" for each category.
    expectations = {
        'shrines_found': {'kind': 'flag', 'value': 1},
        'shrines_status': {'kind': 'flag', 'value': 'Clear'},
        'towers': {'kind': 'flag', 'value': 1},
        'lightroots': {'kind': 'flag', 'value': 1},
        'dragon_tears': {'kind': 'flag', 'value': 1},
        'caves': {'kind': 'flag', 'value': 1},
        'chasms': {'kind': 'flag', 'value': 1},
        'wells': {'kind': 'flag', 'value': 1},
        'koroks': {'kind': 'flag', 'value': 1},
        'travel_koroks': {'kind': 'flag', 'value': 'Clear'},
        'chests': {'kind': 'guid'},
        'bubbuls': {'kind': 'guid'},
        'addison': {'kind': 'guid'},
        'sage_wills': {'kind': 'guid'},
    }

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
    compact = {
        'schema': report['schema'],
        'game': report['game'],
        'source': report['source'],
        'counts': report['counts'],
        'categories': {name: {'kind': expectations.get(name, {}).get('kind', 'flag'),
                              'value': expectations.get(name, {}).get('value', 1),
                              'valueHash': (1 if expectations.get(name, {}).get('value', 1) == 1
                                            else murmur3_32(expectations.get(name, {}).get('value', 'Clear'))),
                              'map': values}
                       for name, values in categories.items()},
    }
    compact_path = out_path.with_name(out_path.stem + '.compact.json')
    compact_path.write_text(json.dumps(compact, ensure_ascii=False, separators=(',', ':')) + '\n')
    print('compact:', compact_path, compact_path.stat().st_size, 'bytes')
    print(json.dumps({k: report['counts'][k] for k in categories}, indent=1))
    print('unmatched:', len(unmatched), '->', out_path)


if __name__ == '__main__':
    main()
