# 存档映射规则（已破解部分，2026-10-21）

> 配套：`docs/collection-sync-research.md`、`docs/save-event-observations.json`。
> 参考项目：[yalincc/totkmap](https://github.com/yalincc/totkmap)（无 LICENSE，仅作验证参考）。

## 1. 基础

- 存档 hash 键 = **murmurhash3 x86_32(名字，UTF-8，seed 0)**；`hash('Complete') = 0x4C0A63F4`。
- `progress.sav`：`0x28` 起每 8 字节 `[u32 hash][u32 value]`；遇到 hash `0xA3DB7114` 停止，
  该条目的 **value = GUID 数组偏移**（本机 = `0xD9510`）。
- GUID 数组：从该偏移起每 8 字节一个小端 u64，直到全 0；格式化成 `0x%016llX`。
- 游戏内时间：`0x449B3BA6` = 小时（0–23），`0x2106FA19` = 分钟（0–59）。

## 2. 已完成类别公式（全部实测/交叉验证）

| 类别 | 名字公式 | 判定值 | 验证 |
|---|---|---|---|
| 神庙发现 | `IsVisitLocation.Dungeon%03d`（000–151） | 1 | 152/152 命中，两座发现的索引 60/62 |
| 神庙状态 | `DungeonState.Dungeon%03d` | `hash('Clear')` | 152/152 命中，本机 Clear=2 |
| 破魔之根 | `IsVisitLocation.CheckPoint%03d` | 1 | 120/120 命中 |
| 鸟望台 | `IsVisitLocation.Tower%02d` | 1 | 15/15 命中 |
| 龙之泪 | `IsVisitLocation.DragonTears%02d`（01–12） | 1 | 12/12 命中 |
| 洞窟入口 | `IsVisitLocationArea_CaveEntrance.<十进制 64 位 GUID>` | 1 | 池边洞窟 GUID `0x2313be6fbca721a3` → `0x0E76344B` 与实测一致 |
| 克洛格（隐藏） | 用 `KOROKS_HIDDEN` 列表的**顺序**：第 i 个 = wiki `hidden-korok:Korok(i+100)`? | 1 | 两次实测：index 526→Korok0626 (246,-822)；index 213→Korok0313 (113,-1081)，与采集点一致（wiki 的 z 与存档 z 符号相反） |

待定：井（`IsVisitLocation.Well%04d` 不匹配，可能是 GUID 形式）、深穴（试 `IsVisitLocationArea_ChasmEntrance.<dec guid>`）、
宝箱、旅行克洛格（`KOROKS_CARRY` 100 条，顺序应同 hidden）、卡邦达、依盖队设计图、贤者遗志。

## 3. RomFS 结构（做逐点映射所需）

- 解包后的 `extracted/v143/romfs/`：`index.json` 记录每个文件的 `{path, offset, size}`，数据在 `romfs.bin`。
- `.zs` = zstd 帧 + 专用字典：
  - 字典包：`Pack/ZsDic.pack.zs` →（用 zstd 默认参数解压）→ SARC：
    `zs.zsdic`（ID 1）、`bcett.byml.zsdic`（ID 2）、`pack.zsdic`（ID 3）；
  - 解压工具：`python zstandard`，`ZstdCompressionDict(data)` + `ZstdDecompressor(dict_data=...)`（用 auto 模式）。
  - 注意 SARC 的 data offset 直接用头里的 `u32@0x0C`（不要再加 0x14）。
- `.bkres.zs`（`Bake/Scene/*.bkres.zs`）→ SARC → 单文件 `bkdat.byaml`（BYAML v4）。
- BYAML v4（格式规范：https://epd.zeldamods.org/wiki/Byaml ）：
  - 头：`BY`(u16 BE) + version u16(4) + u32 key 表偏移 + u32 字符串表偏移 + u32 root 偏移；
  - 容器：u8 类型 + u24 数量；类型：0xC0 数组、0xC1 字典、0xC2 字符串表、0x20–0x2f hash 数组、
    0xD0 bool、0xD1 s32、0xD2 f32、0xD3 u32、0xD4/0xD5/0xD6 指向 64 位值、0xFF null。
- 场景按网格切分：`Bake/Scene/MainField_G1_<x>_<y>.bkres.zs`（地表 5149 个）、`MainField_U*`（地下 5133 个）、
  `Dungeon000..151`（神庙内部 152 个）、`MainField_LDT/LDF`、`OpeningField`。
- **下一步**：解析 `MainField_G*` 的 `bkdat.byaml`，得到地图对象的 `{GUID, Name/UnitConfigName, Translate 坐标}`
  列表，然后：
  - 宝箱：存档 GUID → 地图对象 GUID → 坐标 → wiki 宝箱标记；
  - 神庙：地图对象里的 Dungeon 索引 → 坐标/GUID → wiki 神庙标记（或直接把 index→GUID 建表）；
  - 井/深穴/旅行克洛格：同样用 GUID→坐标。

## 3.5 已生成的映射表（最终版）

工具：`games/totk/scripts/diag/romfs_map_objects.py`（RomFS + zstd 字典 + BYAML v4/v7 解析）、
`games/totk/scripts/diag/build_save_marker_map.py`（生成映射）。

产物：

- `TOTK_build/analysis/save-map/save-marker-map.json`（1.4 MB，含名字/坐标/距离，调试用）
- `TOTK_build/analysis/save-map/save-marker-map.compact.json`（169 KB，`categories: {类别: {存档key: wiki markerId}}`，给 wiki 用）

| 类别 | 数量 | key 形式 | 验证 |
|---|---|---|---|
| 神庙发现 | 152 | hash `IsVisitLocation.Dungeon%03d` | `0x5F6180D7`→乌克乌候、`0xDF539D50`→伊恩伊萨（距离 0.0） |
| 神庙状态 | 152 | hash `DungeonState.Dungeon%03d` | 值 = hash('Clear') |
| 鸟望台 | 15 | hash `IsVisitLocation.Tower%02d` | 坐标匹配 |
| 破魔之根 | 120 | hash `IsVisitLocation.CheckPoint%03d` | 坐标匹配 |
| 龙之泪 | 12 | hash `IsVisitLocation.DragonTears%02d` | 坐标匹配 |
| 洞窟入口 | 342（171×2） | `IsVisitLocation.<名>` + `IsVisitLocationArea_CaveEntrance.<ID>` | `0xE713E9E7`/`0x0E76344B`→池边的洞窟 |
| 深穴 | 70（35×2） | 同上（`CaveEntranceSpecial`/`DeepHole_*`） | 坐标匹配 |
| 井 | 116（58×2） | 同上（`CaveEntranceWell`/`Well_*`） | 坐标匹配 |
| 隐藏克洛格 | 800 | hash `IsAppearKorok.<InstanceID>` | 800/800 命中；`0x3BAFD364`→Korok0626（实测采集点） |
| 旅行克洛格 | 100 | hash `KorokCarryProgress.<InstanceID>` | 顺序映射（wiki 坐标是交付点，不用于匹配） |
| 宝箱 | 1344 | **GUID**（`Hash`，`0x%016X`） | `0xE10681257353F379`→`chest:Treasure0080`（蛋白石）、`0x240D81DDB41B4665`→`Treasure0081`（木箭×5），距离 0.0 |
| 魔犹伊 | 147 | GUID | `0x1C2A653E0D7517D5`→`bubbulfrog:sky:3`，距离 0.37 |
| 卡邦达 | 81 | GUID（`Npc_CraftSignboard001`） | 81/81 坐标匹配 |
| 贤者遗志 | 20 | GUID（**装遗志的宝箱 GUID**） | 20/20 坐标匹配 |

数据来源：`Banc/*/LocationArea/*.locationarea.byml.zs`（地点）、
`Banc/MainField/HiddenKorok/MainField.hiddenkorok.byml.zs`（克洛格 InstanceID→坐标）、
`Banc/**/*_(Static|Dynamic).bcett.byml.zs` 的 `Actors[].Hash/Translate`（GUID 类对象）。

未匹配：宝箱 228（多为无 wiki 标记的神庙/大迷宫内部宝箱）、井/深穴/神庙各 1 条边界情况。

## 3.6 尚未覆盖 / 待办

- **依盖队设计图（34）**：存档是 32 位 hash 旗帜（`Tank`/`Fanboat` 等设计图**类型**），不是逐对象 GUID；
  需要设计图类型 ↔ wiki 标记的映射表（objmap 的 `yiga-schematic` 数据）。
- **左纳乌装置制造机（30）**：存档旗帜名 `RecycleBoxData.<InstanceID>.IsVisit`，需要实例 ID 的坐标来源
  （当前 Actors 扫描未发现 RecycleBox 放置，可能在 `Bake/Scene`）。
- **残旧的地图（31）**：存的是地图物品类型（hash 旗帜），逐点位置需要单独映射。
- **支线与挑战**：用 `hash('Step_'+任务key) == hash('Complete')`，需要 wiki 侧任务 key 表（游戏侧只提供 flags）。
- 旅行克洛格是顺序映射（未用游戏内事件逐条验证），使用时建议标注置信度。

## 4. 已实现接口（游戏侧）

- `GET /v1/save/state?slot=latest`：hash 表（非 0 项）+ GUID 数组 + 坐标 + 游戏内时间；
- `GET /v1/save/delta?since=<seq>`：相邻存档增量（added/changed/removed flags、newGuids、intervalMs）。
- 实测：state ~122KB（5087 非零项 + 119 GUID），delta 捕获 10 个变化；类别统计与实机一致。
