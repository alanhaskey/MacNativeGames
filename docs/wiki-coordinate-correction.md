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
| 对照/QA | `TOTK_build/analysis/save-map/wiki-coordinate-sync.json` | 2974 条：我们的 `game{x,y,z}`、wiki 当前 `game_x/game_z`、`deltaMeters`、`sync`（exact/close/review）、`keys/entries` | 查看每条与 wiki 的差距，人工复核 |
| **纠正补丁** | `TOTK_build/analysis/save-map/wiki-coordinate-patch.json`（+`.ndjson`） | 2506 个 wiki 标记的 `before → after`（`game_x/game_y/game_z/layer`） | 直接给 wiki 打补丁 |

坐标约定（补丁里 `after` 已转换成 wiki 口径）：

```
game_x = RomFS X
game_y = RomFS Y
game_z = -RomFS Z
```

> 注意：`save-marker-map.json` 里存的是 RomFS 原始坐标（z 为负）；补丁已经翻转，wiki 直接使用 `after` 即可。

---

## 2. 补丁文件结构

`wiki-coordinate-patch.json`：

```json
{
  "schema": "macnativegames.wiki-coordinate-patch/1",
  "generatedAt": "2026-10-22T08:50:23Z",
  "convention": "game_x = RomFS X, game_y = RomFS Y, game_z = -RomFS Z",
  "stats": { "syncEntries": 2974, "matchedUniqueMarkers": 2506,
             "ambiguous": 21, "unmatched": 251, "gameYFilled": 11,
             "recordsChanged": 2004, "byCategory": [ ... ] },
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

- `markerId` 用的是 wiki `marker.id`；`sourceId` 是映射表的来源 ID；
- 神庙的"发现/完成"是同一条 wiki 标记，补丁里合并成一条，`categories: ["shrines_found","shrines_status"]`；
- `wiki-coordinate-patch.ndjson`：每行一个 patch，便于流式导入；
- `wiki-coordinate-conflicts.json`：`ambiguous`（21 条，多个候选）+ `unmatched`（251 条，wiki 没有对应标记或坐标差太远）。

### 应用方式（wiki 侧）

按 `markerId` 更新：`game_x = after.game_x`、`game_y = after.game_y`、`game_z = after.game_z`、
`layer = after.layer`。建议：

1. 先只应用 `sync == "exact"` 的记录（2743 条中的绝大多数）；
2. `close`（224 条）逐条复核后再应用（`deltaMeters` 已给出）；
3. `review`（7 条）与 `ambiguous`/`unmatched` 走人工；
4. 应用前备份 `map-content.json`。

---

## 3. 本次发现的坐标问题（值得注意）

| 类别 | 条数 | dY 中位 | dY 范围 | dXZ 最大 | 说明 |
|---|---|---|---|---|---|
| addison 卡邦达 | 81 | **+105.0** | 105.0 ~ 105.0 | 0.0 | wiki y 系统性低 105m |
| bubbuls 魔犹伊 | 146 | **+119.0** | 118.5 ~ 119.5 | 0.5 | wiki 用了不同节点的 y |
| koroks 克洛格 | 771 | +95.7 | 91.3 ~ **996.8** | 1.0 | wiki y 不一致（部分差近 1km） |
| chests 宝箱 | 1050 | +105.0 | -56.9 ~ 143.4 | **44.2** | x/z 也有少量明显偏差 |
| shrines 神庙 | 78（合并 134） | 0.0 | ~0 | 4.9 | 主要是 x/z 小偏差，另 4 条 y 缺失 |
| lightroots 树根 | 113 | 0.0 | -830.0 ~ 0.0 | 1.2 | 少数 y 严重错误 |
| caves/chasms/wells/towers/dragon_tears | 145/31/58/15/12 | ~0 | ~0 | 0~2.2 | 基本准确，补齐 y 即可 |
| sage_wills 贤者遗志 | 6 | — | — | 13.6 | 6 条 y 缺失 |

> 结论：**多数标记 x/z 只是厘米级偏差，真正的问题是 y（第三轴）**；卡邦达/魔犹伊/克洛格/宝箱有系统性或较大偏差。

## 4. 未匹配 / 歧义（需要人工）

`wiki-coordinate-conflicts.json`：

- `ambiguous` 21 条（宝箱 10、克洛格 6、深穴 4、魔犹伊 1）：坐标容差内出现多个 wiki 标记，需人工选；
- `unmatched` 251 条：宝箱 172、克洛格 22、**神庙 18**、**贤者遗志 14**、树根 7 等。
  这些是"游戏里有、wiki 没有对应标记（或坐标差太远）"的对象，可用来补建 wiki 标记；
  每条带 `game{x,y,z}`、`keys`、`entries`，可直接对照。

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
