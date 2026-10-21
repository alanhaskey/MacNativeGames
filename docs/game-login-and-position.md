# 游戏登录与实时位置：对接设计（独立连接器 + wiki 服务端）

> 状态：设计已按 2026-10-21 的决策收敛。游戏侧只读位置导出器与本地桥 `maplinkd` 已完成
> （见 [docs/maplink.md](maplink.md)、[docs/maplink-interfaces.md](maplink-interfaces.md)）。
> 登录与上传放在**独立的连接器 App**里，不改游戏的启动流程，也不随游戏分发。

## 0. 已确认的决策

| # | 决策 | 影响 |
|---|---|---|
| 1 | **线上 wiki 是 https** | 正式通道只能是 **WSS/HTTPS 中继**；浏览器不直连 `http://127.0.0.1`（混合内容被拦）。本机直读桥只留给本地 http 开发与工具 |
| 2 | **位置共享默认开** | 连接器登录窗"共享实时位置"默认勾选；服务端默认 `sharePosition=true`；可关，关后不存、不推 |
| 3 | **暂不做轨迹** | 只保留"最新一个点"；不做 `/trail`、不入库、不画线 |
| 4 | **不允许多设备游戏客户端** | 同一账号同时只有一个游戏会话；新会话顶掉旧会话（服务端也可改为拒绝，见 §5.3） |
| 5 | **不做社交，仅自己可见** | 没有好友/公开接口；`/api/me/player*` 只能读自己 |
| 6 | **启动器与游戏解耦** | 游戏 App 只保留只读位置导出器（已实现）；登录、token、上传、状态 UI 全部在**独立连接器 App** 里，另行分发与更新 |
| 7 | 不做游戏分发 | 玩家自己构建游戏 App（一次）；连接器由 wiki 侧分发，登录/上传逻辑更新不需要重打包 17 GB 的游戏 |

## 1. 目标与边界

1. 玩家构建一次游戏 App（含只读导出器，写 `maplink.json`）；平时用启动器/游戏本体直接玩，**游戏里没有登录窗**。
2. 想要"地图实时定位"时，玩家额外运行一个小的**菜单栏连接器 App**（下称 Wiki Link）：登录 wiki 账号，连接器把
   游戏写的位置上传到 wiki；互动地图上只显示玩家自己的位置。
3. 不登录 / 不运行连接器：游戏照常玩（单机），本地仍有 `maplink.json`（同机工具可读），wiki 显示"游戏未连接"。
4. 连接器只上传位置与会话信息；**不上传存档、物品、进度、按键、密钥**，也不读取 wiki 资料。

## 2. 总体架构（解耦后）

```
┌──────────────────── Mac（玩家本机）─────────────────────────────────┐
│                                                                     │
│  游戏 App（玩家自己构建，与上游差异只有只读导出器）                   │
│   └─ totk-native ──写──▶ <data>/maplink.json（10 Hz，原子替换）       │
│                              │                                       │
│                              │ 只读文件（路径与 schema 是唯一契约）     │
│                              ▼                                       │
│  Wiki Link（独立菜单栏 App，wiki 侧分发/签名/公证/自动更新）          │
│   ├─ 登录窗：服务器地址 / 账号密码 / 记住我 / 共享位置 / 退出          │
│   ├─ Keychain：refresh token；HTTPS 登录与刷新                        │
│   ├─ 上传线程：读 maplink.json，2 Hz WSS 上传、心跳、重连、4009 处理    │
│   └─ 菜单栏状态：未登录 / 等待游戏 / 已连接 / 已在别处登录             │
│                                                                     │
│  （仅本地开发）maplinkd：HTTP/SSE 桥，给同机 http 页面/工具用          │
└─────────────────────────────────────────────────────────────────────┘
                              │ HTTPS / WSS（Bearer token）
                              ▼
┌──────────────────── wiki 服务端（Next.js + Postgres + Redis）────────┐
│  better-auth：账号密码                                            │
│  /api/game/v1/*：登录、刷新、会话、位置上传（WSS）                    │
│  /api/me/player*：地图页读取自己的位置（SSE，Cookie，仅本人）         │
│  Redis：presence:{userId}、pos:{userId}、频道 player:{userId}         │
└─────────────────────────────────────────────────────────────────────┘
                              ▲
                              │ 同源 Cookie（better-auth 会话）
                      玩家浏览器（wiki 地图页，仅本人）
```

**唯一的耦合点**：`~/Library/Application Support/MacNativeGames/TOTK/maplink.json` 的路径与 JSON schema（v1 已冻结）。
不读游戏内存、不注入、不要求游戏进程配合；连接器和游戏**启动顺序无关**。

### 2.1 为什么解耦（针对"不做游戏分发"）

| 维度 | 嵌入式（登录窗在游戏里） | **独立连接器（采用）** |
|---|---|---|
| 更新登录/上传逻辑 | 每次都要重新打包 17 GB 游戏 App（3–17 分钟/人） | 只更新连接器 App（几 MB） |
| 分发 | 需要分发改过的游戏 App | 游戏由玩家自建；连接器单独分发 |
| 服务器地址/错误提示迭代 | 绑死游戏发版 | 连接器独立发版 |
| 启动顺序 | 必须先开游戏 | 先开哪个都行，自动发现 |
| 多游戏复用（BotW 等） | 每个游戏都要改启动器 | 连接器只认 schema，天然复用 |
| 代价 | — | 用户装两个 App；需要首次授权/自动启动 |

游戏侧唯一要求：构建时包含只读导出器（本仓库已完成，`TOTK_MAPLINK=0` 可关）。这属于"一次构建"，不是每次登录都要动。

### 2.2 连接器形态选择

| 形态 | 优点 | 代价 | 建议 |
|---|---|---|---|
| **菜单栏 App**（推荐） | 常驻、状态可见、登录窗自然、可自动启动 | 要签名/公证 | 采用 |
| CLI 守护进程 `wikilinkd` | 无 UI、可脚本化、CI 好测 | 玩家要开终端登录 | 作为 App 的 `--headless` 模式 |
| 浏览器标签页 | 零安装 | https 页面不能读本机文件/桥；登录态与进程管理复杂 | 不采用 |

## 3. 连接器 UX

```
菜单栏图标：○ 未登录   ◐ 等待游戏   ● 已连接（1.4.3）   ⚠ 已在别处登录
点开菜单：
  ─ 状态：等待游戏启动 / 已连接，正在上传 / 游戏未运行
  ─ 账号：Yunnn（wiki.example.com）      [退出登录]
  ─ ☑ 共享实时位置（本次会话）
  ─ ☑ 登录时启动（开机自启）
  ─ 打开我的地图
  ─ 开关：启动/停止上传          [设置…]（服务器地址）
```

首次启动显示登录窗：

```
┌───────────────────────────────────────────────┐
│  Wiki Link — 连接你的地图                       │
│                                               │
│  服务器  [ https://wiki.example.com     ] [检测]│
│          ● 本地 wiki 服务已发现（127.0.0.1）    │
│                                               │
│  邮箱    [ you@example.com              ]      │
│  密码    [ ••••••••                     ]      │
│                                               │
│  ☑ 记住我        ☑ 登录后在地图上共享实时位置   │
│                                               │
│  [      登 录      ]   [ 暂不连接 ]            │
└───────────────────────────────────────────────┘
```

行为：

1. **默认服务器**：`TOTK_WIKI_SERVER` > 上次地址 > 探测 `http://127.0.0.1:3000/api/game/v1/health`（2 秒超时）
   > 连接器内配置的线上地址。检测结果一句话显示。
2. **登录**：按钮转圈；失败显示服务端 `error.message`；服务端限流。
3. **记住我**：refresh token 进 Keychain（服务 `com.<wiki>.wikilink`，账号 = 服务器 URL + userId）；地址/邮箱进 `NSUserDefaults`。
4. **共享位置**：默认勾选（决策 2）；取消则本次会话 `sharePosition=false`。
5. **暂不连接**：连接器退到菜单栏"未登录"，不请求网络；游戏不受影响。
6. **自动发现游戏**：每 2 秒 stat `maplink.json`；`mtime` 5 秒内有更新视为"游戏在运行"，`inWorld` 决定上传内容；
   文件不存在或长期不更新显示"游戏未运行/已退出"。
7. **单设备**：连接器收到 409 或 WSS 4009 时，菜单栏/通知提示"账号已在另一台设备开始游戏"，停止上传并等待用户处理
   （游戏本身不受影响）。
8. **退出登录**：删 Keychain、吊销 refresh、停止上传、清除菜单栏账号。

**安全底线**：密码只发服务端（TLS），不落盘、不写日志；token 只进 Keychain；`http://` 仅允许回环地址。

## 4. 认证协议

### 4.1 Token

- `accessToken`：JWT，**15 分钟**，`Authorization: Bearer`。
- `refreshToken`：随机 256 bit，**30 天**，服务端只存 SHA-256 哈希；每次刷新轮换，检测复用（复用即吊销）。
- scope：`game:presence`、`game:position`。
- 设备：`deviceId`（连接器生成 UUID，存 `NSUserDefaults`）+ `deviceName` + `gameVersion`（取自 `maplink.json`）。

### 4.2 接口

```
POST /api/game/v1/auth/login
  { "email": "...", "password": "...", "deviceId": "uuid", "deviceName": "MacBook Pro", "gameVersion": "1.4.3" }
  200 { "accessToken": "...", "refreshToken": "...", "expiresIn": 900,
        "user": { "id": "...", "name": "Yunnn", "avatarUrl": null } }
  401 { "error": { "code": "invalid-credentials", "message": "邮箱或密码不正确" } }
  423 { "error": { "code": "account-locked", "retryAfter": 900 } }
  429 { "error": { "code": "rate-limited", "retryAfter": 30 } }

POST /api/game/v1/auth/refresh
  { "refreshToken": "..." } → 200 { "accessToken", "refreshToken", "expiresIn" }
  401 { "error": { "code": "invalid-refresh" } }          # 连接器回登录窗

POST /api/game/v1/auth/logout
  { "refreshToken": "..." } → 204

GET  /api/game/v1/me (Bearer) → { "user": {...}, "settings": { "sharePosition": true } }
```

服务端要点：

- 复用 better-auth 的邮箱密码校验，不另做一套密码。
- 登录限流：IP + 账号 5 次/分钟；失败 10 次锁 15 分钟；只存哈希与计数。
- refresh 表：`game_refresh_token(id, user_id, device_id, token_hash, expires_at, revoked_at, rotated_from)`。

### 4.3 错误码

| HTTP | code | 连接器行为 |
|---|---|---|
| 400 | `invalid-request` | 通用错误 |
| 401 | `invalid-credentials` | 提示邮箱或密码错误 |
| 401 | `invalid-refresh` | 清 Keychain，回登录窗 |
| 409 | `already-playing` | 提示在另一台设备游玩；提供"接管"或"暂不连接"（§5.3） |
| 423 | `account-locked` | 显示解锁时间 |
| 429 | `rate-limited` | 显示 `retryAfter` |

## 5. 会话与在场

```
POST /api/game/v1/session         (Bearer)
  { "deviceId": "uuid", "gameVersion": "1.4.3", "sharePosition": true }
  200 { "sessionId": "s_...", "position": { "socketUrl": "wss://wiki/api/game/v1/position",
                                            "protocol": "macnativegames.maplink/1", "rateHz": 2 } }
  409 { "error": { "code": "already-playing", "session": {...}, "canTakeOver": true } }
POST /api/game/v1/session/{id}/heartbeat (Bearer) → 204
DELETE /api/game/v1/session/{id}         (Bearer) → 204
```

连接器**在检测到游戏运行时**才建会话；游戏退出（文件 5 秒不更新）→ 发 DELETE 结束会话。

- 心跳 15 s；`presence:{userId}` TTL 30 s。
- 位置 `pos:{userId}` TTL 120 s；退出/超时后页面显示断开，不留历史。
- `sharePosition=false`：不写 `pos`，仅保留 presence。

### 5.3 单设备（决策 4）

同一账号同时只允许一个游戏会话；两种行为，服务端二选一，连接器都要处理：

- **默认：新会话顶掉旧会话** —— 旧连接器收到 WSS `4009`，提示"账号已在另一台设备开始游戏"，停止上传；
  旧设备的游戏继续本地运行。
- **可选：拒绝新会话** —— 返回 409 `already-playing`；连接器显示"另一台设备正在游玩"，提供"接管"（带 `takeover:true` 重试）
  或"暂不连接"。

浏览器访问 wiki 不受限，但只有一台游戏客户端在上传，且只有本人可见。

## 6. 位置上传（连接器 → wiki）

### 6.1 数据

```json
{ "seq": 10231, "t": 1792547577109, "x": 0.25, "y": -13.753, "z": 32.21,
  "yaw": 3.0, "headingSource": "actor", "inWorld": true, "layer": null }
```

- 频率 **2 Hz**；本地文件仍 10 Hz。`inWorld=false` 也发（页面显示菜单/加载中）。
- 只保留最新点，不排队（决策 3）；断线退避 1/2/4/…/30 s，恢复后先心跳再发位置。
- 位置样本来自文件；连接器只读，不需要游戏配合。

### 6.2 传输

```
WSS /api/game/v1/position
  Header: Authorization: Bearer <accessToken>
  客户端 → 服务端：上面的 JSON，一行一条
  服务端 → 客户端：{ "type": "ack" } / { "type": "error", "code": "..." }
  关闭码：4001 未授权、4009 被新会话接管、4010 会话过期
```

HTTP 回退：`POST /api/game/v1/position`，body `{ "sessionId": "...", "samples": [ ... 最多 60 条 ] }`。

### 6.3 服务端处理

```
校验 Bearer → userId
校验 session 属于 userId、未过期、未被 superseded、sharePosition
Redis: pos:{userId} = sample (TTL 120s)          # 只有最新一条
Redis: presence:{userId} = {sessionId, deviceId, updatedAt} (TTL 30s)
PUBLISH player:{userId} sample
（审计）Postgres 只记会话开始/结束/设备，不记位置
```

## 7. 地图页读取（wiki → 浏览器，仅本人）

```
GET /api/me/player (Cookie) → {
  "online": true, "session": { "deviceName": "MacBook Pro", "gameVersion": "1.4.3" },
  "updatedAt": "...", "settings": { "sharePosition": true },
  "last": { "x": 0.25, "y": -13.753, "z": 32.21, "yaw": 3.0, "inWorld": true, "layer": null }
}
GET /api/me/player/stream (Cookie, SSE) →
  event: hello    data: {"rateHz": 2}
  event: position data: {"seq":..., "x":..., "z":..., "yaw":..., "inWorld":true}
  event: status   data: {"online": false, "reason": "game-exited" | "superseded" | "not-sharing"}
```

- 只能读自己的 `player:{userId}`；接口不接受 userId；越权 403；无社交端点。
- 页面隐藏时断 SSE，回前台重连并拉一次快照。
- 坐标换算由页面完成（现有标定：`pixelsPerGameUnit=2`，原点在 24000×20000 栅格中心）。

## 8. 时序

```
连接器               token 服务        wiki API          Redis        地图页(浏览器)
  │ --login---------->│                 │                 │              │
  │<--tokens----------│                 │                 │              │
  │ 轮询 maplink.json（2 s）→ 游戏在运行                                    │
  │ --session (device A)--------------->│                 │              │
  │<--{sessionId, wsUrl}----------------│                 │              │
  │ --WSS position (2Hz)--------------->│--pos/presence-->│              │
  │                                     │--PUBLISH player:{uid}->│        │
  │                                     │                 │--SSE-------->│
  │ 另一台设备 B 登录并连接               │                 │              │
  │                                     │<--session (B)    │              │
  │<--WSS close 4009（A 停止上传）-------│                 │              │
  │ 游戏退出（文件不再更新）→ DELETE session  │              │              │
  │                                     │--offline------->│--status----->│
```

## 9. 安全与隐私

- 传输：线上 HTTPS/WSS；仅回环允许 http。
- 密码：不进日志/崩溃报告/磁盘；只发一次。
- Token：Keychain；access 只在内存；刷新失败回登录窗；登出吊销。
- 用户隔离：读写都按解出的 userId；不接受外部 userId。
- 单设备：§5.3。
- 频率：上传 2 Hz 单连接；每用户最多 3 个浏览器 SSE。
- 隐私：共享默认开（决策 2），可关；关后不写 `pos`。
- 保留：位置只存最新一条（TTL 120 s），**无轨迹**；会话审计 30 天。
- 数据最小化：连接器只读 `maplink.json`；不碰存档/物品/进度。

## 10. 失败与边界

| 场景 | 行为 |
|---|---|
| 连接器启动时服务器不可达 | 登录窗提示，可重试或"暂不连接" |
| 游戏未运行 | 菜单栏"等待游戏"；登录态保持，不发会话 |
| 游戏退出/崩溃 | 文件 5 s 不更新 → DELETE 会话 → 页面离线 |
| 游戏在菜单/加载 | `inWorld:false`，页面显示"菜单/加载中" |
| token 过期 | 静默 refresh；失败回登录窗 |
| 断网 | 连接器退避重连；游戏不受影响 |
| 第二台设备登录 | 默认顶掉旧会话（4009）；旧端提示并停止上传；可选 409+接管 |
| 服务器迁移 | 连接器设置里改地址，记住 |
| 玩家只想要单机 | 不运行连接器（或退出登录），游戏完全不受影响 |

## 11. 落地顺序与验收

| 阶段 | 内容 | 归属 | 验收 |
|---|---|---|---|
| L0 | 游戏只读导出器 + `inWorld` +（可选）本地桥 | 游戏侧（**已完成**） | 进世界后 `maplink.json` 10 Hz 更新；菜单 `ok:false` |
| L1 | 连接器骨架：菜单栏 App、登录窗、Keychain、服务器探测、状态显示 | wiki 侧（我可以先做参考实现） | 断网/错密码/无服务端都有明确状态；不登录也不影响游戏 |
| L2 | wiki：`/auth/login`、`/auth/refresh`、`/session`（复用 better-auth） | wiki 侧 | curl 登录→刷新→会话全通；refresh 轮换/复用检测 |
| L3 | 连接器：2 Hz WSS 上传 + 心跳 + 重连 + 4009/409 | wiki 侧 | 游戏运行时 2 s 内 Redis 有 pos/presence；第二台设备 1 s 内让旧端停止 |
| L4 | wiki 地图页 SSE + "我的位置"图层 + 在线/未共享状态 | wiki 侧 | 本人页面 200 ms 内跟随；退出 30 s 内离线；他人不可见 |

无 L5（无社交、无轨迹）；后续可选：设备码登录、2FA。

## 12. 决策记录

2026-10-21 已确认：线上 https；位置共享默认开；不做轨迹；不允许多设备游戏客户端；不做社交（仅自己可见）；
**登录器与游戏解耦，登录/上传在独立连接器 App，游戏只保留只读导出器**。

实现时仍需确认的细节：单设备取"顶掉旧会话"还是"拒绝新会话"（§5.3，默认前者）；连接器的名称/签名/分发方式；
是否需要 CLI `--headless` 模式。
