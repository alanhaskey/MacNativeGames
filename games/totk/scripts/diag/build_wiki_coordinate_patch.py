#!/usr/bin/env python3
"""Build a wiki coordinate correction patch from the authoritative RomFS placement positions.

Inputs:
  - TOTK_build/analysis/save-map/wiki-coordinate-sync.json   (game xyz + wiki x/z + match bucket)
  - TOTK_build/analysis/save-map/save-marker-map.json        (authoritative RomFS positions, names, keys)
  - "<Zelda Wiki>/data/content/map-content.json"             (current wiki markers, read-only)

Outputs (TOTK_build/analysis/save-map/):
  - wiki-coordinate-patch.json     matched markers: before -> after (game_x, game_y, game_z, layer)
  - wiki-coordinate-patch.ndjson   one patch record per line (easy to stream/apply)
  - wiki-coordinate-conflicts.json ambiguous / unmatched / duplicate mappings for manual review
  - wiki-coordinate-patch-summary.md short human summary

Coordinate convention (wiki): game_x = RomFS X, game_y = RomFS Y, game_z = -RomFS Z.
`after` always uses the precise placement coordinates; `before` is what the wiki marker currently stores.
"""
from __future__ import annotations

import collections
import json
import math
import statistics
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
ANALYSIS = ROOT / "TOTK_build/analysis/save-map"
SYNC_PATH = ANALYSIS / "wiki-coordinate-sync.json"
FULL_PATH = ANALYSIS / "save-marker-map.json"
WIKI_PATH = Path("/Users/yunnn/CodeProject/Zelda Wiki/data/content/map-content.json")

PATCH_PATH = ANALYSIS / "wiki-coordinate-patch.json"
NDJSON_PATH = ANALYSIS / "wiki-coordinate-patch.ndjson"
CONFLICTS_PATH = ANALYSIS / "wiki-coordinate-conflicts.json"
SUMMARY_PATH = ANALYSIS / "wiki-coordinate-patch-summary.md"

MATCH_TOLERANCE = 1.5  # meters, wiki stores roughly integer game coordinates


def layer_of(y: float) -> int:
    return -1 if y < -500 else (1 if y > 1300 else 0)


def marker_layer(m: dict) -> int | None:
    value = m.get("layer")
    return value if isinstance(value, int) else None


def main() -> int:
    sync = json.loads(SYNC_PATH.read_text())["entries"]
    full = json.loads(FULL_PATH.read_text())
    wiki_markers = json.loads(WIKI_PATH.read_text())["markers"]

    index: dict[tuple[int, int], list[dict]] = collections.defaultdict(list)
    for marker in wiki_markers:
        if marker.get("game_x") is None or marker.get("game_z") is None:
            continue
        index[(round(marker["game_x"]), round(marker["game_z"]))].append(marker)

    matched: list[tuple[dict, dict]] = []
    ambiguous: list[dict] = []
    unmatched: list[dict] = []
    for entry in sync:
        wiki_x, wiki_z = entry["wiki"]["game_x"], entry["wiki"]["game_z"]
        candidates: list[dict] = []
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for marker in index.get((round(wiki_x) + dx, round(wiki_z) + dz), []):
                    if abs(marker["game_x"] - wiki_x) <= MATCH_TOLERANCE and abs(marker["game_z"] - wiki_z) <= MATCH_TOLERANCE:
                        candidates.append(marker)
        target_layer = layer_of(entry["game"]["y"])
        candidates = [m for m in candidates if marker_layer(m) in (None, target_layer)]
        if len(candidates) == 1:
            matched.append((entry, candidates[0]))
        elif len(candidates) > 1:
            ambiguous.append({
                "sourceId": entry["sourceId"],
                "category": entry["category"],
                "name": entry.get("name"),
                "game": entry["game"],
                "candidates": [
                    {"id": m.get("id"), "name": m.get("name"), "category": m.get("category"),
                     "layer": m.get("layer"), "game_x": m.get("game_x"), "game_y": m.get("game_y"),
                     "game_z": m.get("game_z")}
                    for m in candidates
                ],
            })
        else:
            unmatched.append({
                "sourceId": entry["sourceId"],
                "category": entry["category"],
                "name": entry.get("name"),
                "game": entry["game"],
                "keys": entry.get("keys", []),
                "entries": entry.get("entries", []),
            })

    # Deduplicate by wiki marker id (shrines appear twice: found + status; keep one, merge categories).
    by_marker: dict[str, dict] = {}
    for entry, marker in matched:
        marker_id = marker["id"]
        record = {
            "markerId": marker_id,
            "sourceId": entry["sourceId"],
            "category": entry["category"],
            "name": marker.get("name") or entry.get("name"),
            "before": {
                "game_x": marker.get("game_x"),
                "game_y": marker.get("game_y"),
                "game_z": marker.get("game_z"),
                "layer": marker.get("layer"),
            },
            "after": {
                "game_x": round(entry["game"]["x"], 3),
                "game_y": round(entry["game"]["y"], 3),
                "game_z": round(entry["game"]["z"], 3),
                "layer": layer_of(entry["game"]["y"]),
            },
            "deltaMeters": entry.get("deltaMeters"),
            "sync": entry.get("sync"),
            "keys": entry.get("keys", []),
            "entries": entry.get("entries", []),
        }
        previous = by_marker.get(marker_id)
        if previous is None:
            by_marker[marker_id] = record
        else:
            previous["categories"] = sorted(set(previous.get("categories", [previous["category"]]) + [record["category"]]))
            # keep the record with the smaller meter delta as the primary coordinates
            if (record.get("deltaMeters") or 1e9) < (previous.get("deltaMeters") or 1e9):
                merged_categories = previous["categories"]
                by_marker[marker_id] = record
                by_marker[marker_id]["categories"] = merged_categories

    patches = sorted(by_marker.values(), key=lambda r: (r.get("category", ""), r["markerId"]))
    y_filled = sum(1 for r in patches if r["before"].get("game_y") is None)
    def _changed(record: dict) -> bool:
        before = record["before"]
        after = record["after"]
        if before.get("game_y") is None:
            return True
        for key in ("game_x", "game_y", "game_z"):
            old = before.get(key)
            if old is not None and abs(old - after[key]) > 0.51:
                return True
        return False

    changed_xyz = sum(1 for r in patches if _changed(r))

    stats_by_category: dict[str, dict] = {}
    for r in patches:
        cat = r.get("category", "?")
        if cat.startswith("shrines") or any(c.startswith("shrines") for c in r.get("categories", [])):
            cat = "shrines"
        bucket = stats_by_category.setdefault(cat, {"count": 0, "yMissing": 0, "dy": [], "dxz": []})
        bucket["count"] += 1
        if r["before"].get("game_y") is None:
            bucket["yMissing"] += 1
        else:
            bucket["dy"].append(r["after"]["game_y"] - r["before"]["game_y"])
        bucket["dxz"].append(max(abs(r["after"]["game_x"] - (r["before"].get("game_x") or 0)),
                                abs(r["after"]["game_z"] - (r["before"].get("game_z") or 0))))
    summary_rows = []
    for cat in sorted(stats_by_category):
        b = stats_by_category[cat]
        dy = [v for v in b["dy"] if v == v]
        summary_rows.append({
            "category": cat,
            "count": b["count"],
            "yMissing": b["yMissing"],
            "dyMedian": round(statistics.median(dy), 2) if dy else None,
            "dyMin": round(min(dy), 2) if dy else None,
            "dyMax": round(max(dy), 2) if dy else None,
            "dxzMax": round(max(b["dxz"]), 2),
        })

    patch_doc = {
        "schema": "macnativegames.wiki-coordinate-patch/1",
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source": {
            "positions": "TOTK_build/analysis/save-map/save-marker-map.json (RomFS placement Translate)",
            "pairing": "TOTK_build/analysis/save-map/wiki-coordinate-sync.json",
            "wiki": str(WIKI_PATH),
        },
        "convention": "game_x = RomFS X, game_y = RomFS Y, game_z = -RomFS Z",
        "stats": {
            "syncEntries": len(sync),
            "matchedUniqueMarkers": len(patches),
            "ambiguous": len(ambiguous),
            "unmatched": len(unmatched),
            "gameYFilled": y_filled,
            "recordsChanged": changed_xyz,
            "byCategory": summary_rows,
        },
        "patches": patches,
    }
    PATCH_PATH.write_text(json.dumps(patch_doc, ensure_ascii=False, indent=1))
    NDJSON_PATH.write_text("\n".join(json.dumps(r, ensure_ascii=False) for r in patches) + "\n")
    CONFLICTS_PATH.write_text(json.dumps({
        "schema": "macnativegames.wiki-coordinate-conflicts/1",
        "generatedAt": patch_doc["generatedAt"],
        "ambiguous": ambiguous,
        "unmatched": unmatched,
    }, ensure_ascii=False, indent=1))

    lines = [
        "# Wiki 坐标纠正补丁摘要",
        "",
        f"- 生成时间：{patch_doc['generatedAt']}",
        f"- 同步条目：{len(sync)}；唯一匹配 wiki 标记：**{len(patches)}**；"
        f"歧义：{len(ambiguous)}；未匹配：{len(unmatched)}",
        f"- 补 `game_y` 的记录：{y_filled}；坐标实际变化（>0.5m）：{changed_xyz}",
        "",
        "| 类别 | 条数 | y 缺失 | dY 中位 | dY 范围 | dXZ 最大 |",
        "|---|---|---|---|---|---|",
    ]
    for row in summary_rows:
        lines.append(
            f"| {row['category']} | {row['count']} | {row['yMissing']} | {row['dyMedian']} | "
            f"{row['dyMin']} ~ {row['dyMax']} | {row['dxzMax']} |"
        )
    lines += [
        "",
        "字段说明：`before` 是 wiki 当前存的值，`after` 是 RomFS 放置坐标（权威）。",
        "应用方式：按 `markerId`（wiki marker.id）更新 `game_x/game_y/game_z/layer`，"
        "`keys`/`entries` 可用于人工核对。",
    ]
    SUMMARY_PATH.write_text("\n".join(lines) + "\n")

    print(f"patches={len(patches)} ambiguous={len(ambiguous)} unmatched={len(unmatched)} "
          f"yFilled={y_filled} changed={changed_xyz}")
    print(f"written: {PATCH_PATH.name}, {NDJSON_PATH.name}, {CONFLICTS_PATH.name}, {SUMMARY_PATH.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
