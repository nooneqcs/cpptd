// lat_log.h —— 行情→报单→回报→成交 延迟打点（header-only，线程安全）
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "types.h"

namespace ctp {

class LatLog {
public:
    static constexpr double TSC_GHZ = 3.0;   // i5-7400 基频；换机器要校准

    struct Rec {
        std::string inst, ref;
        unsigned long long t_tick = 0, t_order = 0, t_rtn = 0, t_rtn2 = 0, t_trade = 0;
        double px = 0; int vol = 0; char status = 0;
    };

    void set_out(const std::string& p) { path_ = p; }
    void enable(bool on)          { on_ = on; }
    bool enabled() const          { return on_; }

    // MD 线程：记录该合约最近一次行情的到达 TSC
    void mark_tick(const TickMsg& t) {
        if (!on_) return;
        std::lock_guard<std::mutex> lk(m_);
        last_tick_[t.inst] = t.tsc;
    }

    // 主线程：报单发出
    void mark_order(const std::string& inst, double px, int vol) {
        if (!on_) return;
        std::lock_guard<std::mutex> lk(m_);
        Rec r;
        r.inst = inst; r.px = px; r.vol = vol;
        r.t_order = rdtsc_now();
        auto it = last_tick_.find(inst);
        r.t_tick = (it == last_tick_.end()) ? 0 : it->second;
        pend_[inst] = r;                  // 每合约保留一单在途，demo 够用
    }

    // 回报：首条(ack)与终态各记一次
    void mark_rtn(const OrderRtn& o) {
        if (!on_) return;
        std::lock_guard<std::mutex> lk(m_);
        auto it = pend_.find(o.instrument);
        if (it == pend_.end()) return;
        if (it->second.t_rtn == 0)  it->second.t_rtn  = rdtsc_now();  // 首条回报(ack)，后续不覆盖
        if (!is_active(o.status))   it->second.t_rtn2 = rdtsc_now();  // 终态回报(成交/撤/拒)
        it->second.status = o.status;
        it->second.ref    = o.order_ref;
    }

    void mark_trade(const TradeRtn& t) {
        if (!on_) return;
        std::lock_guard<std::mutex> lk(m_);
        auto it = pend_.find(t.instrument);
        if (it == pend_.end()) return;
        Rec r = it->second;
        r.t_trade = rdtsc_now();
        r.ref = t.order_ref;
        done_.push_back(r);
        pend_.erase(it);
        if (!path_.empty()) {
            if (FILE* f = std::fopen(path_.c_str(), "a")) { line(f, r); std::fclose(f); }
        }
    }

    void dump(FILE* out = stdout) {
        std::lock_guard<std::mutex> lk(m_);
        std::fprintf(out, "[lat] n=%zu\n", done_.size());
        std::vector<double> a, b, c;
        for (auto& r : done_) {
            if (r.t_tick && r.t_order >= r.t_tick) a.push_back(ns(r.t_order - r.t_tick));
            if (r.t_rtn && r.t_order)              b.push_back(ns(r.t_rtn - r.t_order));
            if (r.t_trade && r.t_rtn2)             c.push_back(ns(r.t_trade - r.t_rtn2));
        }
        stat(out, "tick->order", a);
        stat(out, "order->rtn", b);
        stat(out, "rtn->trade", c);
    }

private:
    static double ns(unsigned long long d) { return (double)d / TSC_GHZ; }

    static void stat(FILE* out, const char* name, std::vector<double>& v) {
        if (v.empty()) { std::fprintf(out, "  %-12s n=0\n", name); return; }
        std::sort(v.begin(), v.end());
        std::fprintf(out, "  %-12s n=%zu p50=%.0f p99=%.0f max=%.0f ns\n",
                     name, v.size(), v[(size_t)(0.50 * (v.size() - 1))],
                     v[(size_t)(0.99 * (v.size() - 1))], v.back());
    }

    static void line(FILE* f, const Rec& r) {
        std::fprintf(f, "%s,%s,%.4f,%d,%c,%.0f,%.0f,%.0f\n",
            r.inst.c_str(), r.ref.c_str(), r.px, r.vol, r.status ? r.status : '?',
            (r.t_tick && r.t_order) ? ns(r.t_order - r.t_tick) : -1.0,
            (r.t_rtn && r.t_order)  ? ns(r.t_rtn  - r.t_order) : -1.0,
            (r.t_trade && r.t_rtn2) ? ns(r.t_trade - r.t_rtn2) : -1.0);
    }

    mutable std::mutex m_;
    bool on_ = false;
    std::string path_;
    std::unordered_map<std::string, unsigned long long> last_tick_;
    std::unordered_map<std::string, Rec> pend_;
    std::vector<Rec> done_;
};

inline LatLog& lat_g() { static LatLog g; static bool i = (g.set_out("demo_roundtrip.csv"), g.enable(true), true); (void)i; return g; }

} // namespace ctp
