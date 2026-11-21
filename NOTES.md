## 2026-11-21 晚 进度
- CMake + git 就绪（3 次提交）；CTP .so 用 IMPORTED 目标
- iptables 免密 sudoers: /etc/sudoers.d/ctp-drill → sudo -n /usr/sbin/iptables
- tick→报单打点完成：rdtsc_now/TickMsg.tsc、tick_cb、tsc_to_ns 标定、__tick_order/lat
- 离线验证：lat n=0、__tick_order → REJECT not ready (INIT)
- 注意：CMake 产物在 build/cpptd，根目录旧手工 cpptd 已删
- 待办：换可用前置 → READY → 真实 LAT 数字 → sub 行情 → drill 演练
- 状态：待定（阻塞于前置）
## 约定（2026-11-21 起）
- 结论先写 NOTES.md → commit → 对外口径再打 tag
## CTP 错误码 / 断线码速查（2026-11-21 整理）

### 请求返回码（ReqXxx 的返回值）
| 返回 | 含义 | 处理 |
|---|---|---|
| 0 | 成功，结果看回调 | — |
| -1 | 网络连接失败 | 查前置可达性 / 是否已断线 |
| -2 | 未处理请求数超过上限 | 降低并发，等回调 |
| -3 | 每秒发送请求数超过限制 | 加重试/限速（查询尤其） |

### RspInfo 常见错误
| ErrorID | 错误串 | 常见原因 |
|---|---|---|
| 3 | CTP:不合法的登录 | BrokerID/UserID/密码错（SimNow: 9999 + 账号） |
| 5 | CTP:重复的登录 | 账号已在别处登录（快期/旧进程未退） |
| 6 | CTP:还没有登录 | 状态机没到 LOGGEDIN 就发请求 |
| 7 | CTP:还没有初始化 | Init() 未调或前置未注册 |
| 8 | CTP:前置不匹配 | 同一账号连了不同组前置，需保持一致 |
| 63 | CTP:客户端认证失败 | AppID/AuthCode 错，或未认证先登录 |
| — | 首次登录需要修改密码 | 新账号/被重置，改密后两处配置同步 |

> SimNow 特有：密码缺特殊字符或被重置是最常见的 TD 登录失败原因；重置后要同时更新
> `td.cfg.pass` 与 `md.cfg.pass`（本次 11-18 的就是这个）。

### 断线原因码（OnFrontDisconnected）
| 码 | 值 | 含义 |
|---|---|---|
| 0x1001 | 4097 | 网络读失败 |
| 0x1002 | 4098 | 网络写失败（有在途请求时约 15ms 就报） |
| 0x2001 | 8193 | 接收心跳超时（空闲约 12 秒触发） |
| 0x2002 | 8194 | 发送心跳失败 |
| 0x2003 | 8195 | 收到错误报文 |

### 明天开盘排查顺序
1. 前置可达：`timeout 5 bash -c '</dev/tcp/<ip>/<port>'`；refused=端侧没服务，timeout=被丢包/防火墙
2. `auth fail` → AppID/AuthCode；`login fail` → 密码（含 SimNow 重置）
3. `settle fail` → 是否首次登录、结算单是否已就绪
4. 查询无回调 → CTP 约 1 次/秒流控且超限静默丢弃；本项目已串行 + 1.1s 间隔 + 5s 超时跳过
5. 报单被拒先分两类：本地 `RISK REJECT`（手数/偏离/资金/频率）vs 柜台 `INSERT ERR`（看 ErrorMsg 原文）
6. 断线演练后看 `reconcile` 那行汇总：柜台有本地无 / 本地有柜台无 / 量修正 / 状态修正
7. flow 目录被 root 跑过会导致普通用户打不开 → `chown -R xjg:xjg flow_td flow_md`
o

