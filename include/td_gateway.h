
#pragma once
#include <functional>

#include <string>
#include <vector>
#include <deque>
#include <set>
#include <unordered_map>
#include <chrono>
#include "types.h"
#include "event_queue.h"
#include "risk.h"

class CThostFtdcTraderApi;     // 前置声明：本头文件不含任何 CTP 头

namespace ctp {

class TdSpi;                   // SPI 适配器，定义在 td_gateway.cpp

class TdGateway {
public:
    enum QK { Q_ACC = 0, Q_POS, Q_ORDER, Q_TRADE };

    TdGateway() = default;
    ~TdGateway();

    Cfg  cfg;
    Risk risk;

    // ---- 对外接口 ----
    void run();                               // 建 api、Init、进事件循环
    void post_cmd(const std::string& line);
    bool submit_order(const std::string& instrument, char direction, char offset,
                      double price, int volume);
    bool cancel_order(const std::string& order_ref);

    St   state() const { return st_; }
    bool ready()  const { return st_ == St::READY; }
    std::function<void(const std::string&, double, int)> order_cb;  // 报单已发出
    std::function<void(const OrderRtn&)>  rtn_cb;                   // 订单回报
    std::function<void(const TradeRtn&)>  trade_cb;                 // 成交回报

    // ---- 给 SPI 适配器用（回调线程）----
    void post_event(const Event& e);
    void set_session(int front_id, int session_id, int max_order_ref);
    int  take_order_ref() { return ++max_order_ref_; }

private:
    // ---- 事件循环 ----
    void handle(const Event& e);
    void tick();

    // ---- 回报处理 ----
    void on_order_rtn(const OrderRtn& o);
    void on_trade_rtn(const TradeRtn& t);
    void on_cmd(const std::string& line);
    void dump_book();

    // ---- 状态机 / 请求 ----
    void set(St s);
    void req_auth();
    void req_login();
    void req_settle();

    // ---- 恢复与对账 ----
    void start_recovery();
    void check();
    void mark_done(int k);
    void send_qry(int k);
    void reconcile();

    // ---- 演练（断网）----
    void run_cut();
    void run_drill(double px, int vol);
    void run_lost(double px, int vol);
    void run_fill(double px, int vol);
    void net_cycle();
    int  net_block();
    int  net_unblock();
    void post_drill(const std::string& s);

    // ---- tick→报单 本地耗时打点 ----
    long long lat_n_ = 0, lat_sum_ = 0, lat_min_ = 0, lat_max_ = 0;




    // ---- 状态 ----
    St   st_ = St::INIT;
    bool halted_ = false;
    int  front_id_ = 0, session_id_ = 0, max_order_ref_ = 0;
    bool acc_done_ = false, pos_done_ = false, ord_done_ = false, trd_done_ = false;
    double avail_ = 0, balance_ = 0;
    int  pos_cnt_ = 0, reqid_ = 0;

    std::deque<int> qry_q_;
    bool qry_pending_ = false, qry_inflight_ = false;
    int  cur_qry_ = -1;
    std::chrono::steady_clock::time_point qry_sent_at_, next_qry_at_;

    std::unordered_map<std::string, LocalOrder>  book_;
    std::unordered_map<std::string, std::string> sys2ref_;
    std::vector<OrderRtn> cab_orders_;
    std::unordered_map<std::string, int> cab_trade_vol_;

    EventQueue<Event> q_;
    CThostFtdcTraderApi* api_ = nullptr;
    TdSpi* spi_ = nullptr;
};

} // namespace ctp

