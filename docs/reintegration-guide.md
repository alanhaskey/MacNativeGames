# 连接器 / Wiki 重新对接指南（TOTK 游戏网关 v1）

- 适用：MacNativeGames `totk-to-wiki` 分支，游戏 1.4.3，内置网关 `gameapi/1`
- 更新：2026-10-22（对应提交 `fed36cb`）
- 读者：连接器（本地网关消费方）与 wiki（地图标记与上传侧）
- 目标：一次说明白"这次改了什么、接口长什么样、两边各要改哪里、如何联调"

---

## 0. TL;DR（五分钟接入）

1. **发现网关**：读
   `~/Library/Application Support/MacNativeGames/TOTK/gateway.json`，取 `url`（默认 `http://127.0.0.1:17862`）。
   `pid`/`startedAt` 变化 = 网关重启过，要重新全量对齐。
2. **三条数据流，职责不同**：

   | 流 | 端点 | 用途 | 延迟 |
   |---|---|---|---|
   | 位置 | `GET /v1/state`（或 SSE `/v1/stream`） | 玩家实时位置/朝向/层 | ~100 ms |
   | 存档 | `GET /v1/save/state`、`GET /v1/save/delta?since=` | flag 类收集状态（克洛格/神庙/洞窟/塔/树根/龙之泪/井/深穴…） | 自动存档触发后秒级~一分钟 |
   | **即时事件** | `GET /v1/progress/events?since=`（SSE） | **GUID 类**（宝箱/魔犹伊/卡邦达/贤者遗志） | **< 1 秒** |

3. **两个必须记住的语义**：
   - **神庙"发现" ≠ "完成"**：`shrines_found`（`IsVisitLocation.Dungeon%03d`）只表示发现；
     完成必须看 `shrines_status`（`DungeonState.Dungeon%03d`）== `hash('Clear') = 0x62965740`。
     映射表已带 `role: discovered | completed`，按 role 使用。
   - 同一份"上次对齐"时间点之后，三条流要**合并去重**：GUID 事件负责"快"，delta/state 负责"全"。

---

## 1. 本次变更清单（相对上一版对接）

| # | 变更 | 影响方 | 说明 |
|---|---|---|---|
| 1 | **delta 元素统一为对象** | 连接器 | `added`/`changed`/`removed` 都是 `{hash, from, to}`；新增 `from:0`、删除 `to:0` |
| 2 | delta 新增 `newGuids`、`counts` | 连接器 | GUID 类收集也会出现在 delta（存档兜底） |
| 3 | **轮询 delta 时代理会主动刷新最新存档** | 连接器 | 修复"新存档不被发现 → 感觉不同步" |
| 4 | `/v1/save/state` 收紧 | 连接器 | `flags` 只给非零项；新增 `counts`；`guids` 全量升序 |
| 5 | **新增 `/v1/progress/events`（SSE）** | 双方 | GUID 类即时事件；`/v1/meta` 增加 `capabilities.events` |
| 6 | **映射表增加 `role` / `valueHash`** | wiki | 区分 discovered/completed；值判定不再靠猜 |
| 7 | **神庙发现/完成语义修正** | wiki | 发现≠完成，见 §4.3 |
| 8 | `gateway.json` 增加 `pid` / `startedAt` | 连接器 | 判断网关重启、重连后需重新 baseline |

对应提交：`fed36cb`（SSE 事件）、`5e4e5bc`（delta 统一+刷新）、`7a6d0b8`（role 元数据）、
`ee77a87`（Clear/Complete 哈希修正）、`6db4808`（参考消费者）。

---

## 2. 拓扑与连接

两种部署都支持，任选：

```
[A] 直连（推荐，延迟最低）
    wiki/前端 ──HTTP──▶ 127.0.0.1:17862（游戏进程内网关，只读）

[B] 经连接器转发
    wiki/前端 ──▶ 连接器（本地） ──HTTP──▶ 127.0.0.1:17862
```

要求：

- **SSE 必须不被缓冲**：转发时关闭压缩/缓冲（网关已返回 `X-Accel-Buffering: no`），并保持长连接；
- 连接器可在 SSE 事件流上做过滤/聚合，但**不要合并 seq**（或自行维护顺序号）；
- 所有端点只读、幂等；不需要鉴权（仅绑定 127.0.0.1）；
- 建议先 `GET /v1/meta` 探测能力，`capabilities.<name>.available == true` 才调用对应端点。

---

## 3. 接口参考

### 3.1 发现文件

路径：`~/Library/Application Support/MacNativeGames/TOTK/gateway.json`

```json
{"schema":"macnativegames.gateway/1","url":"http://127.0.0.1:17862","port":17862,
 "pid":38636,"game":"totk","version":"1.4.3","startedAt":1792657066648}
```

| 字段 | 说明 |
|---|---|
| `url` | 基础地址（`http://127.0.0.1:<port>`） |
| `pid` | 游戏进程 pid |
| `startedAt` | 网关启动毫秒时间戳；**变化即重启**，seq 会从 0 重新开始 |
| `game`/`version` | `totk` / `1.4.3` |

### 3.2 `GET /v1/meta`

```json
{"api":"gameapi/1","bridge":"1.4.3","builtin":true,"producer":"maplink",
 "rateHz":10,"game":"totk","version":"1.4.3","port":17862,
 "capabilities":{
   "position":{"schema":"macnativegames.maplink/1","available":true,"rateHz":10},
   "trail":{"schema":"macnativegames.trail/1","available":true,"maxSeconds":600,"rateHz":2},
   "save":{"schema":"macnativegames.save/1","summary":true,"slots":true,
           "fields":["position","savedAt","slot"]},
   "progress":{"schema":"macnativegames.progress/1","available":true,
               "state":true,"delta":true,
               "fields":["flags","guids","position","gameTime","seq"]},
   "events":{"schema":"macnativegames.progress.events/1","available":true,
             "kinds":["guid"],"transport":"sse"},
   "control":{"available":false}}}
```

### 3.3 `GET /v1/state`（位置）

```json
{"schema":"macnativegames.maplink/1","ok":true,"game":"totk","version":"1.4.3",
 "frame":16032,"t":1792657293298,"inWorld":true,
 "x":-335.640,"y":129.965,"z":-392.105,"yaw":45.2,
 "rot":[0.704705,0.000000,0.709500,0.000000,1.000000,-0.000000,
        -0.709500,0.000000,0.704705],
 "layer":0,"layerSource":"height","status":"live","ageMs":50}
```

| 字段 | 说明 |
|---|---|
| `inWorld`/`status` | 菜单/加载中为 `false` / `not-in-world` |
| `x,y,z` | 游戏世界坐标（与存档、映射坐标同一坐标系，米） |
| `yaw` | 度，0 = +Z，90 = +X |
| `rot` | 3×3 旋转矩阵（行主序 9 个 float） |
| `layer` | `-1` 地下（y<-500）、`0` 地表、`1` 天空（y>1300）；`layerSource` 恒为 `height` |
| `t` | 采集时间（毫秒）；`ageMs` = now - t |

### 3.4 `GET /v1/save/summary`

```json
{"schema":"macnativegames.save/1","ok":true,"game":"totk","version":"1.4.3",
 "slot":"05","savedAt":"2026-10-22T08:20:01Z",
 "player":{"x":-335.640,"y":129.965,"z":-392.105},
 "optional":{"playtimeSeconds":null,"hearts":null,"stamina":null,"rupees":null}}
```

### 3.5 `GET /v1/save/slots`

```json
{"schema":"macnativegames.save/1","slots":[
  {"slot":"05","savedAt":"2026-10-22T08:20:01Z","size":2307856},
  {"slot":"00","savedAt":"2026-10-22T08:13:36Z","size":2307856}]}
```

### 3.6 `GET /v1/save/state?slot=latest|<NN>`

- `slot` 缺省 `latest` = 最新修改的存档；也可指定 `00`..`05`。
- 当前存档变化时接口会自动刷新（无需重启网关）。

```json
{"schema":"macnativegames.progress/1","ok":true,"game":"totk","version":"1.4.3",
 "slot":"05","savedAtMs":1792657201927,"seq":1,
 "position":{"x":-335.64,"y":129.965,"z":-392.105},
 "gameTime":{"hour":0,"minute":47},
 "flags":[["0xFEA4461E",1],["0x3C6DBBEC",1],["0x5F6180D7",1]],
 "guids":["0x044A11C31E98AA77","0x046D97B7539BF549"],
 "counts":{"flags":30753,"nonzero":5327,"guids":253}}
```

| 字段 | 说明 |
|---|---|
| `savedAtMs` | 存档文件 mtime（毫秒） |
| `seq` | 网关内 delta 序号（重启归 0） |
| `flags` | **只包含非零项**：`[hash字符串, 无符号32位值]`，按 hash 升序 |
| `guids` | 全部 GUID（16 位十六进制字符串），升序 |
| `counts.flags` | 解析出的 flag 总条数；`nonzero` = `flags` 数组长度；`guids` = GUID 数 |

### 3.7 `GET /v1/save/delta?since=<seq>`

- 返回 `seq > since` 的增量（环形缓冲最多 **8** 条；`since=0` 返回保留的全部）。
- 每次调用都会检查并解析最新存档（新存档会生成一条 delta）。
- `seq` 全局单调递增，**跨存档槽连续**；网关重启后从 0 开始（此时用 state 重新 baseline）。

```json
{"schema":"macnativegames.progress/1","ok":true,"seq":1,"deltas":[
  {"schema":"macnativegames.progress/1","ok":true,"seq":1,"slot":"05",
   "savedAtMs":1792657201927,"intervalMs":385033,
   "position":{"x":-335.64,"y":129.965,"z":-392.105},
   "gameTime":{"hour":0,"minute":47},
   "added":[],
   "changed":[{"hash":"0x2106FA19","from":13,"to":47},
              {"hash":"0x7F9895D3","from":0,"to":1}],
   "removed":[],
   "newGuids":["0xB7B4CD3A87A43E3E"],
   "counts":{"added":0,"changed":9,"removed":0,"newGuids":1}}]}
```

| 字段 | 说明 |
|---|---|
| `intervalMs` | 与上一次存档 mtime 的差（毫秒） |
| `added` | 新出现的非零 flag：`{hash, from:0, to:value}` |
| `changed` | 值变化（含变为 0 的删除）：`{hash, from, to}`；`to == 0` 表示删除 |
| `removed` | 键消失且原值非零：`{hash, from, to:0}` |
| `newGuids` | 新增 GUID（16 位十六进制字符串） |
| `counts` | 各类数量（注意：`counts.changed` 含 `to==0` 的删除项） |

> 应用建议：以 `hash` 为 key 覆盖写入；`to == 0` 时删除/置未完成；`newGuids` 追加到 GUID 集合并标记对应对象。

### 3.8 `GET /v1/progress/events?since=<seq>`（SSE，**新增**）

- `Content-Type: text/event-stream`；先 `hello`，之后每次收集推一条 `progress`；15 秒一次 `: ping` 注释心跳。
- 仅覆盖 **GUID 类**：宝箱 / 魔犹伊 / 卡邦达 / 贤者遗志（即 `capabilities.events.kinds == ["guid"]`）。
- 事件在**玩家动作瞬间**产生（实测 < 1 秒，早于自动存档）。
- 缓冲 **256** 条；`since` 只表示服务端事件序号。当前 `progress` 事件不带 seq，因此**断线重连后建议先 `/v1/save/state` 全量对齐**，再以 `since=0` 继续接收新事件。

```
event: hello
data: {"schema":"macnativegames.progress.events/1","kinds":["guid"],"since":0}

event: progress
data: {"schema":"macnativegames.progress.events/1","t":1792657202132,"kind":"guid",
       "guid":"0xB7B4CD3A87A43E3E","hasPosition":true,
       "position":{"x":-335.640,"y":129.965,"z":-392.105},"layer":0}
```

| 字段 | 说明 |
|---|---|
| `kind` | 目前恒为 `"guid"` |
| `guid` | 16 位十六进制；对应映射表 `details.chests/bubbuls/addison/sage_wills` 的 key |
| `position`/`layer` | 收集瞬间玩家位置（与 `/v1/state` 同坐标系）；菜单中可能 `hasPosition:false` |
| `t` | 事件时间（毫秒） |

### 3.9 其他

- `GET /v1/trail?...`：玩家轨迹（`capabilities.trail`），本次未改动，见 `docs/maplink-interfaces.md`。
- `GET /v1/stream`：位置 SSE（兼容旧对接），本次未改动。

---

## 4. 数据契约细节

### 4.1 格式约定

- `hash`：32 位十六进制字符串，带 `0x` 前缀，**大写**（如 `0x5F6180D7`）。
- `guid`：64 位十六进制字符串，带 `0x`，**16 位补零大写**（如 `0x044A11C31E98AA77`）。
- 时间：`savedAtMs`/`t` 是 Unix 毫秒；`savedAt` 是 ISO-8601 UTC 字符串。
- 坐标：游戏世界坐标（米），`game_x = RomFS X`、`game_z = -RomFS Z`、`game_y = RomFS Y`。
- 排序：`flags` 按 hash 升序；`guids` 升序；delta 内不保证顺序。

### 4.2 类别与 role（映射表 `categories`）

| 类别 | kind | role | 判定值 |
|---|---|---|---|
| `shrines_found` | flag | **discovered** | `1` |
| `shrines_status` | flag | **completed** | `Clear`（`0x62965740`） |
| `towers` / `lightroots` / `dragon_tears` / `caves` / `chasms` / `wells` / `koroks` | flag | completed | `1` |
| `travel_koroks` | flag | completed | `Clear`（`0x62965740`） |
| `chests` / `bubbuls` / `addison` / `sage_wills` | **guid** | completed | GUID 是否存在 |

### 4.3 关键语义：神庙"发现"与"完成"

- `hash('IsVisitLocation.Dungeon%03d')` = 1 → 神庙**被发现**（进入过）；
- `hash('DungeonState.Dungeon%03d')` == `hash('Clear')` → 神庙**已通关**；
- wiki 的"完成"标记**只能**用 `shrines_status`，`shrines_found` 最多用于"已发现"图层；
- 其他 flag 类（克洛格/洞窟/塔/树根…）都是一次性完成标志，直接判非零。

### 4.4 delta 应用伪代码

```text
for d in delta.deltas (seq 升序):
    for e in d.added + d.changed + d.removed:
        if e.to == 0:  marks[e.hash] = 0        # 删除/取消
        else:          marks[e.hash] = e.to
    for g in d.newGuids: guids.add(g)
    cursor = d.seq
```

### 4.5 事件与 delta 的合并

- 收到 `progress` 事件：按 `guid` 立即标记；同时把该 GUID 放入本地 `pendingGuids` 集合；
- 下一次 delta 的 `newGuids` 若包含它：直接去重；若不含（事件早于存档），保留即可；
- 建议在本地为事件自增 `eventIndex` 用于日志/重连，但服务端 `since` 只认服务端 seq（当前 progress 事件不带 seq，
  若需要精确补发，断线后先 `/v1/save/state` 全量对齐，再 `events?since=0` 只接收新事件，避免重复）。

---

## 5. 映射文件

仓库内两份（都在 `TOTK_build/analysis/save-map/`）：

| 文件 | 大小 | 用途 |
|---|---|---|
| `save-marker-map.json`（full） | ~1.4 MB | 含坐标/名称/entry/unmatched，离线建库用 |
| `save-marker-map.compact.json`（compact） | ~170 KB | 运行时用：hash→markerId + 类别元数据 |

### 5.1 compact 结构

```json
{"schema":"macnativegames.save-map/1","game":"totk","source":"...","counts":{...},
 "categories":{
   "shrines_found":{"kind":"flag","role":"discovered","value":1,"valueHash":1,
                    "map":{"0xA487C021":"Dungeon:0x3370bc5cc790fa0d"}},
   "shrines_status":{"kind":"flag","role":"completed","value":"Clear",
                     "valueHash":1654019904,
                     "map":{"0x3EB899D5":"Dungeon:0x3370bc5cc790fa0d"}},
   "chests":{"kind":"guid","role":"completed","value":1,"valueHash":1,
             "map":{"0x3DBD99119FFE9C22":"gamertw:chest:Treasure1130"}}}}
```

| 字段 | 说明 |
|---|---|
| `kind` | `flag` = 用 hash 查值；`guid` = 用 GUID 判断存在 |
| `role` | `discovered`（发现）/ `completed`（完成） |
| `value` / `valueHash` | 期望值文本/数值；flag 类按 `valueHash` 比较，**不要自己再算 Clear** |
| `map` | `hash`(或 `guid`) → `markerId`（与 wiki `markerId` 对齐） |

### 5.2 full 结构要点

- `details.<category>.<hash>`：`{markerId, sourceId, name, entry, position:[x,y,z], distance}`；
- `unmatched`：RomFS 中未匹配到 wiki 的对象（如部分宝箱），wiki 可先显示"未映射"；
- 重新生成：

```bash
python3 games/totk/scripts/diag/build_save_marker_map.py \
  TOTK_build/totk/extracted/v143/romfs \
  "/Users/yunnn/CodeProject/Zelda Wiki/data/content/map-content.json" \
  TOTK_build/analysis/save-map/save-marker-map.json
```

---

## 6. 推荐接入流程

### 6.1 首次/重连对齐

```text
读 gateway.json → GET /v1/meta（确认 capabilities）
  → GET /v1/save/slots（可选，展示多个存档）
  → GET /v1/save/state?slot=latest
       · 用 categories.role 应用全量 flags（发现/完成分开）
       · guids → 标记 GUID 类
  → GET /v1/save/delta?since=<state.seq>   之后每 1–2 s 轮询
  → GET /v1/progress/events?since=0         常驻 SSE
```

### 6.2 运行中

- GUID 事件：即时更新地图（推荐先按 guid 反查 markerId，再写完成）；
- delta：处理 flag 类；若 `to==0` 撤销标记；
- 位置：`/v1/state` 轮询或 `/v1/stream`。

### 6.3 网关重启 / 游戏重启

`gateway.json` 的 `pid`/`startedAt` 变化 → 丢弃旧 `seq`/`since`，重新执行 §6.1。
（网关重启后 `seq` 从 0 开始，旧 cursor 会被当成"全部已读"或重复，必须重 baseline。）

### 6.4 换存档槽

每 `delta` 都带 `slot`；`slot` 变化表示游戏换了存档（读档/新建）。
建议换槽时重新 `state` 全量对齐（不同存档的 flag/guid 集合完全不同）。

---

## 7. 连接器改造清单

- [ ] 停用旧版解析：`added` 曾是 `{hash,value}`，现在统一为 `{hash,from,to}`
- [ ] delta 增加 `newGuids`/`counts` 处理
- [ ] 轮询 delta 时不再假设"只有游戏写盘才变"（网关会主动刷新）
- [ ] 转发/代理 SSE `/v1/progress/events`（不缓冲、不压缩、保持长连接）
- [ ] 用 `meta.capabilities.events.available` 做兼容降级（旧游戏构建无此能力）
- [ ] 监听 `gateway.json` 的 `pid`/`startedAt`，变化时重建连接并重新 baseline
- [ ] 透传 `/v1/save/state` 的 `counts` 便于自检
- [ ] 错误处理：连接重置返回 404/空 body 时按"能力缺失"降级，不要当作数据错误

## 8. Wiki 改造清单

- [ ] **神庙完成只认 `shrines_status`**；`shrines_found` 单独做"已发现"样式
- [ ] 按 `categories.<cat>.role` 决定"发现/完成"呈现
- [ ] GUID 类订阅 events 即时标记；delta 兜底去重
- [ ] `valueHash` 比较替代手写 `Clear` 常量
- [ ] 未映射对象（`details` 无此 guid/hash 或 `unmatched`）先标"未映射"，不丢事件
- [ ] 坐标使用游戏坐标；页面若用 RomFS 坐标需 `wiki_z = -game_z`
- [ ] 断线/重启后全量对齐（避免事件流漏单）

---

## 9. 联调测试用例（可直接 curl）

```bash
BASE=$(python3 -c "import json;print(json.load(open('$HOME/Library/Application Support/MacNativeGames/TOTK/gateway.json'))['url'])")
curl -s "$BASE/v1/meta" | python3 -m json.tool | head -40        # 期望见 capabilities.events.available=true
curl -s "$BASE/v1/state"                                        # 期望 inWorld=true 时有 x/y/z
curl -s "$BASE/v1/save/state?slot=latest" | head -c 300         # 期望 flags/guids/counts
curl -s "$BASE/v1/save/delta?since=0" | head -c 400             # 期望 deltas[].added/changed 为对象
curl -N "$BASE/v1/progress/events?since=0"                      # 期望先 hello，再心跳
```

即时事件实测（2026-10-22，古塔恩巴奇附近开箱获得黄玉）：

```
event: progress
data: {"guid":"0xB7B4CD3A87A43E3E","position":{"x":-335.640,"y":129.965,"z":-392.105},"layer":0}
# 映射 → chest:Treasure0438「黄玉」，与游戏内获得物一致
```

---

## 10. 已知限制

1. **即时事件只有 GUID 类**；flag 类（克洛格/神庙/洞窟…）依赖自动存档，实测秒级~一分钟；
2. 映射表仍有未覆盖对象（部分宝箱/特殊宝箱类型；GUID 会出现在事件/delta 里但查不到 marker）；
3. `progress` 事件不带服务端 seq；精确补发请以 `/v1/save/state` 重对齐为准；
4. SSE 缓冲 256 条，网关重启丢失；
5. delta 环形 8 条：轮询间隔建议 ≤ 2 s，否则可能错过连续存档；
6. `/v1/save/state` 的 `flags` 不含零值项；判断"未完成"= 不存在于数组或值为 0。

---

## 11. 附录

### A. 实测样例（截断）

- `/v1/meta`：见 §3.2
- `/v1/save/state?slot=latest`：`counts {"flags":30753,"nonzero":5327,"guids":253}`
- `/v1/save/delta?since=0`：`seq 1`，`changed` 9 项（含时间字段），`newGuids ["0xB7B4CD3A87A43E3E"]`
- `/v1/progress/events`：`hello` + 开箱 `progress` 事件

### B. 最小消费者（Python，概念示例）

```python
import json, urllib.request, threading

BASE = None
def load_base():
    p = __import__("os").path.expanduser("~/Library/Application Support/MacNativeGames/TOTK/gateway.json")
    return json.load(open(p))["url"]

def get(path):
    return json.loads(urllib.request.urlopen(BASE + path, timeout=5).read())

def events_loop(on_event):
    req = urllib.request.Request(BASE + "/v1/progress/events?since=0")
    with urllib.request.urlopen(req, timeout=3600) as r:
        for raw in r:
            line = raw.decode().strip()
            if line.startswith("data: "):
                on_event(json.loads(line[6:]))

BASE = load_base()
state = get("/v1/save/state?slot=latest")
# 1) 全量对齐 flags/guids（用 compact map 的 role/valueHash）
# 2) 起 SSE 线程 -> 即时标记 GUID
threading.Thread(target=events_loop, args=(lambda e: print("instant", e["guid"]),), daemon=True).start()
# 3) 轮询 delta，应用 flag 增量
```

### C. 相关文档

- 接口全量契约：[game-interface.md](game-interface.md)（含 §6.7 即时事件）
- 旧交接说明（历史）：[collection-sync-handoff.md](collection-sync-handoff.md)
- 映射规则与研究方法：[save-mapping-rules.md](save-mapping-rules.md)、[collection-sync-research.md](collection-sync-research.md)
- 参考消费者脚本：`games/totk/scripts/diag/collection_status.py`
