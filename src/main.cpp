#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <sstream>

#include "td_gateway.h"
#include "md_feed.h"
#include "log.h"

int main() {
    ctp::TdGateway td;
    ctp::MdFeed    md;

    ctp::Cfg cfg;                 // 两边共用一份配置
    td.cfg = cfg;
    md.cfg = cfg;

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
            else if (cmd == "mq")             md.post_cmd("q");
            else if (cmd == "ms")             md.post_cmd("s");
            else if (cmd == "mr")             md.post_cmd("r");
            else                              td.post_cmd(line);
        }
    });
    in_th.detach();

    ctp::log("=== cpptd 启动：MD 线程 + TD 主线程 ===");
    td.run();     // 阻塞，主循环在 TdGateway::run
    return 0;
}

