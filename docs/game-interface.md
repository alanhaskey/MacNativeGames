# 游戏侧接口契约（Game-side interface contract）

> 这份文档是**游戏侧对外的唯一接口约定**。登录、账号、上传、地图 UI、连接器等都在外部（wiki 侧 agent）实现，
> 游戏侧只负责产生信息、并（对少数受控操作）接收命令。
>
> 当前实现状态：**位置、轨迹、存档摘要 v1 已实现**；传送（控制域）与存档增强为规划接口。文档里的"planned"
> 都是契约，不是承诺时间。

## 0. 范围与职责

| 组件 | 属于 | 职责 |
|---|---|---|
| 游戏 App（玩家自己构建） | 本仓库 | 只读导出位置；未来：读存档摘要、接收受控命令 |
| 本地网关 `maplinkd` | 本仓库 | 把游戏产出的文件变成 HTTP/SSE 接口；未来扩展 save/trail/control |
| 连接器 / 登录器 / 上传 / 地图 UI | wiki 侧 | 登录、token、网络上传、地图显示；**不在本仓库** |

游戏侧不实现、也不关心：账号、密码、token、HTTPS 上传、服务器地址、连接器 UI。外部消费者只需要 **本机 HTTP**。

**服务随游戏启动**：网关是游戏进程内置的一个线程（原生 C，无 Python、无额外进程）。游戏启动即自动在回环地址提供接口，
游戏退出即关闭；玩家不需要输入任何命令。实际地址写在 `<data>/gateway.json` 里，外部程序读它即可。
发现文件在强制退出后可能残留，读取方必须先探活（见 §3）。

## 1. 设计原则

1. **只读默认**：大部分接口只读取游戏状态；唯一的写操作（传送/标记）单独放在受控域，默认关闭、需要显式开启。
2. **游戏不联网**：游戏进程不发起任何网络请求；数据落在本机文件，由网关读、由外部程序取。
3. **一个网关**：外部只对接 `http://127.0.0.1:17862`，不直接读游戏内存、不依赖文件格式细节。
4. **按域扩展**：位置 / 存档 / 轨迹 / 控制各自独立 schema；新增域不破坏旧域。
5. **能力协商**：消费者必须先读 `/v1/meta` 的 `capabilities`，按实际能力显示 UI。
6. **只增不改**：v1 内只做追加（新字段、新端点）；破坏性变更升 major 版本。
7. **单机可弃用**：外部程序不运行时，游戏完全正常；游戏不等待任何外部程序。

## 2. 架构与信任边界

```
游戏进程（totk-native）── 写 ──▶ <data>/*.json（生产者文件）
                                      │ 只读
                                      ▼
                           maplinkd（本机网关，127.0.0.1:17862）
                              ├─ 只读域：GET /v1/...
                              └─ 受控域：POST /v1/control/...（默认关闭）
                                      │ HTTP
                                      ▼
                   外部消费者（连接器 / wiki 侧工具 / 脚本）
```

- `<data>` = `~/Library/Application Support/MacNativeGames/TOTK`（BotW 为 `BotW`）。
- 网关只绑回环地址；游戏不监听任何端口。
- 信任边界：同一 macOS 用户的本机进程之间。跨用户/跨机器不是本契约的范畴。

## 3. 发现与版本

| 项 | 值 |
|---|---|
| 网关地址 | 随游戏自动启动；默认 `http://127.0.0.1:17862`，被占用时依次试 17863…17871 |
| 服务发现 | `<data>/gateway.json`：`{ "url", "port", "pid", "game", "version", "startedAt" }` |
| 残留处理 | 游戏退出走 `_exit(0)`，发现文件**可能残留**。使用前先探活：`GET /v1/meta`（1 秒超时）或检查 `pid` 是否存活；失败一律按"游戏未运行"处理，并在下次启动时被覆盖 |
| 关闭 | `TOTK_GATEWAY=0` 启动游戏 |
| 生产者目录 | `~/Library/Application Support/MacNativeGames/TOTK/` |
| 位置文件 | `maplink.json` |
| 网关入口 | `GET /v1/meta`（版本、能力、游戏与采样率）；游戏未运行时无服务 |
| 总体 API 版本 | `gameapi/1`（元数据字段 `api`） |

`GET /v1/meta` 示例：

```json
{
  "api": "gameapi/1",
  "bridge": "1.0.0",
  "producer": "maplink.json",
  "rateHz": 10,
  "capabilities": {
    "position": { "schema": "macnativegames.maplink/1", "available": true, "rateHz": 10 },
    "save":     { "summary": false, "slots": false },
    "trail":    { "available": false, "maxSeconds": 600 },
    "control":  { "teleport": false, "waypoint": false, "tokenRequired": true }
  },
  "uptimeMs": 4321000
}
```

> 能力位现在是对象形式（`available` / `summary` / `slots` / `fields`），外部程序按它决定显示哪些入口；未知能力一律忽略。

## 4. 通用约定

| 项 | 约定 |
|---|---|
| 编码 | UTF-8 JSON，单行或格式化均可 |
| 时间 | Unix 毫秒（`t`）；需要可读时间时用 ISO 8601（`savedAt`） |
| 坐标 | 游戏世界坐标，米；`x/z` 为水平面，`y` 为高度；朝向 `yaw`：度，0=+Z、90=+X，俯视顺时针 |
| 分层 | `layer`：`1` 天空、`0` 地表、`-1` 地底、`null` 未知（当前恒为 null） |
| 状态 | `live` / `not-in-world` / `stale` / `disconnected`（见各域） |
| 缓存 | 所有响应 `Cache-Control: no-store` |
| 未实现能力 | 端点返回 `501 { "error": { "code": "not-implemented", "capability": "..." } }` |
| 未知字段 | 消费者必须忽略；服务端不得依赖消费者忽略行为 |
| 限流 | 读域不限；控制域 1 次/秒/命令 |

错误体统一：

```json
{ "error": { "code": "invalid-request", "message": "human readable", "capability": "teleport" } }
```

| HTTP | code | 含义 |
|---|---|---|
| 400 | `invalid-request` | 参数不合法 |
| 403 | `forbidden` | 控制域未开启或 token 错误 |
| 404 | `not-found` | 未知端点/资源 |
| 409 | `bad-state` | 当前状态不允许（如菜单中传送） |
| 423 | `busy` | 上一条命令未完成 |
| 501 | `not-implemented` | 该能力尚未实现 |
| 503 | `producer-missing` | 游戏未运行/文件缺失（仅 health） |

## 5. 位置域（position v1，已实现）

生产者文件 `maplink.json`（10 Hz，原子写，默认开启，`TOTK_MAPLINK=0` 关闭）：

```json
{"schema":"macnativegames.maplink/1","ok":true,"inWorld":true,"game":"totk","version":"1.4.3",
 "frame":123456,"t":1761000000123,"x":1234.5,"y":123.4,"z":-567.8,
 "yaw":90.0,"headingSource":"actor","layer":null,
 "rot":[0,0,1,0,1,0,-1,0,0]}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `ok` / `inWorld` | bool | 在世界中且位置有效；主菜单/加载/标题为 false（菜单占位玩家不会发布） |
| `x/y/z` | number | 世界坐标（米） |
| `yaw` | number\|null | 朝向；`headingSource` 说明来源（`actor`/`motion`/`unknown`） |
| `rot` | number[9] | 原始 3×3 旋转（行主序），供精确换算/校准 |
| `layer` | number\|null | `1` 天空 / `0` 地表 / `-1` 地底 / `null` 不在世界。当前由高度推导（`layerSource:"height"`，阈值：<-500 地底、>1300 天空），消费者可结合 `y` 自行修正 |
| `interior` | bool | **新增**：为 true 表示玩家在独立场景内（神庙/神兽内部等），此时 `x/y/z` 是**冻结的进入前世界坐标**，由 `positionSource:"frozen"` 标识；`rawPosition` 给出内部本地坐标（不可用于大地图定位） |
| `positionSource` | string | `live`（正常世界坐标）或 `frozen`（内部场景期间冻结世界坐标） |
| `rawPosition` | object | 仅 `interior:true` 时出现：内部场景的原始本地坐标（供调试/未来识别具体神庙） |

网关端点（已实现）：

| 端点 | 说明 |
|---|---|
| `GET /v1/state` | 快照，永远 200，状态看 `status` |
| `GET /v1/stream` | SSE：`hello` / `state`(10 Hz) / `status`(变化时) + 15 s 心跳 |
| `GET /v1/health` | `live` 时 200，否则 503 |

`status` 规则：`live`（`ok` 且样本 ≤1 s）；`not-in-world`（`ok:false`）；`stale`（>1 s）；`disconnected`（文件缺失或 >10 s 无更新，带 `reason`）。

详细语义见 [docs/maplink.md](maplink.md)（生产者）与 [docs/maplink-interfaces.md](maplink-interfaces.md)（网关与状态细节）。

## 6. 存档域（save v1，已实现：位置/savedAt/槽位）

**用途**：给外部程序提供"最近一次存档的摘要"，不实时、不读实时内存。

**生产者**：游戏在加载存档/保存后写 `<data>/save-summary.json`（原子替换）。实现方式（游戏内解析 / 网关解析存档文件）对消费者不可见；开局最小切片建议：位置、游戏时长、心/耐力、卢比；背包/神庙/完成度后续。

`save-summary.json`：

```json
{"schema":"macnativegames.save/1","ok":true,"game":"totk","version":"1.4.3",
 "slot":"1","savedAt":"2026-10-21T08:12:33Z","playtimeSeconds":7320,
 "player":{"x":1234.5,"y":123.4,"z":-567.8,"yaw":90.0},
 "hearts":{"current":9,"max":40},"stamina":{"current":1.0,"max":3.0},
 "rupees":1234,
 "optional":{"shrines":null,"completionRate":null}}
```

网关端点（planned）：

| 端点 | 返回 |
|---|---|
| `GET /v1/save/slots` | `{ "slots": [ { "slot": "00", "savedAt": "...", "size": 2307856 } ] }`（按修改时间倒序） |
| `GET /v1/save/summary?slot=latest` | `{ "ok":true, "slot":"00", "savedAt":"...", "player":{x,y,z}, "optional":{...null} }` |
| `GET /v1/save/state?slot=latest` | 解析后的 hash 表 + GUID 数组 + 坐标 + 游戏内时间（见 6.5） |
| `GET /v1/save/delta?since=<seq>` | 相邻存档之间的增量（见 6.5） |
| `GET /v1/save/raw?slot=latest` | **不提供**（不暴露原始存档，避免误用/隐私风险） |

规则：

- 已实测 1.4.3：`progress.sav` 2,307,856 字节，按标签哈希扫描位置（`0xC884818D` / `0x8773F02D`），不依赖文件大小白名单。
- 只读；`savedAt` 取文件 mtime；`ok:false` 表示没有可读存档；写入中返回 409 `bad-state`。
- 解析按 (路径, mtime, size) 缓存，避免重复读 2.3 MB。
- 存档格式随游戏版本变化；`version` 字段用于消费者提示不兼容。
- 隐私：摘要只含玩家自己的数据；网关只允许本机读取。
- 失败：存档正在写入时返回 409 `bad-state`（不要读半写文件）。

## 6.5 进度域（progress v1，已实现：解析后的存档状态）

**用途**：给 wiki/收集同步提供"存档里已经完成了什么"，只读、不暴露原始存档。
**实现**：网关读取 `progress.sav`，解析 hash 表（`u32 hash → u32 value`，从 `0x28` 到哨兵 `0xA3DB7114`，
哨兵的 value 是 GUID 数组偏移）与 GUID 数组，并附带存档坐标与游戏内时间。

| 端点 | 返回 |
|---|---|
| `GET /v1/save/state?slot=latest` | `{ok, slot, savedAtMs, seq, position{x,y,z}, gameTime{hour,minute}, flags:[[hash,value],...], guids:[...], counts{flags,nonzero,guids}}` |
| `GET /v1/save/delta?since=<seq>` | `{ok, seq, deltas:[...]}`，元素结构见下 |

示例（截断）：

```json
{"schema":"macnativegames.progress/1","ok":true,"game":"totk","version":"1.4.3","slot":"05",
 "savedAtMs":1761110000000,"seq":1,
 "position":{"x":168.178,"y":1484.956,"z":1633.941},"gameTime":{"hour":20,"minute":27},
 "flags":[["0x5F6180D7",1],["0xB33C6725",1]],
 "guids":["0x058472B9F50BBFA8"],
 "counts":{"flags":30753,"nonzero":5087,"guids":119}}
```

约定：

- `flags` 只返回**非 0 的条目**（缺失即 0）；键是小写 `0x%08X` 形式的 **murmurhash3-32(游戏内名字)**，
  名字/类别表由消费者维护（参考 yalincc/totkmap 的 `totk_save_hashes.js`，正式实现建议自行从 RomFS 生成）。
- `guids` 是存档 GUID 数组（魔犹伊/遗志/卡邦达/宝箱等对象实例），按存档顺序。
- `delta` 只在解析到**新的最新存档**时生成（最多保留 8 条）；`since` 为上次收到的 `seq`；
  跨槽位不会误报。`intervalMs` 供消费者判断增量是否"干净"（窗口越长越可能混合多个事件）。
- `/v1/save/delta` 与 `/v1/save/state` 都会刷新"最新存档"；**只轮询 delta 也能发现新存档**。
- 没有新存档时：`seq` 不变，`deltas` 为空数组（正常结果，不是错误）。

### 6.5.1 delta 元素的精确结构

三个数组的元素都是**对象**（2026-10-22 后的构建；更早的构建 `added` 用 `{hash,value}`）：

```json
{
  "seq": 6, "slot": "00", "savedAtMs": 1792631681458, "intervalMs": 194783,
  "position": {"x":459.894,"y":1549.955,"z":1698.868},
  "gameTime": {"hour":1,"minute":38},
  "added":   [{"hash":"0x8899AABB","from":0,"to":1}],
  "changed": [{"hash":"0x2106FA19","from":30,"to":38}],
  "removed": [{"hash":"0x11223344","from":1,"to":0}],
  "newGuids": ["0x44FCEE2B3F9BAB36"],
  "counts": {"added":1,"changed":1,"removed":1,"newGuids":1}
}
```

语义：

- `added`：该 key 在上一份存档里**不存在**（注意：存档表里很多 key 默认带 0，所以实际很少见），`from` 恒为 0；
- `changed`：key 存在但值不同，且至少一侧非 0；**非 0 → 0 也在这里**（`to: 0`）；
- `removed`：key 上一份存在且非 0、这一份不存在，`to` 恒为 0；
- **判定"现在是否完成"用 `to`**（同时也可以随时用 `/v1/save/state` 的全量 `flags` 校正）；
- 按 `seq` 升序合并即可；`slot` 是**产生这条增量那次存档的槽位**，可能比当前 `state?slot=latest`
  旧（后来又有新存档时），这是正常的；`state?slot=latest` 永远是当前最新。

阶段值（容易混淆）：

| 字符串 | murmur3 值 | 用在哪 |
|---|---|---|
| `Clear` | `0x62965740` | 神庙状态、旅行克洛格完成 |
| `Complete` | `0x4C0A63F4` | 任务/挑战完成（`hash('Step_'+key)`） |
| `NotClear` | `0x8729BFA5` | 旅行克洛格未完成（初始阶段，非零）|
- 已实测：1.4.3 存档；类别统计与实机一致（神庙 2/152、克洛格 3/800、洞穴 1/151、井 0/58）。
- 写入中返回 409 `save-busy`；无存档 `ok:false, reason:"no-save"`。

## 6.6 收集同步的消费方式（wiki 侧）

1. 加载 `save-marker-map.compact.json`：
   `categories: {类别: {kind: "flag"|"guid", value: 1|"Clear", valueHash: 1|0x62965740, map: {存档 key: wiki markerId}}}`；
2. 轮询 `GET /v1/save/state?slot=latest`：
   - `flags`（`"0x%08X" -> value`）：`kind == "flag"` 的类别，**值等于 `valueHash`** 才算完成
     （`1` 或 `0x62965740`）。只"存在"不算；
     例如旅行克洛格/神庙状态在其他阶段也有非零值；
   - `guids`（`"0x%016X"`）：命中宝箱/魔犹伊/卡邦达/贤者遗志的 GUID 即完成；
3. 需要"刚完成"的实时事件时用 `GET /v1/save/delta?since=<seq>`（间隔长/组大时进人工确认）；
   建议启动时拉一次全量，之后每 5–10 秒轮询 delta；每 1–5 分钟再拉一次全量做校正；
4. 未在映射表里的 key 可以忽略（例如成就类旗帜）；不要把位置/时间字段当事件。

## 6.7 即时收集事件（progress.events v1，已实现：SSE）

> 连接器 / wiki 的完整对接流程见 [reintegration-guide.md](reintegration-guide.md)。

`GET /v1/progress/events?since=<seq>`（`text/event-stream`）。这是**内存即时**通道，只覆盖 GUID 类收集物
（宝箱 / 魔犹伊 / 卡邦达 / 贤者遗志）；flag 类（克洛格/神庙/洞窟/塔/树根/龙之泪/井/深穴）仍走 `/v1/save/delta`。

流格式：
```
event: hello
data: {"schema":"macnativegames.progress.events/1","kinds":["guid"],"since":0}

event: progress
data: {"schema":"macnativegames.progress.events/1","t":1792657202132,"kind":"guid",
       "guid":"0xB7B4CD3A87A43E3E","hasPosition":true,
       "position":{"x":-335.640,"y":129.965,"z":-392.105},"layer":0}
```
- `guid`：16 位十六进制；用映射表 `details.chests/bubbuls/addison/sage_wills` 直接查 markerId/name；
- `position`/`layer`：收集瞬间的玩家位置（可与映射坐标交叉校验，`distance` 已给）；
- `t`：毫秒时间戳；`seq` 隐含在推送顺序中（SSE 断线可用 `since=<最后收到的 seq>` 补发，缓冲区 256 条）；
- 15 秒一次 `: ping` 心跳，客户端应忽略注释行；
- 实测：开箱瞬间（黄玉 `chest:Treasure0438`）< 1 秒推送，早于自动存档；
- 可用环境变量 `TOTK_EVENTS=0` 关闭，`TOTK_EVENTS_LOG=1` 写 `events.log` 便于排查。

推荐消费顺序（wiki 侧）：
1. 启动：`/v1/save/state` 全量对齐 + 订阅 `/v1/progress/events?since=0`；
2. GUID 事件 → 立即标记并显示位置；
3. `/v1/save/delta` → 处理 flag 类完成（注意用 `role`：`shrines_found` 只是发现，
   `shrines_status == hash('Clear')` 才是完成）；
4. 断线重连：`/v1/save/state` 重新对齐 + `events?since=<seq>`。

## 7. 轨迹域（trail v1，已实现）

**用途**：在地图上画最近一段路径。**不持久化、不上传**，由网关在内存里累积（重启即清空）。

| 端点 | 参数 | 返回 |
|---|---|---|
| `GET /v1/trail` | `seconds=1..600`（默认 120）、`max=1..6000`（默认 600） | `{ "schema":"macnativegames.trail/1", "source":"bridge", "seconds":120, "points":[...], "truncated":false }` |

点的字段：`{ "t": 1761000000123, "x": 1.0, "z": 2.0, "yaw": 90.0, "inWorld": true }`（不重复发送相同位置；2 Hz 上限）。

规则：

- 缓冲区 10 分钟/最多 2400 点；采样 2 Hz（与网关读取频率一致，位置本身 10 Hz）。
- 不跨网关重启保留；不写磁盘；不参与任何网络上传（上传由外部程序决定）。
- 时间缺口 > 2 s 时相邻点之间插入 `"gap": true` 标记（消费者画断线）。
- 能力位：`capabilities.trail.available`。

## 8. 控制域（control v1，planned，默认关闭）

**用途**：外部程序让游戏执行少数受控操作。**写操作，风险最高，默认关闭。**

### 8.1 开启与鉴权

- 游戏侧环境变量 `TOTK_CONTROL=1` 才启用命令轮询；默认不轮询。
- 网关控制端点要求请求头 `X-Control-Token`，token 在 `<data>/control.token`（0600，首次开启时生成）。
- 仅回环；网关不把控制端点暴露到局域网。
- 每次开启会话首次写操作前，**强制备份当前存档**（`profile-before-control-<时间>`），失败则拒绝控制命令。

### 8.2 命令

| 命令 | 参数 | 状态 |
|---|---|---|
| `teleport` | `{x,y,z}` | planned |
| `waypoint` | `{x,y,z,label}`（游戏内放一个自定义标记） | planned |
| `ping` | `{}`（连通性/延迟探测） | planned |

### 8.3 ABI（游戏 ↔ 网关，本地文件）

网关把命令追加到 `<data>/control.txt`，一行一个 JSON：

```json
{"id":"c-0001","command":"teleport","params":{"x":120.5,"y":30.0,"z":-880.2},"expiresAt":1761000005000}
```

游戏每 100 ms 轮询（复用现有 input.txt 的轮询线程），满足条件才执行，并把结果追加到 `<data>/control-results.jsonl`：

```json
{"id":"c-0001","command":"teleport","status":"done","frame":123456,"at":1761000003123}
{"id":"c-0002","command":"teleport","status":"rejected","reason":"not-in-world"}
{"id":"c-0003","command":"waypoint","status":"rejected","reason":"capability-disabled"}
```

`status`：`done` / `rejected`（带 `reason`）/ `expired`。命令 5 秒过期；同一时刻只执行一条。

### 8.4 网关端点

```
POST /v1/control/teleport   { "x":120.5, "y":30.0, "z":-880.2 }
  200 { "id":"c-0001", "status":"done", "frame":123456 }
  409 { "error": { "code":"bad-state", "message":"not in world" } }
  423 { "error": { "code":"busy" } }
  403 { "error": { "code":"forbidden", "message":"control disabled or token missing" } }

POST /v1/control/waypoint   { "x":..., "y":..., "z":..., "label":"..." }   → 同上
POST /v1/control/ping       {}  → { "pong": true, "frame": 123456 }
```

同步返回（网关等结果，超时 3 s 返回 504 `timeout`）。

### 8.5 安全与副作用

- 传送会改变玩家状态：**只在玩家显式开启控制、且已备份存档时允许**；菜单/加载中（`inWorld=false`）一律拒绝。
- 冷却：同一命令 1 次/秒；每会话最多 100 次；超出拒绝。
- 游戏侧记录每条命令与结果到游戏日志；网关不记录 token。
- 传送实现复用已逐站点验证的 `forceSetMatrix` 路径（`survey move` 同源），但**不复用** survey 的测试档案门禁；
  产品开关是独立的 `TOTK_CONTROL`。
- 默认配置下这些端点返回 501/403，消费者必须按 `capabilities.control` 判断，不要假定可用。

## 9. 预留域（Reserved）

| 域 | 端点（预留） | 说明 |
|---|---|---|
| 世界信息 | `GET /v1/world` | 区域名、天气、游戏内时间、坐标所在的塔区 |
| 进/背包 | `GET /v1/inventory` | 只读摘要，字段待定（隐私与体积要评估） |
| 进度 | `GET /v1/progress` | 神庙/任务/完成度摘要 |
| 多游戏 | 字段 `game`/`version` | BotW 复用同一契约，域内语义按游戏 `version` 区分 |

预留只表示"命名与版本空间保留"，未实现前一律 501。

## 10. 安全模型

- 只绑回环；读域无鉴权（同用户本机信任）；控制域需 token + 环境变量 + 存档备份。
- 生产者文件默认 0644（只含位置）；`control.token` 0600；存档摘要仅本机可读。
- 游戏不联网、不上传、不读外部文件（除自己的数据目录）。
- 消费者不得把网关暴露到公网；需要远程时由消费者自己中继（游戏侧不参与）。
- 不提供 `save/raw`、不提供内存读写、不提供任意命令执行。

## 11. 兼容与版本策略

- `gameapi/1`：新增端点/字段不算破坏；删除/改名/改单位需升 `gameapi/2`。
- 各域单独版本（`macnativegames.maplink/1`、`.save/1`、`.trail/1`）。
- 消费者必须：读 `/v1/meta` → 判能力 → 忽略未知字段 → 按 `status` 显示。
- 弃用：先标 `"deprecated": true` 至少一个版本，再移除。

## 12. 状态矩阵与路线

| 域 | 能力 | 状态 | 备注 |
|---|---|---|---|
| position | 内置网关（随游戏启动）+ state/stream/health + 发现文件 | **已实现** | 1.4.3 实测；10 Hz；含 inWorld；无 Python、无额外进程 |
| meta | 能力协商 | **已实现**（对象形式） | trail/save 能力位已在其中 |
| save | summary/slots | **已实现** | 位置/savedAt/槽位；心/耐力/卢比仍为 null（planned） |
| trail | 内存缓冲 | **已实现** | 网关内存 2 Hz、10 分钟、重启即清 |
| control | teleport/waypoint/ping | planned | 默认关闭，token + 备份 |
| world/inventory/progress | — | reserved | 按需 |

## 13. 外部消费者接入清单（给 wiki 侧 agent）

1. 游戏运行后自动有服务：读 `<data>/gateway.json` 拿 URL（或默认 17862）；**不需要启动任何命令**。
   若游戏没开、你又需要测试，可用开发桥 `tools/maplinkd/maplinkd.py` 读取文件。
2. `GET /v1/meta`，记录 `api` 与 `capabilities`；缺哪个能力就不显示对应 UI。
3. 位置：`GET /v1/stream`（SSE）或轮询 `/v1/state`；只信 `status`。
4. 存档：`GET /v1/save/summary?slot=latest`，按 `savedAt` 显示"上次存档"。
5. 轨迹：`GET /v1/trail?seconds=120`，自己决定是否绘制。
6. 传送：只在 `capabilities.control.teleport` 为真、用户确认且你已提示风险时调用；处理 403/409/423/504。
7. 坐标换算、地图投影、上传、登录都在你那边；游戏侧不参与。

## 14. 下一步

已实现的能力位与下一批候选（轨迹、存档摘要、控制域、世界信息）的优先级、验收与风险见
[docs/next-features-plan.md](next-features-plan.md)。

## 15. 待定问题

1. 存档摘要第一批字段（建议：位置、时长、心/耐力、卢比）。
2. 传送是否进入产品（风险高）；还是只做 `waypoint`（游戏内标记，副作用小）。
3. 轨迹由网关内存缓冲是否够用（重启丢失），还是需要落到本地文件（仍不上传）。
4. 是否需要 `GET /v1/events`（控制结果的 SSE 推送），还是同步返回足够。
5. BotW 是否在本契约 v1 内一并支持（字段已有 `game`，语义需按版本定义）。
