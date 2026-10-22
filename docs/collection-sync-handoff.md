# 收集同步交付清单与接入说明（给 wiki 侧）

> 状态：2026-10-22。游戏侧接口已打包进 App 并实机联调通过；映射表已生成。
> 本文档面向 wiki/连接器侧开发者，说明"有哪些文件、怎么接、注意什么"。
> **最新接口与对接清单请以 [reintegration-guide.md](reintegration-guide.md) 为准**（2026-10-22 更新，含 SSE 即时事件、role 语义、delta 新结构）。

## 1. 架构（一句话）

游戏 App 内自带只读网关，暴露**解析后的存档**（hash 表 flags + GUID 数组 + 坐标 + 游戏内时间）；
wiki 侧用**映射表**把存档 key 翻译成具体标记，标记完成。全程无需内存读取、无需玩家启动任何服务。

```
TOTK.app ──(127.0.0.1:17862, 随游戏启动/退出)──> wiki/连接器
   │  /v1/save/state   hash 表 + GUID + 坐标 + 游戏时间
   │  /v1/save/delta   每次新存档的增量
   └─ save-marker-map.compact.json（静态映射表，随 wiki 发布）
```

## 2. 文件清单

### 2.1 游戏侧（已实现，随 App 发布）

| 文件 | 作用 |
|---|---|
| `games/totk/runtime/gateway.c` | 内置 HTTP/SSE 网关，新增 `/v1/save/state`、`/v1/save/delta` |
| `games/totk/runtime/memscan.c` | 诊断用内存扫描（env `TOTK_MEMSCAN=1` 才启用，正常游戏可忽略）|
| `/Applications/Tears of the Kingdom.app` | 已重打包，包含上述接口 |

### 2.2 映射表（wiki 侧直接使用）

| 文件 | 大小 | 用途 |
|---|---|---|
| `TOTK_build/analysis/save-map/save-marker-map.compact.json` | 169 KB | **正式交付**：`categories: {类别: {kind, value, map}}` |
| `TOTK_build/analysis/save-map/save-marker-map.json` | 1.4 MB | 调试版：额外含 `details`（名字/坐标/匹配距离）与 `unmatched` |

格式：

```json
{
  "schema": "macnativegames.save-map/1",
  "game": "totk",
  "counts": {"shrines_found": 152, "chests": 1344, "...": 0},
  "categories": {
    "shrines_found": {
      "kind": "flag",
      "value": 1,
      "map": {"0x5F6180D7": "<wiki markerId>", "...": "..."}
    },
    "shrines_status": {"kind": "flag", "value": "Clear", "valueHash": 1654019904, "map": {"0x03B85CAE": "..."}},
    "travel_koroks":  {"kind": "flag", "value": "Clear", "valueHash": 1654019904, "map": {"0x...": "..."}},
    "chests":         {"kind": "guid", "map": {"0xF026D163D19391C5": "..."}}
  }
}
```

- `flag` 类：key 是 32 位 hash（`"0x%08X"`），**值必须等于 `value`** 才算完成；
  `value: "Clear"` 表示 `hash('Clear') = 0x4C0A63F4`（神庙状态/旅行克洛格等有"进行中"阶段值，存在≠完成）。
- `guid` 类：key 是 64 位实例 GUID（`"0x%016X"`），在 `/v1/save/state` 的 `guids` 数组里存在即完成。
- `map` 的值是 wiki `map-content.json` 里 `markers[].id`（注意：不同类别 id 形态不同，有的为 uuid，
  有的为 `gamertw:chest:Treasure0080` 这类字符串；用 `id` 字段精确查找，不要用 `source_id` 猜）。

### 2.3 工具（可复现生成映射）

| 文件 | 作用 |
|---|---|
| `games/totk/scripts/diag/romfs_map_objects.py` | RomFS 读取、zstd 字典提取、BYAML v4/v7 解析 |
| `games/totk/scripts/diag/build_save_marker_map.py` | 生成映射表（命令见 §5）|
| `games/totk/scripts/diag/collection_status.py` | **参考消费者**：读网关 state + 映射 → 输出已完成标记 |

### 2.4 文档

| 文件 | 内容 |
|---|---|
| `docs/game-interface.md` §6.5、§6.6 | 接口契约 + 消费方式 |
| `docs/save-mapping-rules.md` | 映射规则、RomFS/BYAML 细节、未覆盖项 |
| `docs/collection-sync-research.md` | 调研全过程与结论（背景）|
| `docs/save-event-observations.json` | 23 个实测事件记录（验证用）|

## 3. 接口（游戏运行中即可访问）

发现：`~/Library/Application Support/MacNativeGames/TOTK/gateway.json`
（`{"url":"http://127.0.0.1:17862", "port":17862, "pid":..., "version":"1.4.3"}`）；
进程退出后文件可能残留，**以 `/v1/meta` 能否访问为准**。

| 端点 | 说明 |
|---|---|
| `GET /v1/meta` | 能力位；`capabilities.progress.available = true` 表示支持本文档接口 |
| `GET /v1/state` | 实时位置/朝向/层（`inWorld` 为 false 时是菜单占位）|
| `GET /v1/trail?seconds=&max=` | 最近轨迹（2Hz，内存环）|
| `GET /v1/save/slots` | 存档槽位列表 |
| `GET /v1/save/summary?slot=latest` | 最近存档摘要（位置/时间）|
| `GET /v1/save/state?slot=latest` | **解析后的存档**：`flags:[[hash,value],...]`（只含非 0）、`guids:["0x...",...]`、`position`、`gameTime`、`counts`、`seq` |
| `GET /v1/save/delta?since=<seq>` | 相邻存档增量（元素结构见 §3.1）；`delta` 也会刷新最新存档 |

`/v1/save/state` 约 120 KB（保存 5000+ 个非零 flag），首次拉取后建议缓存，之后用 delta 更新。
存档正在写入时返回 409 `save-busy`，重试即可；菜单/未进世界也能读（读的是磁盘文件）。

### 3.1 delta 元素结构（务必按对象解析）

```json
{"seq":6,"slot":"00","savedAtMs":1792631681458,"intervalMs":194783,
 "position":{"x":459.894,"y":1549.955,"z":1698.868},
 "gameTime":{"hour":1,"minute":38},
 "added":[{"hash":"0x8899AABB","from":0,"to":1}],
 "changed":[{"hash":"0x2106FA19","from":30,"to":38}],
 "removed":[{"hash":"0x11223344","from":1,"to":0}],
 "newGuids":["0x44FCEE2B3F9BAB36"],
 "counts":{"added":1,"changed":1,"removed":1,"newGuids":1}}
```

- 三个数组的元素都是对象 `{hash, from, to}`；`added.from = 0`，`removed.to = 0`；
- `changed` 包含"非 0 → 0"（`to: 0`）；
- 判定完成用 **`to`**；按 `seq` 升序合并；
- 没有新存档：`seq` 不变、`deltas: []`（正常）；
- `slot` 是产生该增量的那次存档槽位，可能比当前 `state?slot=latest` 旧（后来又有新存档时）；
  `state?slot=latest` 始终是当前最新；两者配合：全量校正 + 增量推进；
- 阶段值：`hash('Clear') = 0x62965740`（神庙状态、旅行克洛格），
  `hash('Complete') = 0x4C0A63F4`（任务完成），`hash('NotClear') = 0x8729BFA5`（旅行克洛格初始非零值）。

## 4. 接入步骤（建议实现）

1. **启动时**：读 `gateway.json` → 探测 `/v1/meta` → 请求 `/v1/save/state?slot=latest`；
2. **判定完成**：遍历 `categories`：
   - `kind == "flag"`：`flags[hash] == value`（`"Clear"` → `0x4C0A63F4`）；
   - `kind == "guid"`：`guid in set(guids)`；
   - 命中 key → 取 `map[key]`（wiki markerId），去重后得到"已完成标记集合"；
3. **标记**：按 markerId 更新 wiki 侧状态（只读同步，不回写游戏）；
4. **增量**：每 5–10 秒轮询 `/v1/save/delta?since=<上次seq>`（或监听存档变化后查询）：
   - `newGuids` 命中 `guid` 类映射 → 新完成对象；
   - `added/changed` 中 flag 值变为期望值 → 新完成对象；
   - `intervalMs` 很大（>2 分钟）或一次出现多个事件 → 进**人工确认队列**，不要盲标；
5. 界面显示"上次存档时间/游戏内时间/进度 N/M"；`/v1/save/state` 的 `counts` 可直接展示。

参考实现：`python3 games/totk/scripts/diag/collection_status.py --map <compact.json>`
（默认连 `http://127.0.0.1:17862`，也可 `--state 本地json` 离线跑）。

最小伪代码：

```ts
const state = await fetch(`${gatewayUrl}/v1/save/state?slot=latest`).then(r => r.json());
const flags = new Map(state.flags.map(([h, v]) => [h, v]));
const guids = new Set(state.guids);
const CLEAR = 0x4C0A63F4;
for (const [category, info] of Object.entries(map.categories)) {
  for (const [key, markerId] of Object.entries(info.map)) {
    const done = info.kind === 'guid'
      ? guids.has(key)
      : flags.get(key) === (info.value === 'Clear' ? CLEAR : info.value);
    if (done) markDone(category, markerId);
  }
}
```

## 5. 重新生成映射表（游戏更新 / wiki 标记变化时）

前置：本机已解包同一版本的 RomFS（`TOTK_build/totk/extracted/v143/romfs/`，含 `index.json` + `romfs.bin`）。

```bash
python3 games/totk/scripts/diag/build_save_marker_map.py \
  TOTK_build/totk/extracted/v143/romfs \
  "/Users/yunnn/CodeProject/Zelda Wiki/data/content/map-content.json" \
  TOTK_build/analysis/save-map/save-marker-map.json
# 可选第 4 个参数：社区参考哈希表（仅用于"哪些宝箱里面是贤者遗志"这一项）
```

会同时输出 `save-marker-map.compact.json`。游戏版本变化（存档格式/RomFS）时需要重新验证。

## 6. 覆盖与置信度

| 类别 | 数量 | 置信度 |
|---|---|---|
| 神庙发现 / 状态 | 152 / 152 | 高（公式 + 实测索引 60/62）|
| 鸟望台 / 破魔之根 / 龙之泪 | 15 / 120 / 12 | 高（坐标 0–1m）|
| 洞窟 / 深穴 / 井 | 171 / 35 / 58 个点（每点 2 key）| 高（池边洞窟 key 实测命中）|
| 隐藏克洛格 | 800 | 高（800/800 公式命中 + 实测坐标）|
| 旅行克洛格 | 100 | **中**（顺序映射；连接器实测完成值 `0x62965740`=`hash('Clear')`，与映射一致）|
| 宝箱 | 1344 key → 1236 标记 | 高（实测"琥珀"开箱 GUID→`chest:Treasure0094` 距离 0.0）|
| 魔犹伊 / 卡邦达 / 贤者遗志 | 147 / 81 / 20 | 高（实测魔犹伊 GUID 命中）|

未覆盖：**依盖队设计图**（存档为设计图类型 hash）、**左纳乌装置制造机**（`RecycleBoxData.<ID>.IsVisit`，
缺实例坐标）、**残旧的地图**、**支线/挑战**（公式已知，需 wiki 任务 key 表）。

## 7. 实机验证记录（2026-10-22）

- 启动游戏 → `/v1/meta` progress 能力为 true；
- `/v1/save/state` 200（约 123 KB），与玩家实际进度一致：神庙 2、洞穴 1、克洛格 3、
  旅行克洛格 1、宝箱 7、魔犹伊 1；
- 玩家开"琥珀"宝箱并存档后：`/v1/save/delta` 返回 seq=1、6 个 new GUID，
  其中 `0xF026D163D19391C5` 经映射表解析为 **`chest:Treasure0094`（名字：琥珀）**，距离 0.0；
  参考消费者的宝箱计数 7 → 8。

## 8. 注意事项

- 只读：网关不提供写接口，不要尝试修改存档；
- 回环：仅 `127.0.0.1`；发现文件用 `pid`/`/v1/meta` 做存活判断；
- 隐私：接口只暴露玩家自己的数据，不暴露原始存档文件；
- 授权：映射表由游戏数据 + wiki 数据生成；参考项目（yalincc/totkmap 无 LICENSE、
  zeldamods/objmap-totk 有 LICENSE）仅用于验证/名称对照，正式分发前请确认授权；
- 版本：映射表与 1.4.3 RomFS、当前 wiki `map-content.json` 快照绑定；wiki 内容更新后重新生成。
