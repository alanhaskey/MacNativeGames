# 游戏登录与实时位置：对接设计（给 wiki 服务端与游戏启动器）

> 状态：设计已按 2026-10-21 的决策收敛，实现未开始。游戏侧只读位置导出器与本地桥 `maplinkd` 已完成
> （见 [docs/maplink.md](maplink.md)、[docs/maplink-interfaces.md](maplink-interfaces.md)）。

## 0. 已确认的决策

| # | 决策 | 影响 |
|---|---|---|
| 1 | **线上 wiki 是 https** | 正式通道只能是 **WSS/HTTPS 中继**；浏览器不直连 `http://127.0.0.1`（混合内容被拦）。本机直读桥只保留给本地 http 开发与工具 |
| 2 | **位置共享默认开** | 登录窗"共享实时位置"默认勾选；服务端默认 `sharePosition=true`；可关，关后服务端不存、不推 |
| 3 | **暂不做轨迹** | 只保留"最新一个点"；不做 `/trail`、不入库、不画线 |
| 4 | **不允许多设备游戏客户端** | 同一账号同时只能有一个游戏会话；新会话顶掉旧会话（服务端也可改为拒绝新会话，见 §5.3） |
| 5 | **不做社交，仅自己可见** | 没有任何"好友/公开"接口；`/api/me/player*` 只能读自己；在线状态不暴露给其他用户 |

## 1. 目标与边界

1. 玩家打开游戏时，启动器先显示登录窗：默认连接**本地 wiki 服务端**（`http://127.0.0.1:3000`），也可改成线上 https 地址；
   可以账号密码登录，也可以**单机游玩**。
2. 登录后，游戏把玩家实时位置关联到这个 wiki 账号；玩家在 wiki 互动地图上实时看到自己的位置，
   **只有自己可见**，且同一账号同一时间只有一个游戏客户端在传。
3. 未登录 / 单机模式：游戏照常玩，本地仍写 `maplink.json`（同机工具可读），不上传、不在 wiki 显示。
4. 游戏只上传位置与少量会话信息；**不上传存档、物品、进度、按键、密钥**，也不读取 wiki 的任何资料。

## 2. 总体架构

```
┌────────────────────────── Mac（玩家本机）──────────────────────────┐
│  游戏 App                                                            │
│   ├─ TOTK 启动器（ObjC，AppKit）                                     │
│   │    ├─ 登录窗：服务器地址 / 账号密码 / 记住我 / 共享位置 / 单机游玩 │
│   │    ├─ Keychain：refresh token（系统钥匙串，不落明文）              │
│   │    ├─ Wiki 客户端：HTTPS 登录/刷新、WSS 位置上传、心跳、重连       │
│   │    └─ 启动游戏进程，读 <data>/maplink.json（游戏写、只读）          │
│   └─ totk-native（运行时）→ maplink.json（10 Hz，原子替换）           │
│                                                                      │
│  （仅本地开发）maplinkd：HTTP/SSE 桥，给同机工具/页面用；             │
│    App 本身不需要 Python，上传由启动器的原生代码完成。                 │
└──────────────────────────────────────────────────────────────────────┘
                              │ HTTPS / WSS（Bearer token）
                              ▼
┌──────────────────────── wiki 服务端（Next.js + Postgres + Redis）────┐
│  认证：better-auth（邮箱密码）                                       │
│  游戏 API /api/game/v1/*：登录、刷新、会话、位置上传（WSS）             │
│  用户 API /api/me/player*：地图页读取自己的位置（SSE）                 │
│  Redis：presence:{userId}、pos:{userId}、频道 player:{userId}          │
└──────────────────────────────────────────────────────────────────────┘
                              ▲
                              │ 同源 Cookie（better-auth 会话）
                      玩家浏览器（wiki 地图页，仅本人）
```

由于线上是 https，**正式数据通道只有服务器中继（B）**：

| 通道 | 谁读 | 适用 | 说明 |
|---|---|---|---|
| A. 本机直读桥 | 同机 http 页面/工具 | 仅本地开发、单机模式 | `http://127.0.0.1:17862/v1/stream`；https 页面调它会被混合内容拦截，不作为产品路径 |
| **B. 服务器中继** | 任何设备上的 wiki 页面 | **正式功能** | 游戏 → WSS 上传 → Redis → 本人地图页（SSE）；天然支持换设备、换网络 |

## 3. 启动器登录窗（游戏端 UX）

```
┌───────────────────────────────────────────────┐
│  Tears of the Kingdom                       ⓘ │
│                                               │
│  服务器  [ https://wiki.example.com     ] [检测]│
│          ● 本地 wiki 服务已发现（127.0.0.1）    │
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

1. **默认服务器**：`TOTK_WIKI_SERVER` > 上次使用地址 > 探测 `http://127.0.0.1:3000/api/game/v1/health`（2 秒超时）
   > 编译期默认线上地址。检测结果一句话显示（"已发现本地 wiki 服务" / "无法连接，可单机游玩"）。
2. **账号密码**：点"登录"后按钮转圈；失败显示服务端 `error.message`；连续失败由服务端限流；错误码见 §4.3。
3. **记住我**：refresh token 存 Keychain（服务 `org.macnativegames.totk.wiki`，账号 = 服务器 URL + userId）；
   用户名/服务器地址存 `NSUserDefaults`。下次启动 token 有效则自动登录，失败回登录窗。
4. **共享实时位置**：默认勾选（决策 2）；取消勾选则本次会话 `sharePosition=false`，服务端不存位置，页面显示"未共享"。
5. **单机游玩**：不请求网络，直接启动游戏；不显示已连接；地图页显示"游戏未连接"。
6. **登录成功**：窗口关闭并启动游戏；启动器转后台负责 WSS 上传与心跳；退出游戏时发送会话结束。
7. **单设备**：若该账号已在另一台设备运行游戏，按 §5.3 处理（默认"新会话顶掉旧会话"），旧设备立即收到提示并停止上传。

**安全底线**：密码只发服务端（TLS），不落盘、不写日志；token 只进 Keychain；关闭窗口清空密码字段。
`http://` 仅允许回环地址；非回环且非 https 时红色警告并要求确认。

## 4. 认证协议

### 4.1 方案

P1 采用"启动器内账号密码 → 双 token"。后续可平滑加设备码登录或 2FA，token 协议不变。

- `accessToken`：JWT，**15 分钟**，`Authorization: Bearer`。
- `refreshToken`：随机 256 bit，**30 天**，服务端只存 SHA-256 哈希；每次刷新轮换，检测复用（复用即吊销）。
- scope：`game:presence`、`game:position`；不含 wiki 写权限。
- 设备：`deviceId`（启动器生成 UUID，存 `NSUserDefaults`）+ `deviceName` + `gameVersion`。

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
  { "refreshToken": "..." }
  200 { "accessToken": "...", "refreshToken": "...", "expiresIn": 900 }
  401 { "error": { "code": "invalid-refresh" } }        # 客户端回登录窗

POST /api/game/v1/auth/logout
  { "refreshToken": "..." }        → 204

GET  /api/game/v1/me               (Bearer) → { "user": {...}, "settings": { "sharePosition": true } }
```

服务端要点：

- 复用 better-auth 的邮箱密码校验（服务端 `signInEmail` 或同一张用户表 + 相同哈希参数），不另做一套密码。
- 登录限流：IP + 账号 5 次/分钟；失败 10 次锁 15 分钟；只存哈希与计数。
- refresh 表：`game_refresh_token(id, user_id, device_id, token_hash, expires_at, revoked_at, rotated_from)`。
- **单设备约束落在 refresh/session 层**：同一 userId 只允许一个"活跃游戏会话"，详见 §5.3。

### 4.3 错误码约定

| HTTP | code | 客户端行为 |
|---|---|---|
| 400 | `invalid-request` | 显示通用错误 |
| 401 | `invalid-credentials` | 提示邮箱或密码错误 |
| 401 | `invalid-refresh` | 清 Keychain，回登录窗 |
| 409 | `already-playing` | 提示"该账号正在另一台设备游玩"，提供"接管"或"单机游玩"（见 §5.3） |
| 423 | `account-locked` | 显示解锁时间 |
| 429 | `rate-limited` | 显示 `retryAfter` 秒后重试 |

## 5. 会话与在场

```
POST /api/game/v1/session         (Bearer)
  { "deviceId": "uuid", "gameVersion": "1.4.3", "sharePosition": true }
  200 { "sessionId": "s_...", "position": { "socketUrl": "wss://wiki/api/game/v1/position",
                                            "protocol": "macnativegames.maplink/1", "rateHz": 2 } }
  409 { "error": { "code": "already-playing",
                   "session": { "deviceName": "Mac mini", "startedAt": "..." },
                   "canTakeOver": true } }

POST /api/game/v1/session/{id}/heartbeat   (Bearer) → 204      # 也可由 WSS ping 代替
DELETE /api/game/v1/session/{id}           (Bearer) → 204      # 退出游戏时
```

### 5.1 在场

- 心跳 15 s；服务端 `presence:{userId}` TTL 30 s，超时即离线。
- 位置 `pos:{userId}` TTL 120 s；退出/超时后页面显示"游戏已断开"，不保留历史。
- `sharePosition=false`：服务端不写 `pos`，仅保留 presence（用于"已登录但未共享"的状态）。

### 5.2 会话与账号绑定

- session 属于 userId + deviceId；任何接口都不接受外部传入的 userId。
- 会话开始后，上传必须带 Bearer；token 过期 → 刷新后重连（session 不重建）。

### 5.3 单设备策略（决策 4）

同一账号**同时只允许一个游戏会话**。两种可选行为，服务端二选一，客户端都要能处理：

- **默认：新会话顶掉旧会话（takeover）** —— `POST /session` 成功，旧会话标记 `superseded`，旧 WSS 以关闭码
  `4009`（superseded）断开；旧启动器显示"账号已在另一台设备开始游戏"，停止上传，游戏本身继续运行（本地模式）。
- **可选：拒绝新会话** —— 返回 409 `already-playing`；登录窗展示"另一台设备正在游玩"，并提供"接管"按钮
  （带 `?takeover=1` 或 body `takeover:true` 重试）或"单机游玩"。

浏览器访问 wiki 不受此限制：多设备可以登录网页，但**只有一台游戏客户端**在传位置，且只有账号本人可见。

## 6. 位置上传（游戏 → wiki）

### 6.1 数据

```json
{ "seq": 10231, "t": 1792547577109, "x": 0.25, "y": -13.753, "z": 32.21,
  "yaw": 3.0, "headingSource": "actor", "inWorld": true, "layer": null }
```

- 频率：**2 Hz**（地图足够；本地文件仍 10 Hz）。`inWorld=false` 也发，页面据此显示"菜单/加载中"。
- 只保留最新点：网络抖动时丢旧帧，不排队（决策 3 无轨迹）。
- 断线重连：指数退避 1/2/4/…/30 s；恢复后先发心跳再发位置。

### 6.2 传输

```
WSS /api/game/v1/position
  Header: Authorization: Bearer <accessToken>
  客户端 → 服务端：上面的 JSON，一行一条
  服务端 → 客户端：{ "type": "ack" } / { "type": "error", "code": "..." }
  关闭码：4001 未授权、4009 已被新会话接管、4010 会话过期
```

HTTP 回退：`POST /api/game/v1/position`，body `{ "sessionId": "...", "samples": [ ... 最多 60 条 ] }`。

### 6.3 服务端处理

```
校验 Bearer → userId
校验 session 属于 userId、未过期、未被 superseded、sharePosition
Redis: pos:{userId} = sample (TTL 120s)          # 只有最新一条，无历史
Redis: presence:{userId} = {sessionId, deviceId, updatedAt} (TTL 30s)
PUBLISH player:{userId} sample
（审计，可选）Postgres 只记会话开始/结束/设备，不记位置
```

## 7. 地图页读取（wiki → 浏览器，仅本人）

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
  event: status   data: {"online": false, "reason": "game-exited" | "superseded" | "not-sharing"}
```

- 只能读自己的 `player:{userId}`；接口不接受 userId 参数；越权 403。
- 无社交接口：不提供"查看他人是否在线/在哪"的任何端点。
- 页面 `visibilitychange` 隐藏时断开 SSE，回到前台重连并拉一次 `/api/me/player`。
- 地图坐标换算由页面完成（现有标定：`pixelsPerGameUnit=2`，原点在 24000×20000 栅格中心）。

## 8. 时序（登录 → 定位 → 单设备接管）

```
启动器            token 服务           wiki API            Redis           地图页(浏览器)
  │ --login------->│                    │                   │                 │
  │<--tokens-------│                    │                   │                 │
  │ --session (device A)---------------->│                   │                 │
  │<--{sessionId, wsUrl}-----------------│                   │                 │
  │ --WSS position (2Hz)---------------->│--pos/presence---->│                 │
  │                                      │--PUBLISH player:{uid}->│            │
  │                                      │                   │--SSE position-->│
  │                                      │                   │                 │
  │  另一台设备 B 登录并开游戏            │                   │                 │
  │                                      │<--session (device B)                  │
  │                                      │--- supersede A --->│                 │
  │<--WSS close 4009（A 停止上传）--------│                   │                 │
  │ --DELETE session（退出）------------->│--offline--------->│                 │
  │                                      │                   │--status-------->│
```

## 9. 安全与隐私

- **传输**：线上强制 HTTPS/WSS；只允许回环地址用明文 http（本地服务端）。
- **密码**：不进游戏日志、不进崩溃报告、不落盘；只发一次。
- **Token**：Keychain；access 只在内存；刷新失败回登录窗；登出删 token 并吊销。
- **用户隔离**：读写都由 Cookie/Bearer 解出的 userId 决定；任何接口不接受外部 userId。
- **单设备**：一个游戏会话；超出由 §5.3 处理。
- **频率**：位置上传 2 Hz、单连接；同一账号最多 3 个浏览器 SSE 订阅。
- **隐私**：位置共享默认开（决策 2），但必须可关；关闭后不写 `pos`。
- **保留**：位置只存最新一条（TTL 120 s），**不存轨迹**（决策 3）；会话审计（开始/结束/设备）30 天。
- **数据最小化**：不读存档/物品/进度；`maplink.json` 只读。

## 10. 失败与边界

| 场景 | 行为 |
|---|---|
| 启动时服务器不可达 | 登录窗提示"无法连接"，可重试或单机游玩 |
| 登录 401 | 显示服务端消息；密码清空 |
| token 过期 | 静默 refresh；失败回登录窗 |
| 游戏中断网 | 游戏不受影响；启动器重连，页面 `online:false` |
| 游戏崩溃/强退 | WSS 断开 → 30 s presence 过期 → 页面离线 |
| 第二台设备登录 | 默认顶掉旧会话（4009）；旧端提示并停止上传；可选择"单机游玩" |
| 服务器迁移 | 登录窗可改地址；记住上次地址 |
| 页面在 https 上 | 只能走中继，不直连本机桥（决策 1） |

## 11. 落地顺序与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| L1 | 启动器登录窗（本地/线上地址、账号密码、记住我、共享开关、单机） | 断网/错密码/无服务端都有明确状态并能进单机 |
| L2 | wiki：`/auth/login`、`/auth/refresh`、`/session`（复用 better-auth） | curl 登录→刷新→会话全通；refresh 轮换/复用检测生效 |
| L3 | 启动器 WSS 上传 + 心跳 + 重连 + 4009 处理；Redis 写入 | 进世界后 2 s 内 Redis 有 pos/presence；第二台设备登录时旧端 1 s 内停止 |
| L4 | wiki 地图页 SSE + "我的位置"图层 + 在线/未共享状态 | 本人页面 200 ms 内跟随；退出 30 s 内离线；其他账号不可见 |

无 L5 社交/轨迹；后续可选：设备码登录、2FA、短轨迹（需重新评估隐私）。

## 12. 决策记录

2026-10-21 已确认：线上 https；位置共享默认开；不做轨迹；不允许多设备游戏客户端；不做社交（仅自己可见）。
仍需实现时确认的细节：单设备策略取"顶掉旧会话"还是"拒绝新会话"（§5.3，默认前者）。
