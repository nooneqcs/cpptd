#include "md_feed.h"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <chrono>

#include "ThostFtdcMdApi.h"     // 唯一允许 include 行情头的地方
#include "log.h"

namespace ctp {

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void fill_err(MdEvent& e, const CThostFtdcRspInfoField* info) {
    if (info && info->ErrorID != 0) {
        e.ok  = false;
        e.err = info->ErrorID;
        e.msg = gbk2utf8(info->ErrorMsg);
    }
    else e.ok = true;
}

// ===== SPI 适配器：只入队 =====
class MdSpi : public CThostFtdcMdSpi {
public:
    explicit MdSpi(MdFeed* f) : f_(f) {}

    void OnFrontConnected() override {
        MdEvent e; e.ev = MdEv::FRONT_CONN; f_->post_event(e);
    }
    void OnFrontDisconnected(int r) override {
        MdEvent e; e.ev = MdEv::FRONT_DISC; e.reqid = r; f_->post_event(e);
    }
    void OnHeartBeatWarning(int n) override {
        MdEvent e; e.ev = MdEv::HB_WARN; e.reqid = n; f_->post_event(e);
    }
    void OnRspUserLogin(CThostFtdcRspUserLoginField*,
                        CThostFtdcRspInfoField* info, int reqid, bool) override {
        MdEvent e; e.ev = MdEv::LOGIN_RSP; e.reqid = reqid; fill_err(e, info);
        f_->post_event(e);
    }
    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* p,
                            CThostFtdcRspInfoField* info, int reqid, bool last) override {
        MdEvent e; e.ev = MdEv::SUB_RSP; e.reqid = reqid; e.last = last; fill_err(e, info);
        if (p) e.tag = p->InstrumentID;
        f_->post_event(e);
    }
    void OnRspUnSubMarketData(CThostFtdcSpecificInstrumentField* p,
                              CThostFtdcRspInfoField* info, int reqid, bool last) override {
        MdEvent e; e.ev = MdEv::UNSUB_RSP; e.reqid = reqid; e.last = last; fill_err(e, info);
        if (p) e.tag = p->InstrumentID;
        f_->post_event(e);
    }
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* d) override {
        if (!d) return;
        MdEvent e; e.ev = MdEv::TICK;
        TickMsg& t = e.tick;
        t.inst       = d->InstrumentID;
        t.arrival_ms = now_ms();
        t.tsc        = rdtsc_now();          // 到达时刻，打点起点
        t.last_px    = d->LastPrice > 1e10 ? 0 : d->LastPrice;
        t.bid1       = d->BidPrice1  > 1e10 ? 0 : d->BidPrice1;
        t.ask1       = d->AskPrice1  > 1e10 ? 0 : d->AskPrice1;
        t.volume     = d->Volume;
        t.oi         = (int)d->OpenInterest;
        std::snprintf(t.update_time, sizeof(t.update_time), "%s", d->UpdateTime);
        t.update_ms  = d->UpdateMillisec;
        f_->post_event(e);
    }

private:
    MdFeed* f_;
};

// ================= MdFeed =================

MdFeed::~MdFeed() {
    if (md_) md_->Release();
}

void MdFeed::post_event(const MdEvent& e) { q_.push(e); }

void MdFeed::post_cmd(const std::string& line) {
    MdEvent e; e.ev = MdEv::CMD; e.tag = line; q_.push(e);
}

void MdFeed::set(St s) {
    if (s != st_) { log("STATE %s -> %s", st_name(st_), st_name(s)); st_ = s; }
}

// 供下单侧取对手价（跨线程安全）
bool MdFeed::quote(const std::string& inst, double& bid1, double& ask1) {
    std::lock_guard<std::mutex> lk(subs_mtx_);
    auto it = subs_.find(inst);
    if (it == subs_.end()) return false;
    bid1 = it->second.bid1;
    ask1 = it->second.ask1;
    return bid1 > 0.0 && ask1 > 0.0;
}

int MdFeed::ready_count(const std::vector<std::string>& insts) {
    std::lock_guard<std::mutex> lk(subs_mtx_);
    int n = 0;
    for (auto& s : insts) {
        auto it = subs_.find(s);
        if (it != subs_.end() && it->second.bid1 > 0 && it->second.ask1 > 0) ++n;
    }
    return n;
}

void MdFeed::run() {
    md_ = CThostFtdcMdApi::CreateFtdcMdApi(cfg.flow_md.c_str());
    spi_ = new MdSpi(this);
    md_->RegisterSpi(spi_);
    md_->RegisterFront(const_cast<char*>(cfg.md_front.c_str()));
    md_->Init();
    log("md init front=%s flow=%s", cfg.md_front.c_str(), cfg.flow_md.c_str());
    log("命令: sub <合约...> / unsub <合约> / q 列表 / s 状态 / r 重订阅");
    for (;;) {
        MdEvent e;
        if (q_.pop_wait(e, 200)) handle(e);
        tick();
    }
}

void MdFeed::handle(const MdEvent& e) {
    switch (e.ev) {
    case MdEv::FRONT_CONN:
        set(St::CONNECTED);
        req_login();
        break;
    case MdEv::FRONT_DISC:
        log("FRONT DISCONNECTED reason=%d", e.reqid);
        set(St::INIT);
        {
            std::lock_guard<std::mutex> lk(subs_mtx_);
            for (auto& kv : subs_) { kv.second.stale = true; kv.second.prev_tick_ms = 0; }
        }
        break;
    case MdEv::HB_WARN:
        log("WARN heartbeat lapse=%ds", e.reqid);
        break;
    case MdEv::LOGIN_RSP:
        if (!e.ok) { log("login fail %d %s", e.err, e.msg.c_str()); break; }
        set(St::LOGGEDIN);
        log("登录成功，重订阅 %zu 个", subs.size());
        set(St::READY);
        do_sub(subs);              // 断线后同样走这里，自动重订阅
        break;
    case MdEv::SUB_RSP:
        if (!e.ok) log("SUB RSP %s rej %d %s", e.tag.c_str(), e.err, e.msg.c_str());
        break;
    case MdEv::UNSUB_RSP:
        if (!e.ok) log("UNSUB RSP %s rej %d %s", e.tag.c_str(), e.err, e.msg.c_str());
        break;
    case MdEv::TICK:
        on_tick(e.tick);
        break;
    case MdEv::CMD:
        on_cmd(e.tag);
        break;
    }
}

// 定期检查：长时间无行情 → 标 stale，最多重订阅 2 次
void MdFeed::tick() {
    long long now = now_ms();
    if (now - last_check_ms_ < 2000) return;
    last_check_ms_ = now;
    if (st_ != St::READY) return;

    std::vector<std::string> resub;
    {
        std::lock_guard<std::mutex> lk(subs_mtx_);
        for (auto& kv : subs_) {
            SubItem& s = kv.second;
            long long ref = s.last_tick_ms ? s.last_tick_ms : s.sub_at_ms;
            if (ref == 0) continue;
            if (now - ref < stale_ms) continue;
            if (!s.stale) {
                s.stale = true;
                log("STALE %s 已 %lldms 无行情", s.inst.c_str(), now - ref);
            }
            if (s.resub < 2) {
                s.resub++;
                log("RESUB %s 第 %d 次", s.inst.c_str(), s.resub);
                resub.push_back(s.inst);
                s.last_tick_ms = now;    // 重新计时，避免每 2 秒刷屏
            }
        }
    }
    for (auto& x : resub) do_sub({ x });
}

void MdFeed::on_tick(const TickMsg& t) {
    std::lock_guard<std::mutex> lk(subs_mtx_);
    auto it = subs_.find(t.inst);
    if (it == subs_.end()) return;
    SubItem& s = it->second;

    if (s.prev_tick_ms > 0) {
        long long gap = t.arrival_ms - s.prev_tick_ms;
        if (gap > s.max_gap_ms) s.max_gap_ms = gap;
        if (gap > hole_ms) {
            s.holes++;
            log("HOLE %s 间隔 %lldms（>%dms）", t.inst.c_str(), gap, hole_ms);
        }
    }
    s.prev_tick_ms = t.arrival_ms;
    s.last_tick_ms = t.arrival_ms;
    s.stale   = false;
    s.ticks++;
    s.last_px = t.last_px;
    s.bid1    = t.bid1;
    s.ask1    = t.ask1;
    s.volume  = t.volume;
    s.oi      = t.oi;
    std::snprintf(s.update_time, sizeof(s.update_time), "%s", t.update_time);
    s.update_ms = t.update_ms;

    // 注意：此处持有 subs_mtx_，回调内禁止再调 MdFeed 的加锁接口
    if (tick_cb) tick_cb(t);
}

// ---------- 命令 ----------
void MdFeed::on_cmd(const std::string& line) {
    std::istringstream is(line);
    std::string cmd;
    if (!(is >> cmd)) return;

    if (cmd == "sub") {
        std::vector<std::string> list;
        std::string s;
        while (is >> s) list.push_back(s);
        if (list.empty()) { log("用法: sub <合约...>"); return; }
        for (auto& x : list) {
            if (std::find(subs.begin(), subs.end(), x) == subs.end())
                subs.push_back(x);
        }
        do_sub(list);
    }
    else if (cmd == "unsub") {
        std::string s;
        if (is >> s) {
            do_unsub(s);
            for (auto it = subs.begin(); it != subs.end(); ++it)
                if (*it == s) { subs.erase(it); break; }
        }
        else log("用法: unsub <合约>");
    }
    else if (cmd == "q") dump();
    else if (cmd == "s") {
        std::lock_guard<std::mutex> lk(subs_mtx_);
        log("STATE=%s 订阅=%zu", st_name(st_), subs_.size());
    }
    else if (cmd == "r") { log("重订阅全部"); do_sub(subs); }
    else log("命令: sub / unsub / q / s / r");
}

// ---------- 订阅管理（只在主线程调） ----------
void MdFeed::do_sub(const std::vector<std::string>& list) {
    if (st_ != St::READY) { log("订阅被忽略：状态 %s", st_name(st_)); return; }
    if (list.empty()) return;

    std::vector<std::string> all = list;     // 保证 c_str 指针稳定
    std::vector<char*> p;
    for (auto& s : all) p.push_back(const_cast<char*>(s.c_str()));
    int r = md_->SubscribeMarketData(p.data(), (int)p.size());

    long long t = now_ms();
    {
        std::lock_guard<std::mutex> lk(subs_mtx_);
        for (auto& s : list) {
            SubItem& it = subs_[s];
            it.inst      = s;
            it.sub_at_ms = t;
            it.stale     = false;
        }
    }
    log("SUB %zu 个 -> ret=%d", p.size(), r);
}

void MdFeed::do_unsub(const std::string& inst) {
    std::string one = inst;
    char* p = const_cast<char*>(one.c_str());
    int r = md_->UnSubscribeMarketData(&p, 1);
    {
        std::lock_guard<std::mutex> lk(subs_mtx_);
        subs_.erase(inst);
    }
    log("UNSUB %s -> ret=%d", inst.c_str(), r);
}

void MdFeed::req_login() {
    CThostFtdcReqUserLoginField f{};
    std::snprintf(f.BrokerID, sizeof(f.BrokerID), "%s", cfg.broker.c_str());
    std::snprintf(f.UserID,   sizeof(f.UserID),   "%s", cfg.user.c_str());
    std::snprintf(f.Password, sizeof(f.Password), "%s", cfg.pass.c_str());
    if (md_->ReqUserLogin(&f, ++reqid_) != 0) log("ReqUserLogin send fail");
}

void MdFeed::dump() {
    std::lock_guard<std::mutex> lk(subs_mtx_);
    log("---- 订阅 %zu ----", subs_.size());
    for (auto& kv : subs_) {
        auto& s = kv.second;
        log("%-8s px=%.2f bid=%.2f ask=%.2f vol=%d oi=%d %s.%03d | ticks=%lld holes=%lld maxgap=%lldms resub=%d%s",
            s.inst.c_str(), s.last_px, s.bid1, s.ask1, s.volume, s.oi,
            s.update_time, s.update_ms, s.ticks, s.holes, s.max_gap_ms, s.resub,
            s.stale ? " STALE" : "");
    }
}

} // namespace ctp

