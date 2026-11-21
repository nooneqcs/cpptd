#include "risk.h"
#include <cmath>
#include <chrono>
#include <algorithm>

namespace ctp {

void Risk::init() {
    auto& a = inst["au2612"]; a.multiplier = 1000; a.margin_rate = 0.09;
    auto& r = inst["rb2601"]; r.multiplier = 10;   r.margin_rate = 0.10;
}

double Risk::now_s() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Risk::token() {
    double t = now_s();
    if (last_ == 0) { last_ = t; tok_ = cfg.burst; }
    tok_ = std::min((double)cfg.burst, tok_ + (t - last_) * cfg.rate);
    last_ = t;
    if (tok_ >= 1.0) { tok_ -= 1.0; return true; }
    return false;
}

bool Risk::check(const std::string& id, double px, int vol, char offset,
                 double avail, std::string& why, double& need) {
    if (vol <= 0 || vol > cfg.max_vol) { r_vol++; why = "手数超限"; return false; }

    auto it = inst.find(id);
    InstInfo d = (it != inst.end()) ? it->second : InstInfo();
    if (d.ref > 0) {
        double dev = std::fabs(px - d.ref) / d.ref;
        if (dev > cfg.dev_pct) {
            r_price++;
            why = "价格偏离 " + std::to_string(dev * 100).substr(0, 4) + "%";
            return false;
        }
    }
    else r_noref++;                 // 没有参考价 → 跳过偏离校验，但计数

    need = 0;
    if (offset == OF_Open) {
        need = px * d.multiplier * vol * d.margin_rate;
        if (need + frozen > avail) {
            r_fund++;
            why = "可用不足 需 " + std::to_string((long long)need);
            return false;
        }
    }
    if (!token()) { r_flow++; why = "报单频率超限"; return false; }
    frozen += need;
    ok++;
    return true;
}

} // namespace ctp

