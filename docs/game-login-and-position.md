# 游戏登录与实时位置：对接设计（给 wiki 服务端与游戏启动器）

> 状态：设计文档，未实现。游戏侧的只读位置导出器与本地桥 `maplinkd` 已完成（见
> [docs/maplink.md](maplink.md)、[docs/maplink-interfaces.md](maplink-interfaces.md)），本文描述**下一步**：
> 启动登录窗、账号体系对接、登录后的实时定位如何从游戏走到 wiki 服务器，再走到玩家自己的地图页面。

## 1. 目标与边界

1. 玩家打开游戏时，启动器先显示登录窗：默认连接**本地 wiki 服务端**（`http://127.0.0.1:3000`），也可以改成线上地址；
   可以账号密码登录，也可以**单机游玩**（不登录）。
2. 登录后，游戏把玩家实时位置（10 Hz 采样，上传可降频）关联到这个 wiki 账号；玩家在 wiki 互动地图上能实时看到
   自己的位置，**只能看到自己的**（用户隔离）。
3. 未登录 / 单机模式：游戏照常玩，本地仍会写 `maplink.json`（同机工具可读），但不上传、不在 wiki 上显示。
4. 游戏只上传位置与少量会话信息；**不上传存档、物品、进度、按键、密钥**，也不读取 wiki 的任何资料。

## 2. 总体架构

```
┌────────────────────────── Mac（玩家本机）──────────────────────────┐
│  游戏 App                                                            │
│   ├─ TOTK 启动器（ObjC，AppKit）                                     │
│   │    ├─ 登录窗：服务器地址 / 账号密码 / 记住我 / 单机游玩           │
│   │    ├─ Keychain：refresh token（系统钥匙串，不落明文）              │
│   │    ├─ Wiki 客户端：HTTPS 登录/刷新、WSS 位置上传、心跳、重连       │
│   │    └─ 启动游戏进程，读 <data>/maplink.json（游戏写、只读）          │
│   └─ totk-native（运行时）→ maplink.json（10 Hz，原子替换）           │
│                                                                      │
│  （可选）maplinkd：本地 HTTP/SSE 桥，给同机的开发者/工具用；          │
│    App 本身不需要 Python，上传由启动器的原生代码完成。                 │
└──────────────────────────────────────────────────────────────────────┘
                              │ HTTPS / WSS（Bearer token）
                              ▼
┌──────────────────────── wiki 服务端（Next.js + Postgres + Redis）────┐
│  认证：better-auth（邮箱密码，后续可加 2FA）                           │
│  游戏 API /api/game/v1/*：登录、刷新、会话、位置上传（WSS）             │
│  用户 API /api/me/player*：地图页读取自己的位置（SSE）                 │
│  Redis：presence:{userId}、pos:{userId}、频道 player:{userId}          │
└──────────────────────────────────────────────────────────────────────┘
                              ▲
                              │ 同源 Cookie（better-auth 会话）
                      玩家浏览器（wiki 地图页）
```

两条数据通道，按场景选择：

| 通道 | 谁读 | 适用 | 说明 |
|---|---|---|---|
| **A. 本机直读桥** | 同机浏览器/工具 | 本地开发、单机模式 | 读取 `http://127.0.0.1:17862/v1/stream`。注意：线上 https 页面不能调 `http://127.0.0.1`（混合内容），仅本地 http 页面可用 |
| **B. 服务器中继**（推荐长期） | 任何设备上的 wiki 页面 | 登录后、线上、多设备 | 游戏 → WSS 上传 → Redis → 玩家自己的地图页（SSE）。跨设备、跨网络都成立 |

**结论**：登录后的正式功能走 **B**；A 保留给本地调试与单机模式。

## 3. 启动器登录窗（游戏端 UX）

```
┌───────────────────────────────────────────────┐
│  Tears of the Kingdom                       ⓘ │
│                                               │
│  服务器  [ http://127.0.0.1:3000        ] [检测]│
│          ● 已发现本地 wiki 服务                 │
│                                               │
│  邮箱    [ you@example.com              ]      │
│  密码    [ ••••••••                     ]      │
│                                               │
│  ☑ 记住我        ☑ 登录后在地图上共享实时位置   │
│                                               │
│  [      登 录      ]   [ 单机游玩 ]            │
│                                               │
│  上次登录：2026-10-21 09:12（Yunnn）           │
└───────────────────────────────────────────────┘
```

行为：

1. **默认服务器**：`TOTK_WIKI_SERVER` 环境变量 > 上次使用的地址 > 探测 `http://127.0.0.1:3000/api/game/v1/health`
   （2 秒超时）> 编译期默认线上地址。检测结果用一句话显示（"已发现本地 wiki 服务" / "无法连接，可继续单机"）。
2. **账号密码**：点"登录"后按钮转圈，失败显示服务端原文（限 `error.message`），不显示内部细节；连续失败由服务端限流。
3. **记住我**：勾选后保存 refresh token 到 Keychain（服务名 `org.macnativegames.totk.wiki`，账号 = 服务器 URL + 用户 id），
   用户名字/服务器地址存 `NSUserDefaults`。下次启动若 token 有效则"自动登录中…"，失败回到登录窗。
4. **单机游玩**：不请求网络，直接启动游戏；不显示"已连接"，地图页显示"游戏未连接"。
5. **登录成功**：窗口关闭并启动游戏；启动器转后台，负责 WSS 上传与心跳；退出游戏时发送会话结束。
6. **离线/断网**：启动器在登录窗/后台显示状态；已登录但服务器掉线时游戏继续玩，位置缓存最近 10 秒，重连后只补发最新点。

**安全底线**：密码只发给服务端（TLS），不落盘、不写日志；token 只进 Keychain；窗口关闭后清空密码字段内存。
`http://` 只允许回环地址，非回环且非 https 时给红色警告并要求确认。

## 4. 认证协议

### 4.1 方案选择

| 方案 | 优点 | 代价 | 建议 |
|---|---|---|---|
| A. 启动器内账号密码 → 双 token | 符合需求，UX 最短 | 游戏端要建表单/错误处理；需防暴力破解 | **P1 采用** |
| B. 浏览器 OAuth / 设备码 | 不在游戏里输入密码，可复用网页登录/2FA | 多一步跳浏览器；游戏端要轮询 | P2 可选 |
| C. 内嵌 WKWebView 登录页 | 复用网页 UI | cookie/跨域/安全边界复杂 | 不推荐 |

P1 用 A；token 体系按"短期 access + 长期 refresh（轮换）"设计，后续接 B 或 2FA 时协议不变。

### 4.2 Token

- `accessToken`：JWT（或随机串 + Redis 映射），**15 分钟**，`Authorization: Bearer`。
- `refreshToken`：随机 256 bit，**30 天**，服务端只存 SHA-256 哈希；每次刷新**轮换**，检测复用（复用即吊销该设备全部 token）。
- scope：`game:presence`、`game:position`；不授予 wiki 写权限。
- 设备：`deviceId`（启动器生成 UUID，存 `NSUserDefaults`）+ `deviceName`（Mac 型号）+ `gameVersion`。
- 吊销：登出 / 改密码 / 用户封禁 → refresh 失效；服务端可主动 `session.revoke`。

### 4.3 接口

```
POST /api/game/v1/auth/login
  { "email": "...", "password": "...", "deviceId": "uuid", "deviceName": "MacBook Pro", "gameVersion": "1.4.3" }
  200 { "accessToken": "...", "refreshToken": "...", "expiresIn": 900,
        "user": { "id": "...", "name": "Yunnn", "avatarUrl": null } }
  401 { "error": { "code": "invalid-credentials", "message": "邮箱或密码不正确" } }
  429 { "error": { "code": "rate-limited", "retryAfter": 30 } }

POST /api/game/v1/auth/refresh
  { "refreshToken": "..." }
  200 { "accessToken": "...", "refreshToken": "...", "expiresIn": 900 }
  401 { "error": { "code": "invalid-refresh" } }        # 客户端回到登录窗

POST /api/game/v1/auth/logout
  { "refreshToken": "..." }        → 204

GET  /api/game/v1/me               (Bearer) → { "user": {...}, "settings": { "sharePosition": true } }
```

服务端实现要点：

- 复用 better-auth 的邮箱密码校验（服务端调用 `signInEmail` 或直接查同一张用户表 + 同样的哈希参数），**不要另做一套密码**。
- 登录限流：按 IP + 账号，5 次/分钟；失败 10 次锁 15 分钟；只记哈希与计数，不记明文。
- refresh 表：`game_refresh_token(id, user_id, device_id, token_hash, expires_at, revoked_at, rotated_from)`。
- 返回体永不包含 `password`、内部 id 之外的管理字段。

## 5. 会话与在场

```
POST /api/game/v1/session         (Bearer)
  { "deviceId": "uuid", "gameVersion": "1.4.3", "sharePosition": true }
  200 { "sessionId": "s_...", "position": { "socketUrl": "wss://wiki/api/game/v1/position",
                                            "protocol": "macnativegames.maplink/1", "rateHz": 2 } }

POST /api/game/v1/session/{id}/heartbeat   (Bearer) → 204      # 也可由 WSS ping 代替
DELETE /api/game/v1/session/{id}           (Bearer) → 204      # 退出游戏时
```

- 一个用户可有多台设备在线；地图页只显示"最近活跃"的那台，UI 可切换（P2）。
- 心跳 15 s；服务端 `presence:{userId}` TTL 30 s，超时即"离线"。
- 会话开始后若 `sharePosition=false`，服务端不接收/立即丢弃位置，UI 显示"未共享位置"。

## 6. 位置上传（游戏 → wiki）

### 6.1 数据

上传的是 `maplink.json` 的裁剪版（每帧不需要全部字段）：

```json
{ "seq": 10231, "t": 1792547577109, "x": 0.25, "y": -13.753, "z": 32.21,
  "yaw": 3.0, "headingSource": "actor", "inWorld": true, "layer": null }
```

- 频率：**2 Hz**（地图足够；本地文件仍是 10 Hz）。`inWorld=false` 时也发，用来让页面切到"菜单/加载中"。
- 只保留最新点：网络抖动时丢旧帧，不排队。
- 断线重连：指数退避 1/2/4/…/30 s；恢复后先发一条 session 心跳再发位置。

### 6.2 传输

首选 WebSocket（双向、延迟低）：

```
WSS /api/game/v1/position
  Header: Authorization: Bearer <accessToken>
  客户端 → 服务端：上面的 JSON，一行一条
  服务端 → 客户端：{ "type": "ack" } 或 { "type": "error", "code": "..." }
  鉴权失败：4001 关闭码；token 过期：客户端刷新后重连
```

HTTP 回退（某些网络禁 WS）：`POST /api/game/v1/position`，body `{ "sessionId": "...", "samples": [ ... 最多 60 条 ] }`。

### 6.3 服务端处理

```
校验 Bearer → userId
校验 session 属于 userId 且未过期、sharePosition
Redis: pos:{userId} = sample (TTL 120s)
Redis: presence:{userId} = {sessionId, deviceId, updatedAt} (TTL 30s)
PUBLISH player:{userId} sample
（可选）Postgres 只记"会话开始/结束"审计；位置默认不入库
```

## 7. 地图页读取（wiki → 浏览器）

复用现有 better-auth 的 Cookie 会话，同源，无 CORS：

```
GET /api/me/player          (Cookie) → {
  "online": true,
  "session": { "deviceName": "MacBook Pro", "gameVersion": "1.4.3" },
  "updatedAt": "2026-10-21T09:53:00Z",
  "settings": { "sharePosition": true },
  "last": { "x": 0.25, "y": -13.753, "z": 32.21, "yaw": 3.0, "inWorld": true, "layer": null }
}

GET /api/me/player/stream   (Cookie, SSE) →
  event: hello    data: {"rateHz": 2}
  event: position data: {"seq":..., "x":..., "z":..., "yaw":..., "inWorld":true}
  event: status   data: {"online": false, "reason": "game-exited"}
```

- 只能读**自己**的 `player:{userId}`，接口不接受 userId 参数；越权一律 403。
- 页面在 `visibilitychange` 隐藏时断开 SSE，回到前台重连并拉一次 `/api/me/player`。
- 地图坐标换算仍由页面完成（现有标定：`pixelsPerGameUnit=2`，原点在 24000×2000 栅格中心）。

## 8. 时序（登录 → 定位）

```
启动器            token 服务           wiki API            Redis           地图页(浏览器)
  │ --login------->│                    │                   │                 │
  │<--tokens-------│                    │                   │                 │
  │ --session---------------------------->│                   │                 │
  │<--{sessionId, wsUrl}-----------------│                   │                 │
  │ 启动游戏，读 maplink.json            │                   │                 │
  │ --WSS position (2Hz)---------------->│--pos/presence---->│                 │
  │                                      │--PUBLISH player:{uid}->│            │
  │                                      │                   │--SSE position-->│
  │ --HTTP /session/{id} DELETE（退出）-->│--offline--------->│                 │
  │                                      │                   │--status-------->│
```

## 9. 安全与隐私

- **传输**：线上强制 HTTPS/WSS；只允许回环地址用明文 http（本地服务端）。
- **密码**：不进游戏日志、不进崩溃报告、不落盘；登录请求只发一次。
- **Token**：Keychain；`accessToken` 只在内存；刷新失败即回登录窗；登出删 token 并通知服务端吊销。
- **用户隔离**：位置读写都绑定 Cookie/Bearer 解出的 userId；任何接口都不接受外部传入的 userId。
- **频率控制**：位置上传 2 Hz、单连接；每用户最多 3 个活跃会话；超限拒绝新会话。
- **隐私开关**：登录窗的"共享实时位置"默认**开**（这是功能价值），但必须可关；关闭后服务端不存位置、页面显示未共享。
- **保留策略**：位置默认只存"最新一条"，Redis TTL 120 s；会话审计（开始/结束/设备）保留 30 天；不做长期轨迹，除非以后单独开关。
- **数据最小化**：绝不读取存档/物品/进度；`maplink.json` 由游戏只读产生，上传端也只读它。

## 10. 失败与边界

| 场景 | 行为 |
|---|---|
| 启动时服务器不可达 | 登录窗显示"无法连接"，可重试或单机游玩；不阻塞启动 |
| 登录 401 | 显示服务端消息；保留输入（密码清空） |
| token 过期 | 静默 refresh；失败 → 回登录窗 |
| 游戏中断网 | 游戏不受影响；启动器重连，地图页 `online:false` |
| 游戏崩溃/强退 | WSS 断开 → 30 s 内 presence 过期 → 页面显示离线 |
| 多设备同账号 | 面板取最近活跃设备；可在页面切换（P2） |
| 服务器迁移 | 登录窗可改地址；记住上次地址 |

## 11. 落地顺序与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| L1 | 启动器登录窗（本地服务默认 + 单机 + 记住我 + Keychain） | 断网/错密码/无服务端都能给出明确状态并进入单机 |
| L2 | wiki：`/auth/login`、`/auth/refresh`、`/session`（复用 better-auth） | curl 登录→刷新→会话全通；refresh 轮换与复用检测生效 |
| L3 | 启动器 WSS 上传 + 心跳 + 重连；Redis 写入 | 游戏进世界后 2 s 内 Redis 有 `pos/{presence}`；断网重连不积压 |
| L4 | wiki 地图页 SSE + "我的位置"图层 + 在线状态 | 同一账号页面 200 ms 内跟随；退出游戏 30 s 内显示离线；别的账号看不到 |
| L5（可选） | 设备码登录 / 2FA / 多设备切换 / 隐私开关面板 | 按需 |

## 12. 待确认

1. 线上 wiki 的域名与是否为 https（决定是否必须先做服务器中继）。
2. 位置共享默认开还是默认关；是否需要在游戏内提供开关（不只是登录窗）。
3. 是否需要保留短轨迹（如最近 10 分钟）供地图画线，还是只显示当前点。
4. 多设备同时在线时，地图页显示策略（最近活跃 / 让用户选）。
5. 是否需要把"游戏在线"作为 wiki 的社交功能（好友可见），还是严格只有自己可见。
