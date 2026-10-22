# Wiki 坐标纠正（含第三轴 Y）

- 更新：2026-10-22
- 用途：把 wiki 上 `map-content.json` 的标记坐标纠正为**游戏 RomFS 放置坐标**，并补上 `game_y`（第三轴）
- 生成器：`games/totk/scripts/diag/build_wiki_coordinate_patch.py`

---

## 1. 哪个 JSON 是"最准确的坐标"

三个文件分工不同，不要混用：

| 文件 | 位置 | 内容 | 用途 |
|---|---|---|---|
| **权威坐标** | `TOTK_build/analysis/save-map/save-marker-map.json` | `details.<category>.<hash>.position` = **RomFS 放置坐标** `[x, y, z_romfs]`（浮点精度），另有 `markerId/name/entry` | 唯一的坐标来源（其余文件都由它派生） |
| 对照/QA | `TOTK_build/analysis/save-map/wiki-coordinate-sync.json` | 2974 条：我们的 `game{x,y,z}`、wiki 当前 `game_x/game_z`、`deltaMeters`、`sync`（exact/close/review）、`keys/entries` | 查看每条与 wiki 的差距、人工复核 |
| **纠正补丁** | `TOTK_build/analysis/save-map/wiki-coordinate-patch.json`（+`.ndjson`） | **2822** 个 wiki 标记的 `before → after`（`game_x/game_y/game_z/layer`） | 直接给 wiki 打补丁 |

坐标约定（补丁里 `after` 已转换成 wiki 口径）：

```
game_x = RomFS X
game_y = RomFS Y
game_z = -RomFS Z
```

> 注意：`save-marker-map.json` 里存的是 RomFS 原始坐标（z 为负）；补丁已经翻转，wiki 直接使用 `after` 即可。

**匹配方式**：我们的 `markerId` 与 wiki 的 `marker.id` 完全同源，补丁按 **ID 精确匹配**（2974/2974 全部命中），
不依赖坐标猜配对——不会出现"把破魔之根配到神庙"这类错配。

---

## 2. 补丁文件结构

`wiki-coordinate-patch.json`：

```json
{
  "schema": "macnativegames.wiki-coordinate-patch/1",
  "generatedAt": "2026-10-22T08:52:26Z",
  "convention": "game_x = RomFS X, game_y = RomFS Y, game_z = -RomFS Z",
  "stats": {
    "syncEntries": 2974,
    "matchedUniqueMarkers": 2822,
    "ambiguous": 0,
    "unmatched": 0,
    "gameYFilled": 32,
    "recordsChanged": 2035,
    "byCategory": [ ... ]
  },
  "patches": [
    {
      "markerId": "gamertw:addison:Sign000",
      "sourceId": "addison:Sign000",
      "category": "addison",
      "name": "卡邦达",
      "before": {"game_x": -4112.93896, "game_y": 25.7178955, "game_z": 2758.5, "layer": 0},
      "after":  {"game_x": -4112.939,   "game_y": 130.718,     "game_z": 2758.5, "layer": 0},
      "deltaMeters": 0.0, "sync": "exact",
      "keys": ["0x70D0AC829AFA29D1"], "entries": ["Npc_CraftSignboard001"],
      "categories": ["addison"]
    }
  ]
}
```

- `markerId` = wiki `marker.id`；`sourceId` 是映射表来源 ID；
- 神庙的"发现/完成"是同一标记，补丁合并为一条，`categories: ["shrines_found","shrines_status"]`；
- 2822 = 2974 条同步记录去掉 152 条神庙 found/status 重复；
- `wiki-coordinate-patch.ndjson`：每行一个 patch，便于流式导入；
- `wiki-coordinate-conflicts.json`：当前为空（`ambiguous: []`、`unmatched: []`），保留结构供将来 ID 缺失时使用。

### 应用方式（wiki 侧）

按 `markerId` 更新：`game_x = after.game_x`、`game_y = after.game_y`、`game_z = after.game_z`、
`layer = after.layer`。建议：

1. 先应用 `sync == "exact"` 的记录（绝大多数）；
2. `close`（若干）逐条复核后再应用（`deltaMeters` 已给出）；
3. `review` 与超大修正（见 §3）建议抽样核对后再批量应用；
4. 应用前备份 `map-content.json`。

---

## 3. 本次纠正的规模（按类别）

| 类别 | 条数 | y 缺失 | dY 中位 | dY 范围 | dXZ 最大 |
|---|---|---|---|---|---|
| addison 卡邦达 | 81 | 0 | **+105.0** | 105.0 ~ 105.0 | 0.0 |
| bubbuls 魔犹伊 | 147 | 0 | **+119.0** | 118.5 ~ 119.5 | 0.5 |
| caves 洞窟 | 146 | 0 | 0.0 | ~0 | 0.0 |
| chasms 深穴 | 35 | 1 | 0.0 | ~0 | **48.8** |
| chests 宝箱 | 1236 | 0 | **+105.0** | -56.9 ~ **+1694.4** | **54.7** |
| dragon_tears 龙之泪 | 12 | 0 | 0.0 | ~0 | 0.0 |
| koroks 克洛格 | 800 | 0 | **+95.8** | 91.3 ~ 135.2 | 0.5 |
| lightroots 树根 | 120 | 1 | 0.0 | ~0 | 0.5 |
| sage_wills 贤者遗志 | 20 | **20** | — | — | 13.6 |
| shrines 神庙 | 152 | 10 | 0.0 | ~0 | 15.5 |
| towers 塔 | 15 | 0 | 0.0 | ~0 | 0.0 |
| wells 井 | 58 | 0 | 0.0 | ~0 | 0.0 |

关键结论：

- **大多数类别的 x/z 已经准确，真正缺的是 y（第三轴）**；本次补 `game_y` 的记录 32 条（wiki 原本为 null）；
- 有系统性 y 偏差：卡邦达 **+105m**、魔犹伊 **+119m**、克洛格 **+95.8m**（原先 y 整体偏低）；
- 少数宝箱/深穴/克洛格存在**大偏差**（宝箱最高 1694m、深穴 48.8m），属于 wiki 数据错误，补丁会修正；
- 若 wiki 端有对"敌人/地点"等原生标记的坐标也来自同一批数据，可一并参考。

## 4. 需要人工复核的项

- `sync` 为 `close` / `review` 的记录（少量）：`deltaMeters` 较大，建议抽查；
- 单项修正超过 200m 的记录（约几十条，主要是宝箱/克洛格/深穴）：建议按 `keys/entries` 核对是哪件物品后再应用；
- `wiki-coordinate-conflicts.json` 目前为空；若将来 `markerId` 对不上，会出现在 `unmatched` 里，可人工补 ID。

## 5. 重新生成

```bash
python3 games/totk/scripts/diag/build_wiki_coordinate_patch.py
```

依赖：

- `TOTK_build/analysis/save-map/save-marker-map.json`（先跑 `build_save_marker_map.py` 生成）
- `TOTK_build/analysis/save-map/wiki-coordinate-sync.json`
- `/Users/yunnn/CodeProject/Zelda Wiki/data/content/map-content.json`（只读）

输出都在 `TOTK_build/analysis/save-map/`：

- `wiki-coordinate-patch.json`
- `wiki-coordinate-patch.ndjson`
- `wiki-coordinate-conflicts.json`
- `wiki-coordinate-patch-summary.md`
