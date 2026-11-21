#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <sstream>
#include <mutex>

#include "td_gateway.h"
#include "md_feed.h"
#include "log.h"

int main() {
    ctp::TdGateway td;
    ctp::MdFeed    md;

    ctp::Cfg cfg = ctp::load_cfg();   // 凭据从环境变量读
    if (cfg.user.empty() || cfg.pass.empty())
        ctp::log("提示：未设置 CTP_USER / CTP_PASS，登录会失败");

                 // 两边共用一份配置
    td.cfg = cfg;
    md.cfg = cfg;

    // 行情驱动下单：第一笔行情到达时投递报单命令，把 tick 的 TSC 带给 TD 打点
    md.tick_cb = [&td](const ctp::TickMsg& t) {
        if (t.ask1 <= 0.0) return;
        static bool fired = false;
        ctp::log("TICK-CB %s bid=%.2f ask=%.2f ready=%d",
                 t.inst.c_str(), t.bid1, t.ask1, (int)td.ready());
        if (fired) return;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "__tick_order %s %.2f %llu",
                      t.inst.c_str(), t.ask1, t.tsc);
        td.post_cmd(buf);
        if (td.ready()) fired = true;    // 只有 READY 下的那一笔才算“已报”，避免开局空转
    };

    std::thread md_th([&md] { md.run(); });
    md_th.detach();

    std::thread in_th([&td, &md] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            std::istringstream is(line);
            std::string cmd;
            is >> cmd;
            if (cmd == "sub" || cmd == "unsub" || cmd == "fake") md.post_cmd(line);
            else if (cmd == "mq")                               md.post_cmd("q");
            else if (cmd == "ms")                               md.post_cmd("s");
            else if (cmd == "mr")                               md.post_cmd("r");
            else                                                td.post_cmd(line);
        }
    });
    in_th.detach();

    ctp::log("=== cpptd 启动：MD 线程 + TD 主线程 ===");
    td.run();     // 阻塞
    return 0;
}

