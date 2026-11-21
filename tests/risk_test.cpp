// risk_test.cpp —— 四道风控的单元断言，不依赖 CTP SDK
#include <cstdio>
#include <cmath>
#include <string>
#include "risk.h"

using namespace ctp;

static int g_fail = 0;

static void expect(bool cond, const char* name, const std::string& detail = "") {
    printf("[%s] %s%s%s\n", cond ? "PASS" : "FAIL", name,
           detail.empty() ? "" : " -> ", detail.c_str());
    if (!cond) ++g_fail;
}

int main() {
    // 1) 手数上限 10
    {
        Risk r; r.init();
        std::string why; double need = 0;
        bool ok = r.check("au2612", 700.0, 20, OF_Open, 1e12, why, need);
        expect(!ok && r.r_vol == 1, "手数上限：vol=20 应拒", why);
    }
    // 2) 价格偏离 ±3%
    {
        Risk r; r.init();
        r.inst["au2612"].ref = 700.0;
        std::string w1, w2; double n1 = 0, n2 = 0;
        bool in  = r.check("au2612", 700.0, 1, OF_Open, 1e12, w1, n1);
        bool out = r.check("au2612", 800.0, 1, OF_Open, 1e12, w2, n2);
        expect(in && !out && r.r_price == 1, "价格偏离 ±3%：700 放行 / 800 拒", w2);
    }
    // 3) 令牌桶 rate=5 burst=5，10 连发
    {
        Risk r; r.init();
        r.inst["au2612"].ref = 700.0;
        int pass = 0, rej = 0;
        for (int i = 0; i < 10; ++i) {
            std::string why; double need = 0;
            if (r.check("au2612", 700.0, 1, OF_Open, 1e12, why, need)) ++pass;
            else ++rej;
        }
        expect(pass == 5 && rej == 5, "令牌桶 5/s burst5：10 连发 pass=5 rej=5",
               "pass=" + std::to_string(pass) + " rej=" + std::to_string(rej));
    }
    // 4) 可用资金：margin_rate 临时改 100 → 需 700*1000*1*100 = 7e7
    {
        Risk r; r.init();
        r.inst["au2612"].ref = 700.0;
        r.inst["au2612"].margin_rate = 100.0;
        std::string why; double need = 0;
        bool ok = r.check("au2612", 700.0, 1, OF_Open, 1e6, why, need);
        expect(!ok && r.r_fund == 1 && std::fabs(need - 7e7) < 1.0,
               "可用不足：需 7e7 / 可用 1e6 应拒", why);
    }

    if (g_fail) printf("\n%d 项失败\n", g_fail);
    else        printf("\n全部通过\n");
    return g_fail ? 1 : 0;
}

