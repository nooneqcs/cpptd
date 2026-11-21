# cpptd —— CTP 交易网关（Linux / C++17）

SIMNOW 仿真环境的 CTP 交易网关，含断线重连、恢复对账、事前风控、行情驱动下单与本地打点。

## 结构

include/        纯声明，不含 CThostFtdc*
  types.h       业务类型 + CTP 常量镜像
  log.h         带时间戳日志、GBK→UTF-8
  event_queue.h 模板事件队列（回调线程 push / 主线程 pop_wait）
  risk.h        事前风控四道
  td_gateway.h  交易网关（TdSpi 适配器只在 .cpp 内）
  md_feed.h     行情接入 / 断线重订阅 / 到达统计
src/            log.cpp  risk.cpp  td_gateway.cpp  md_feed.cpp  main.cpp
tests/          risk_test.cpp 四道风控断言（不依赖 CTP SDK）

## 三条边界

1. types.h 不出现任何 CThostFtdc*
2. ThostFtdcTraderApi.h 只在 td_gateway.cpp，ThostFtdcMdApi.h 只在 md_feed.cpp
3. main.cpp 无业务逻辑

常量靠 static_assert 在编译期与 CTP 头对齐，边界成立又不担心值写错。

## 构建

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j

CTP SDK 默认读 $HOME/ctp_sdk/trader、$HOME/ctp_sdk/md，可用 -DCTP_ROOT= 覆盖。
CTP 的 .so 没有 lib 前缀，因此用 IMPORTED 目标指定完整路径。

    ./build/risk_test     # 风控单测，不需要 CTP 环境
    ./build/cpptd         # 主程序，需先在当前目录建 flow_td/ flow_md/

## 命令

| 命令 | 作用 |
|---|---|
| b 合约 0买1卖 0开1平3平今4平昨 价 手 | 下单（过风控） |
| c OrderRef / ca | 撤单 / 全撤 |
| q | 本地单簿 |
| s | 状态、可用资金、余额 |
| r | 手动触发恢复查询 |
| p 合约 参考价 | 设参考价（偏离校验用） |
| k | 风控计数 |
| rtest n | 令牌桶压测 |
| cut / drill / lost / fill | 断网演练（需 iptables 免密） |
| lat | tick→报单 本地耗时统计 |
| sub / unsub / mq / ms / mr | 行情订阅与状态 |

## tick→报单打点

OnRtnDepthMarketData 里取 TSC 作为起点，投递命令后由 TD 线程在 ReqOrderInsert 返回时取终点，
用启动时以 steady_clock 标定的频率换算成纳秒。

口径：含命令行投递与事件队列穿越，不含柜台网络往返；样本是单笔，不是端到端延迟。

## 已知限制

- 演练依赖 sudo -n /usr/sbin/iptables（需 sudoers 免密）

- 截至 2026-11-21，SIMNOW 前置拒绝连接，登录/结算/对账尚未在 Linux 实测


## 运行前

    export CTP_USER=<账号>
    export CTP_PASS=<密码>

前置/经纪商可用 CTP_TD_FRONT、CTP_MD_FRONT、CTP_BROKER 覆盖，见 include/types.h 的 load_cfg()。
