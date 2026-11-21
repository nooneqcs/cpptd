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

    ctp::Cfg cfg;                 // 两边共用一份配置
    td.cfg = cfg;
    md.cfg = cfg;

    // 行情驱动下单：只报第一笔，把 tick 到达的 TSC 随命令交给 TD 线程打点
    md.tick_cb = [&td](const ctp::TickMsg& t) {
        static std::once_flag once;
        if (!td.ready()) return;
        if (t.ask1 <= 0.0) return;
        std::call_once(once, [&] {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "__tick_order %s %.2f %llu",
                          t.inst.c_str(), t.ask1, t.tsc);
            td.post_cmd(buf);
            ctp::log("行情驱动：%s 卖一 %.2f，已投递报单命令", t.inst.c_str(), t.ask1);
        });
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
            if (cmd == "sub" || cmd == "unsub") md.post_cmd(line);
            else if (cmd == "mq")               md.post_cmd("q");
            else if (cmd == "ms")               md.post_cmd("s");
            else if (cmd == "mr")               md.post_cmd("r");
            else                                td.post_cmd(line);
        }
    });
    in_th.detach();

    ctp::log("=== cpptd 启动：MD 线程 + TD 主线程 ===");
    td.run();     // 阻塞
    return 0;
}

