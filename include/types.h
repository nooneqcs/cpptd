// types.h —— 纯业务类型，不含任何 CThostFtdc*
#pragma once
#include <string>

namespace ctp {

// ---- CTP 常量镜像（值由 td_gateway.cpp / md_feed.cpp 里的 static_assert 校验）----
constexpr char DIR_Buy  = '0';
constexpr char DIR_Sell = '1';

constexpr char OF_Open           = '0';
constexpr char OF_Close          = '1';
constexpr char OF_CloseToday     = '3';
constexpr char OF_CloseYesterday = '4';

constexpr char OST_AllTraded             = '0';  // 全部成交
constexpr char OST_PartTradedQueueing    = '1';  // 部分成交还在队列
constexpr char OST_PartTradedNotQueueing = '2';  // 部分成交不在队列
constexpr char OST_NoTradeQueueing       = '3';  // 未成交还在队列
constexpr char OST_NoTradeNotQueueing    = '4';  // 未成交不在队列
constexpr char OST_Canceled              = '5';  // 已撤单
constexpr char OST_Unknown               = 'a';

enum class St { INIT, DISCONNECTED, CONNECTED, AUTHED, LOGGEDIN, SETTLED, RECOVERING, READY };

inline const char* st_name(St s) {
    switch (s) {
    case St::INIT:         return "INIT";
    case St::DISCONNECTED: return "DISCONNECTED";
    case St::CONNECTED:    return "CONNECTED";
    case St::AUTHED:       return "AUTHED";
    case St::LOGGEDIN:     return "LOGGEDIN";
    case St::SETTLED:      return "SETTLED";
    case St::RECOVERING:   return "RECOVERING";
    case St::READY:        return "READY";
    }
    return "?";
}

// 是否"在途"（非终态）
inline bool is_active(char st) {
    return !(st == OST_AllTraded || st == OST_PartTradedNotQueueing ||
             st == OST_NoTradeNotQueueing || st == OST_Canceled);
}

// ---- 配置 ----
struct Cfg {
    std::string front      = "tcp://182.254.243.31:30002";
    std::string md_front   = "tcp://182.254.243.31:30012";
    std::string broker     = "9999";
    std::string user       = "[REDACTED]";
    std::string pass       = "[REDACTED]";
    std::string appid      = "simnow_client_test";
    std::string authcode   = "0000000000000000";
    std::string flow_td    = "./flow_td/";   // 原 D:\Code\... 改成相对路径
    std::string flow_md    = "./flow_md/";
    std::string drill_ip   = "182.254.243.31";
    std::string drill_port = "30002";
    std::string drill_inst = "au2612";
};

// ---- 本地单 / 回报 ----
struct LocalOrder {
    std::string order_ref, sys_id, exchange_id, instrument;
    char direction = 0, offset = 0, status = 0;
    double price = 0;
    int vol_total = 0, vol_traded = 0;
    int front_id = 0, session_id = 0;
    bool acked = false;
};

struct OrderRtn {
    std::string instrument, order_ref, sys_id, exchange_id;
    char direction = 0, offset = 0, status = 0;
    double price = 0;
    int vol_total = 0, vol_traded = 0, front_id = 0, session_id = 0;
};

struct TradeRtn {
    std::string instrument, order_ref, sys_id, trade_id;
    double price = 0;
    int volume = 0;
};

// ---- TD 事件 ----
enum class Ev {
    FRONT_CONN, FRONT_DISC, HEARTBEAT_WARN,
    AUTH_RSP, LOGIN_RSP, SETTLE_RSP,
    QRY_ACC, QRY_POS, QRY_ORDER, QRY_TRADE,
    ORDER_RTN, TRADE_RTN, INS_RSP, ACT_RSP, INS_ERR, ACT_ERR, CMD, DRILL_LOG
};

struct Event {
    Ev ev = Ev::FRONT_CONN;
    int reqid = 0, err = 0;
    bool ok = false, last = false;
    std::string tag, msg;
    OrderRtn ord;
    TradeRtn trd;
};

// ---- MD 侧类型与事件 ----

// 读时间戳计数器（同一台机器跨线程可比，TSC 不随核变化）
inline unsigned long long rdtsc_now() {
#ifdef _MSC_VER
    return __rdtsc();
#else
    return __builtin_ia32_rdtsc();
#endif
}

struct TickMsg {
    std::string inst;
    long long arrival_ms = 0;
    unsigned long long tsc = 0;   // 到达时刻的 TSC，用于 tick→报单打点
    double last_px = 0, bid1 = 0, ask1 = 0;
    int volume = 0, oi = 0;
    char update_time[16] = { 0 };
    int update_ms = 0;
};

enum class MdEv { FRONT_CONN, FRONT_DISC, HB_WARN, LOGIN_RSP, SUB_RSP, UNSUB_RSP, TICK, CMD };

struct MdEvent {
    MdEv ev = MdEv::FRONT_CONN;
    int reqid = 0, err = 0;
    bool ok = false, last = false;
    std::string tag, msg;
    TickMsg tick;
};

} // namespace ctp

