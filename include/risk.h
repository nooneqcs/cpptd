#pragma once
#include <string>
#include <unordered_map>
#include "types.h"

namespace ctp {

struct InstInfo {
    double multiplier  = 1000.0;  // 合约乘数（au 1000 克/手，rb 10 吨/手）
    double margin_rate = 0.10;    // 保证金率
    double tick        = 0.02;    // 最小变动
    double ref         = 0.0;     // 参考价，用于偏离校验
};

// 事前风控四道：手数 / 价格偏离 / 可用资金 / 报单频率（令牌桶）
class Risk {
public:
    struct Limits {
        int    max_vol  = 10;
        double dev_pct  = 0.03;
        int    rate     = 5;      // 令牌/秒
        int    burst    = 5;
    };

    Limits cfg;
    std::unordered_map<std::string, InstInfo> inst;

    long long ok = 0, r_vol = 0, r_price = 0, r_fund = 0, r_flow = 0, r_noref = 0;
    double frozen = 0;            // 本进程内已预估占用的保证金

    void init();
    static double now_s();

    // true=放行；why 输出拒因；need 输出本次预估占用保证金
    bool check(const std::string& id, double px, int vol, char offset,
               double avail, std::string& why, double& need);

private:
    bool token();
    double tok_ = 0, last_ = 0;
};

} // namespace ctp

