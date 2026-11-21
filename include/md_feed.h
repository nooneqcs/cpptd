#pragma once
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <mutex>
#include "types.h"
#include "event_queue.h"

class CThostFtdcMdApi;        // 前置声明

namespace ctp {

class MdSpi;

class MdFeed {
public:
    struct SubItem {
        std::string inst;
        long long sub_at_ms = 0, last_tick_ms = 0, prev_tick_ms = 0;
        int resub = 0;
        bool stale = false;
        long long ticks = 0, holes = 0, max_gap_ms = 0;
        double last_px = 0, bid1 = 0, ask1 = 0;
        int volume = 0, oi = 0;
        char update_time[16] = { 0 };
        int update_ms = 0;
    };

    MdFeed() = default;
    ~MdFeed();

    Cfg cfg;                                 // 复用 broker/user/pass/md_front/flow_md
    std::vector<std::string> subs = { "au2612" };
    int stale_ms = 10000;   // 超过这么久没行情 → 标 stale 并重订阅（最多 2 次）
    int hole_ms  = 1500;    // 相邻行情间隔超过它计一次空洞（正常快照 500ms）

    void run();
    void post_cmd(const std::string& line);
    void post_event(const MdEvent& e);

    // 行情快照（跨线程安全），供下单侧取对手价
    bool quote(const std::string& inst, double& bid1, double& ask1);
    int  ready_count(const std::vector<std::string>& insts);

private:
    void handle(const MdEvent& e);
    void tick();
    void on_tick(const TickMsg& t);
    void on_cmd(const std::string& line);
    void do_sub(const std::vector<std::string>& list);
    void do_unsub(const std::string& inst);
    void req_login();
    void dump();
    void set(St s);

    St   st_ = St::INIT;
    int  reqid_ = 0;
    long long last_check_ms_ = 0;

    mutable std::mutex subs_mtx_;                        // quote() 跨线程读
    std::unordered_map<std::string, SubItem> subs_;

    EventQueue<MdEvent> q_;
    CThostFtdcMdApi* md_ = nullptr;
    MdSpi* spi_ = nullptr;
};

} // namespace ctp

