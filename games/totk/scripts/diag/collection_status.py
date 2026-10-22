#!/usr/bin/env python3
"""Reference consumer for the collection sync: gateway state + save-marker map -> completed markers.

Usage:
  collection_status.py --map TOTK_build/analysis/save-map/save-marker-map.compact.json \
      [--url http://127.0.0.1:17862 | --state state.json] [--json]

Reads GET /v1/save/state (or a saved copy of that JSON), applies the compact mapping and prints the
completed wiki markers per category. Intended as a template for the wiki side; read-only.
"""
import argparse
import json
import struct
import sys
import urllib.request
from pathlib import Path


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


def load_state(url, state_path):
    if state_path:
        return json.loads(Path(state_path).read_text())
    with urllib.request.urlopen(url.rstrip('/') + '/v1/save/state?slot=latest', timeout=10) as response:
        return json.loads(response.read())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--map', required=True)
    parser.add_argument('--url', default='http://127.0.0.1:17862')
    parser.add_argument('--state')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()

    state = load_state(args.url, args.state)
    if not state.get('ok'):
        sys.exit(f"gateway state not ok: {state.get('reason')}")
    mapping = json.loads(Path(args.map).read_text())
    flags = {int(key, 16): value for key, value in state['flags']}
    guids = {int(value, 16) for value in state['guids']}
    clear = murmur3_32('Clear')

    result = {'savedAtMs': state.get('savedAtMs'), 'position': state.get('position'),
              'gameTime': state.get('gameTime'), 'categories': {}}
    for category, info in mapping['categories'].items():
        done = {}
        for key, marker in info['map'].items():
            if info['kind'] == 'guid':
                if int(key, 16) in guids:
                    done[key] = marker
            else:
                wanted = info.get('valueHash')
                if wanted is None:
                    wanted = clear if info.get('value') == 'Clear' else info.get('value', 1)
                if flags.get(int(key, 16), 0) == wanted:
                    done[key] = marker
        result['categories'][category] = {
            'done': len(set(done.values())),
            'total': len(set(info['map'].values())),
            'markers': done,
        }
    if args.json:
        print(json.dumps(result, indent=2, ensure_ascii=False))
    else:
        for category, row in result['categories'].items():
            print(f"{category:16s} {row['done']:4d} / {row['total']}")
        print('position:', state.get('position'), 'gameTime:', state.get('gameTime'))


if __name__ == '__main__':
    main()
