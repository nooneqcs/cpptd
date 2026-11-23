#include "td_gateway.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#include <chrono>

#include "ThostFtdcTraderApi.h"     // 唯一允许 include CTP 头的文件之一
#include "log.h"

namespace ctp {

// ===== 常量镜像校验：types.h 的字面量必须与 CTP 定义一致 =====
static_assert(DIR_Buy  == THOST_FTDC_D_Buy,  "DIR_Buy 与 CTP 不一致");
static_assert(DIR_Sell == THOST_FTDC_D_Sell, "DIR_Sell 与 CTP 不一致");
static_assert(OF_Open           == THOST_FTDC_OF_Open,           "OF_Open 不一致");
static_assert(OF_Close          == THOST_FTDC_OF_Close,          "OF_Close 不一致");
static_assert(OF_CloseToday     == THOST_FTDC_OF_CloseToday,     "OF_CloseToday 不一致");
static_assert(OF_CloseYesterday == THOST_FTDC_OF_CloseYesterday, "OF_CloseYesterday 不一致");
static_assert(OST_AllTraded             == THOST_FTDC_OST_AllTraded,             "OST_AllTraded 不一致");
static_assert(OST_PartTradedQueueing    == THOST_FTDC_OST_PartTradedQueueing,    "OST_PartTradedQueueing 不一致");
static_assert(OST_PartTradedNotQueueing == THOST_FTDC_OST_PartTradedNotQueueing, "OST_PartTradedNotQueueing 不一致");
static_assert(OST_NoTradeQueueing       == THOST_FTDC_OST_NoTradeQueueing,       "OST_NoTradeQueueing 不一致");
static_assert(OST_NoTradeNotQueueing    == THOST_FTDC_OST_NoTradeNotQueueing,    "OST_NoTradeNotQueueing 不一致");
static_assert(OST_Canceled              == THOST_FTDC_OST_Canceled,              "OST_Canceled 不一致");
static_assert(OST_Unknown               == THOST_FTDC_OST_Unknown,               "OST_Unknown 不一致");
// TSC → 纳秒：启动时用 steady_clock 标定一次（不依赖主频标称值）
static double tsc_to_ns(unsigned long long d) {
    static const double hz = []() {
        auto t0 = std::chrono::steady_clock::now();
        unsigned long long c0 = rdtsc_now();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto t1 = std::chrono::steady_clock::now();
        unsigned long long c1 = rdtsc_now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        return s > 0 ? (double)(c1 - c0) / s : 1.0;
    }();
    return (double)d * 1e9 / hz;
}


// ===== CTP 结构 → 业务结构 =====
static OrderRtn from_order(const CThostFtdcOrderField* o) {
    OrderRtn r;
    if (!o) return r;
    r.instrument  = o->InstrumentID;
    r.order_ref   = o->OrderRef;
    r.sys_id      = o->OrderSysID;
    r.exchange_id = o->ExchangeID;
    r.direction   = o->Direction;
    r.offset      = o->CombOffsetFlag[0];
    r.status      = o->OrderStatus;
    r.price       = o->LimitPrice;
    r.vol_total   = o->VolumeTotalOriginal;
    r.vol_traded  = o->VolumeTraded;
    r.front_id    = o->FrontID;
    r.session_id  = o->SessionID;
    return r;
}

static TradeRtn from_trade(const CThostFtdcTradeField* t) {
    TradeRtn r;
    if (!t) return r;
    r.instrument = t->InstrumentID;
    r.order_ref  = t->OrderRef;
    r.sys_id     = t->OrderSysID;
    r.trade_id   = t->TradeID;
    r.price      = t->Price;
    r.volume     = t->Volume;
    return r;
}

static void fill_err(Event& e, const CThostFtdcRspInfoField* info) {
    if (info && info->ErrorID != 0) {
        e.ok = false;
        e.err = info->ErrorID;
        e.msg = gbk2utf8(info->ErrorMsg);   // Linux 下走 iconv
    }
    else e.ok = true;
}

// ===== SPI 适配器：只用回调入队 =====
class TdSpi : public CThostFtdcTraderSpi {
public:
    explicit TdSpi(TdGateway* gw) : gw_(gw) {}

    void OnFrontConnected() override {
        Event e; e.ev = Ev::FRONT_CONN; gw_->post_event(e);
    }
    void OnFrontDisconnected(int r) override {
        Event e; e.ev = Ev::FRONT_DISC; e.reqid = r; gw_->post_event(e);
    }
    void OnHeartBeatWarning(int n) override {
        Event e; e.ev = Ev::HEARTBEAT_WARN; e.reqid = n; gw_->post_event(e);
    }
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField*,
                           CThostFtdcRspInfoField* info, int reqid, bool) override {
        Event e; e.ev = Ev::AUTH_RSP; e.reqid = reqid; fill_err(e, info); gw_->post_event(e);
    }
    void OnRspUserLogin(CThostFtdcRspUserLoginField* login,
                        CThostFtdcRspInfoField* info, int reqid, bool) override {
        Event e; e.ev = Ev::LOGIN_RSP; e.reqid = reqid; fill_err(e, info);
        if (login) {
            gw_->set_session(login->FrontID, login->SessionID, atoi(login->MaxOrderRef));
            e.tag = std::string("front=") + std::to_string(login->FrontID) +
                    " session=" + std::to_string(login->SessionID) +
                    " maxref=" + login->MaxOrderRef;
        }
        gw_->post_event(e);
    }
    void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*,
                                    CThostFtdcRspInfoField* info, int reqid, bool) override {
        Event e; e.ev = Ev::SETTLE_RSP; e.reqid = reqid; fill_err(e, info); gw_->post_event(e);
    }
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* a,
                                CThostFtdcRspInfoField* info, int reqid, bool last) override {
        Event e; e.ev = Ev::QRY_ACC; e.reqid = reqid; e.last = last; fill_err(e, info);
        if (a) e.tag = std::to_string(a->Available) + " " + std::to_string(a->Balance);
        gw_->post_event(e);
    }
    void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* p,
                                  CThostFtdcRspInfoField* info, int reqid, bool last) override {
        Event e; e.ev = Ev::QRY_POS; e.reqid = reqid; e.last = last; fill_err(e, info);
        if (p && p->Position) e.tag = "row";
        gw_->post_event(e);
    }
    void OnRspQryOrder(CThostFtdcOrderField* o, CThostFtdcRspInfoField* info,
                       int reqid, bool last) override {
        Event e; e.ev = Ev::QRY_ORDER; e.reqid = reqid; e.last = last; fill_err(e, info);
        e.ord = from_order(o);
        gw_->post_event(e);
    }
    void OnRspQryTrade(CThostFtdcTradeField* t, CThostFtdcRspInfoField* info,
                       int reqid, bool last) override {
        Event e; e.ev = Ev::QRY_TRADE; e.reqid = reqid; e.last = last; fill_err(e, info);
        e.trd = from_trade(t);
        gw_->post_event(e);
    }
    void OnRtnOrder(CThostFtdcOrderField* o) override {
        Event e; e.ev = Ev::ORDER_RTN; e.ord = from_order(o); gw_->post_event(e);
    }
    void OnRtnTrade(CThostFtdcTradeField* t) override {
        Event e; e.ev = Ev::TRADE_RTN; e.trd = from_trade(t); gw_->post_event(e);
    }
    void OnRspOrderInsert(CThostFtdcInputOrderField* in, CThostFtdcRspInfoField* info,
                          int reqid, bool) override {
        Event e; e.ev = Ev::INS_RSP; e.reqid = reqid; fill_err(e, info);
        if (in) e.tag = std::string(in->InstrumentID) + " ref=" + in->OrderRef;
        gw_->post_event(e);
    }
    void OnRspOrderAction(CThostFtdcInputOrderActionField* in, CThostFtdcRspInfoField* info,
                          int reqid, bool) override {
        Event e; e.ev = Ev::ACT_RSP; e.reqid = reqid; fill_err(e, info);
        if (in) e.tag = std::string(in->InstrumentID) + " sys=" + in->OrderSysID;
        gw_->post_event(e);
    }
    void OnErrRtnOrderInsert(CThostFtdcInputOrderField* in, CThostFtdcRspInfoField* info) override {
        Event e; e.ev = Ev::INS_ERR; fill_err(e, info);
        if (in) e.tag = std::string(in->InstrumentID) + " ref=" + in->OrderRef;
        gw_->post_event(e);
    }
    void OnErrRtnOrderAction(CThostFtdcOrderActionField* in, CThostFtdcRspInfoField* info) override {
        Event e; e.ev = Ev::ACT_ERR; fill_err(e, info);
        if (in) e.tag = std::string(in->InstrumentID) + " sys=" + in->OrderSysID;
        gw_->post_event(e);
    }

private:
    TdGateway* gw_;
};

// ================= TdGateway =================

TdGateway::~TdGateway() {
    if (api_) api_->Release();   // spi_ 由进程生命周期持有，不在此释放
}

void TdGateway::post_event(const Event& e) { q_.push(e); }

void TdGateway::post_cmd(const std::string& line) {
    Event e; e.ev = Ev::CMD; e.tag = line; q_.push(e);
}

void TdGateway::set_session(int front_id, int session_id, int max_order_ref) {
    front_id_ = front_id;
    session_id_ = session_id;
    if (max_order_ref > max_order_ref_) max_order_ref_ = max_order_ref;
}

void TdGateway::set(St s) {
    if (s != st_) { log("STATE %s -> %s", st_name(st_), st_name(s)); st_ = s; }
}

// ---------- 主循环 ----------
void TdGateway::run() {
    api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(cfg.flow_td.c_str());
    spi_ = new TdSpi(this);
    api_->RegisterSpi(spi_);
    api_->SubscribePrivateTopic(THOST_TERT_QUICK);
    api_->SubscribePublicTopic(THOST_TERT_QUICK);
    api_->RegisterFront(const_cast<char*>(cfg.front.c_str()));
    api_->Init();
    risk.init();

    log("api init front=%s flow=%s", cfg.front.c_str(), cfg.flow_td.c_str());
    log("命令: b 下单 / c 撤单 / ca 全撤 / q 查单 / s 状态 / r 刷新");
    log("      p 参考价 / k 风控 / rtest <n> 令牌桶压测");
    log("      cut / drill <价> <手> / lost <价> <手> / fill <价> <手>");

    for (;;) {
        Event e;
        if (q_.pop_wait(e, 200)) handle(e);
        tick();
    }
}

// ---------- 发单（风控在这里拦） ----------
bool TdGateway::submit_order(const std::string& instrument, char direction, char offset,
                             double price, int volume) {
    if (halted_) { log("REJECT 已停止运行（登录失败）"); return false; }
    if (st_ != St::READY) { log("REJECT not ready (%s)", st_name(st_)); return false; }

    std::string why; double need = 0;
    if (!risk.check(instrument, price, volume, offset, avail_, why, need)) {
        log("RISK REJECT %s px=%.2f vol=%d -> %s",
            instrument.c_str(), price, volume, why.c_str());
        return false;
    }

    CThostFtdcInputOrderField f{};
    std::snprintf(f.BrokerID, sizeof(f.BrokerID), "%s", cfg.broker.c_str());
    std::snprintf(f.InvestorID, sizeof(f.InvestorID), "%s", cfg.user.c_str());
    std::snprintf(f.UserID, sizeof(f.UserID), "%s", cfg.user.c_str());
    std::snprintf(f.InstrumentID, sizeof(f.InstrumentID), "%s", instrument.c_str());
    std::snprintf(f.OrderRef, sizeof(f.OrderRef), "%d", ++max_order_ref_);
    std::snprintf(f.CombOffsetFlag, sizeof(f.CombOffsetFlag), "%c", offset);
    std::snprintf(f.CombHedgeFlag, sizeof(f.CombHedgeFlag), "%c", THOST_FTDC_HF_Speculation);
    f.Direction = direction;
    f.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    f.LimitPrice = price;
    f.VolumeTotalOriginal = volume;
    f.TimeCondition = THOST_FTDC_TC_GFD;
    f.VolumeCondition = THOST_FTDC_VC_AV;
    f.MinVolume = 1;
    f.ContingentCondition = THOST_FTDC_CC_Immediately;
    f.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    f.IsAutoSuspend = 0;
    f.UserForceClose = 0;

    LocalOrder lo;
    lo.order_ref  = f.OrderRef;
    lo.instrument = instrument;
    lo.direction  = direction;
    lo.offset     = offset;
    lo.price      = price;
    lo.vol_total  = volume;
    lo.front_id   = front_id_;
    lo.session_id = session_id_;
    lo.status     = OST_Unknown;
    book_[lo.order_ref] = lo;

    int r = api_->ReqOrderInsert(&f, ++reqid_);
    log("INSERT %s ref=%s %c/%c px=%.2f vol=%d -> ret=%d",
        instrument.c_str(), f.OrderRef, direction, offset, price, volume, r);
    if (r != 0) { book_[lo.order_ref].status = OST_Canceled; return false; }
    if (order_cb) order_cb(instrument, price, volume);
    return true;
}

bool TdGateway::cancel_order(const std::string& order_ref) {
    auto it = book_.find(order_ref);
    if (it == book_.end()) { log("CANCEL unknown ref=%s", order_ref.c_str()); return false; }
    if (!is_active(it->second.status)) { log("CANCEL ref=%s 已终态", order_ref.c_str()); return false; }

    CThostFtdcInputOrderActionField f{};
    std::snprintf(f.BrokerID, sizeof(f.BrokerID), "%s", cfg.broker.c_str());
    std::snprintf(f.InvestorID, sizeof(f.InvestorID), "%s", cfg.user.c_str());
    std::snprintf(f.UserID, sizeof(f.UserID), "%s", cfg.user.c_str());
    std::snprintf(f.InstrumentID, sizeof(f.InstrumentID), "%s", it->second.instrument.c_str());
    std::snprintf(f.OrderRef, sizeof(f.OrderRef), "%s", it->second.order_ref.c_str());
    f.FrontID = it->second.front_id;
    f.SessionID = it->second.session_id;
    f.ActionFlag = THOST_FTDC_AF_Delete;
    if (!it->second.sys_id.empty()) {
        std::snprintf(f.OrderSysID, sizeof(f.OrderSysID), "%s", it->second.sys_id.c_str());
        std::snprintf(f.ExchangeID, sizeof(f.ExchangeID), "%s", it->second.exchange_id.c_str());
    }
    int r = api_->ReqOrderAction(&f, ++reqid_);
    log("CANCEL ref=%s sys=%s -> ret=%d", order_ref.c_str(), it->second.sys_id.c_str(), r);
    return r == 0;
}

// ---------- 事件分发 ----------
void TdGateway::handle(const Event& e) {
    switch (e.ev) {
    case Ev::FRONT_CONN:
        if (halted_) { log("登录已停止：账号可能被锁，解锁后重启程序"); break; }
        set(St::CONNECTED); req_auth();
        break;
    case Ev::FRONT_DISC:
        log("FRONT DISCONNECTED reason=%d", e.reqid);
        set(St::DISCONNECTED);
        acc_done_ = pos_done_ = ord_done_ = trd_done_ = false;
        qry_pending_ = qry_inflight_ = false;
        qry_q_.clear();
        break;
    case Ev::HEARTBEAT_WARN:
        log("WARN heartbeat lapse=%ds", e.reqid);
        break;
    case Ev::AUTH_RSP:
        if (!e.ok) { halted_ = true; log("auth fail %d %s -> 停止重试", e.err, e.msg.c_str()); break; }
        set(St::AUTHED); req_login();
        break;
    case Ev::LOGIN_RSP:
        if (!e.ok) { halted_ = true; log("login fail %d %s -> 停止重试", e.err, e.msg.c_str()); break; }
        log("login ok %s", e.tag.c_str());
        set(St::LOGGEDIN); req_settle();
        break;
    case Ev::SETTLE_RSP:
        if (!e.ok) { halted_ = true; log("settle fail %d %s -> 停止重试", e.err, e.msg.c_str()); break; }
        set(St::SETTLED); start_recovery();
        break;
    case Ev::QRY_ACC:
        if (!e.ok) log("query account err %d %s", e.err, e.msg.c_str());
        if (e.last) {
            acc_done_ = true; qry_inflight_ = false;
            avail_ = std::stod(e.tag); log("account avail=%.2f", avail_);
            risk.frozen = 0;          // 用柜台真实可用重建，清掉本地预扣
        }
        break;

    case Ev::QRY_POS:
        if (!e.ok) log("query pos err %d %s", e.err, e.msg.c_str());
        if (e.last) { pos_done_ = true; qry_inflight_ = false; log("positions rows=%d", pos_cnt_); }
        break;
    case Ev::QRY_ORDER:
        if (!e.ok) log("query order err %d %s", e.err, e.msg.c_str());
        if (!e.ord.order_ref.empty()) cab_orders_.push_back(e.ord);
        if (e.last) { ord_done_ = true; qry_inflight_ = false; log("query orders rows=%zu", cab_orders_.size()); }
        break;
    case Ev::QRY_TRADE:
        if (!e.ok) log("query trade err %d %s", e.err, e.msg.c_str());
        if (!e.trd.order_ref.empty()) cab_trade_vol_[e.trd.order_ref] += e.trd.volume;
        if (e.last) { trd_done_ = true; qry_inflight_ = false; log("query trades refs=%zu", cab_trade_vol_.size()); }
        break;
    case Ev::ORDER_RTN: on_order_rtn(e.ord); break;
    case Ev::TRADE_RTN: on_trade_rtn(e.trd); break;
    case Ev::INS_RSP:
        log("INSERT RSP %s %s err=%d %s", e.tag.c_str(), e.ok ? "ok" : "rej", e.err, e.msg.c_str());
        break;
    case Ev::ACT_RSP:
        log("CANCEL RSP %s %s err=%d %s", e.tag.c_str(), e.ok ? "ok" : "rej", e.err, e.msg.c_str());
        break;
    case Ev::INS_ERR:
        log("INSERT ERR %s err=%d %s", e.tag.c_str(), e.err, e.msg.c_str());
        break;
    case Ev::ACT_ERR:
        log("CANCEL ERR %s err=%d %s", e.tag.c_str(), e.err, e.msg.c_str());
        break;
    case Ev::DRILL_LOG:
        log("DRILL %s", e.tag.c_str());
        break;
    case Ev::CMD: on_cmd(e.tag); break;
    }
}

void TdGateway::on_order_rtn(const OrderRtn& o) {
    if (rtn_cb) rtn_cb(o);
    auto& lo = book_[o.order_ref];
    lo.order_ref   = o.order_ref;
    lo.instrument  = o.instrument;
    lo.exchange_id = o.exchange_id;
    lo.direction   = o.direction;
    lo.offset      = o.offset;
    lo.status      = o.status;
    lo.price       = o.price;
    lo.vol_total   = o.vol_total;
    lo.vol_traded  = o.vol_traded;
    lo.front_id    = o.front_id;
    lo.session_id  = o.session_id;
    lo.acked       = true;
    if (!o.sys_id.empty()) { lo.sys_id = o.sys_id; sys2ref_[o.sys_id] = o.order_ref; }
    log("RTN %s ref=%s sys=%s st=%c vol=%d/%d%s", o.instrument.c_str(),
        o.order_ref.c_str(), o.sys_id.c_str(), o.status, o.vol_traded, o.vol_total,
        st_ == St::READY ? "" : "  [断线窗口内]");
}

void TdGateway::on_trade_rtn(const TradeRtn& t) {
    if (trade_cb) trade_cb(t);
    auto& lo = book_[t.order_ref];
    lo.order_ref  = t.order_ref;
    lo.instrument = t.instrument;
    if (!t.sys_id.empty()) { lo.sys_id = t.sys_id; sys2ref_[t.sys_id] = t.order_ref; }
    int& s = cab_trade_vol_[t.order_ref];
    s += t.volume;
    if (s > lo.vol_traded) lo.vol_traded = s;
    log("TRADE %s ref=%s tradeid=%s px=%.2f vol=%d%s", t.instrument.c_str(),
        t.order_ref.c_str(), t.trade_id.c_str(), t.price, t.volume,
        st_ == St::READY ? "" : "  [断线窗口内]");
}
// ---------- 查询流控：CTP 约 1 次/秒，串行 + 超时跳过 ----------
void TdGateway::tick() {
    if (!qry_pending_) return;
    auto now = std::chrono::steady_clock::now();
    if (qry_inflight_) {
        if (now - qry_sent_at_ < std::chrono::seconds(5)) return;
        log("WARN 查询 %d 超时，跳过", cur_qry_);
        mark_done(cur_qry_);
        qry_inflight_ = false;
    }
    if (now < next_qry_at_) return;
    if (qry_q_.empty()) { qry_pending_ = false; check(); return; }
    cur_qry_ = qry_q_.front();
    qry_q_.pop_front();
    send_qry(cur_qry_);
    qry_sent_at_  = now;
    qry_inflight_ = true;
    next_qry_at_  = now + std::chrono::milliseconds(1100);
}

void TdGateway::mark_done(int k) {
    if      (k == Q_ACC)   acc_done_ = true;
    else if (k == Q_POS)   pos_done_ = true;
    else if (k == Q_ORDER) ord_done_ = true;
    else if (k == Q_TRADE) trd_done_ = true;
}

void TdGateway::send_qry(int k) {
    switch (k) {
    case Q_ACC: {
        CThostFtdcQryTradingAccountField a{};
        std::snprintf(a.BrokerID,   sizeof(a.BrokerID),   "%s", cfg.broker.c_str());
        std::snprintf(a.InvestorID, sizeof(a.InvestorID), "%s", cfg.user.c_str());
        api_->ReqQryTradingAccount(&a, ++reqid_);
        break;
    }
    case Q_POS: {
        CThostFtdcQryInvestorPositionField p{};
        std::snprintf(p.BrokerID,   sizeof(p.BrokerID),   "%s", cfg.broker.c_str());
        std::snprintf(p.InvestorID, sizeof(p.InvestorID), "%s", cfg.user.c_str());
        api_->ReqQryInvestorPosition(&p, ++reqid_);
        break;
    }
    case Q_ORDER: {
        CThostFtdcQryOrderField o{};
        std::snprintf(o.BrokerID,   sizeof(o.BrokerID),   "%s", cfg.broker.c_str());
        std::snprintf(o.InvestorID, sizeof(o.InvestorID), "%s", cfg.user.c_str());
        api_->ReqQryOrder(&o, ++reqid_);
        break;
    }
    case Q_TRADE: {
        CThostFtdcQryTradeField t{};
        std::snprintf(t.BrokerID,   sizeof(t.BrokerID),   "%s", cfg.broker.c_str());
        std::snprintf(t.InvestorID, sizeof(t.InvestorID), "%s", cfg.user.c_str());
        api_->ReqQryTrade(&t, ++reqid_);
        break;
    }
    }
    log("QUERY %d 发出", k);
}
// ---------- 命令 ----------
void TdGateway::on_cmd(const std::string& line) {
    std::istringstream is(line);
    std::string cmd;
    if (!(is >> cmd)) return;

    if (cmd == "b") {
        std::string inst, dirs;
        int off = -1, vol = 0;
        double px = 0;
        if (is >> inst >> dirs >> off >> px >> vol) {
            int dir = atoi(dirs.c_str());
            char d = (dir == 0) ? DIR_Buy : DIR_Sell;
            char o = OF_Open;
            if      (off == 1) o = OF_Close;
            else if (off == 3) o = OF_CloseToday;
            else if (off == 4) o = OF_CloseYesterday;
            submit_order(inst, d, o, px, vol);
        }
        else log("用法: b <合约> <0买|1卖> <0开|1平|3平今|4平昨> <价格> <手数>");
    }
    else if (cmd == "c") {
        std::string ref;
        if (is >> ref) cancel_order(ref);
        else log("用法: c <OrderRef>");
    }
    else if (cmd == "ca") {
        std::vector<std::string> refs;
        for (auto& kv : book_)
            if (is_active(kv.second.status)) refs.push_back(kv.first);
        log("CANCEL ALL 共 %zu 张", refs.size());
        for (auto& r : refs) cancel_order(r);
    }
    else if (cmd == "q") dump_book();
    else if (cmd == "s")
        log("STATE=%s 本地单=%zu 可用=%.2f 余额=%.2f halted=%d",
            st_name(st_), book_.size(), avail_, balance_, (int)halted_);
    else if (cmd == "r") { log("手动刷新查询"); start_recovery(); }
    else if (cmd == "p") {
        std::string id; double px = 0;
        if (is >> id >> px) { risk.inst[id].ref = px; log("REF %s = %.2f", id.c_str(), px); }
        else log("用法: p <合约> <参考价>");
    }
    else if (cmd == "k")
        log("RISK ok=%lld 手数=%lld 偏离=%lld 资金=%lld 频率=%lld 无参考价=%lld 冻结=%.0f",
            risk.ok, risk.r_vol, risk.r_price, risk.r_fund, risk.r_flow, risk.r_noref, risk.frozen);
    else if (cmd == "rtest") {
        int n = 10; is >> n;
        int pass = 0, rej = 0;
        std::string why; double need = 0;
        for (int i = 0; i < n; ++i) {
            if (risk.check(cfg.drill_inst, 700.0, 1, OF_Open, 1e12, why, need)) ++pass;
            else ++rej;
        }
        risk.frozen = 0;
        log("RTEST n=%d pass=%d rej=%d（令牌桶瞬时压测）", n, pass, rej);
    }
    else if (cmd == "lost") {
        double px = 0; int vol = 1;
        if (is >> px >> vol) run_lost(px, vol);
        else log("用法: lost <价> <手>  —— 先阻断，1s 后下单");
    }
    else if (cmd == "fill") {
        double px = 0; int vol = 1;
        if (is >> px >> vol) run_fill(px, vol);
        else log("用法: fill <价> <手>  —— 挂排队单后断网");
    }
    else if (cmd == "__lost_order") {
        double px = 0; int vol = 1;
        if (is >> px >> vol)
            submit_order(cfg.drill_inst, DIR_Buy, OF_Open, px, vol);
    }
    else if (cmd == "cut") { log("DRILL 仅断网 20s，不加报单"); run_cut(); }
    else if (cmd == "drill") {
        double px = 0; int vol = 1;
        if (is >> px >> vol) run_drill(px, vol);
        else log("用法: drill <价格> <手数>  —— 报单后 300ms 断网 20s");
    }
    else if (cmd == "__tick_order") {
        std::string inst; double px = 0; unsigned long long t0 = 0;
        if (is >> inst >> px >> t0) {
            bool ok = submit_order(inst, DIR_Buy, OF_Open, px, 1);
            unsigned long long t1 = rdtsc_now();
            if (ok) {
                long long ns = (long long)tsc_to_ns(t1 - t0);
                lat_n_++; lat_sum_ += ns;
                if (lat_n_ == 1 || ns < lat_min_) lat_min_ = ns;
                if (ns > lat_max_) lat_max_ = ns;
                log("LAT tick->order %lld ns inst=%s px=%.2f (n=%lld)",
                    ns, inst.c_str(), px, lat_n_);
            }
        }
        else log("用法: __tick_order <合约> <价> <tsc>");
    }
    else if (cmd == "lat") {
        log("LAT n=%lld avg=%.0f min=%lld max=%lld ns", lat_n_,
            lat_n_ ? (double)lat_sum_ / lat_n_ : 0.0, lat_min_, lat_max_);
    }

    else {
        log("命令: b / c / ca / q / s / r / p / k / rtest");
        log("      cut / drill <价> <手> / lost <价> <手> / fill <价> <手>");
    }
}

void TdGateway::dump_book() {
    log("---- 本地单 %zu ----", book_.size());
    for (auto& kv : book_) {
        auto& o = kv.second;
        log("ref=%-4s sys=%-12s %-8s %c/%c st=%c %d/%d @%.2f %s",
            o.order_ref.c_str(), o.sys_id.c_str(), o.instrument.c_str(),
            o.direction, o.offset, o.status, o.vol_traded, o.vol_total,
            o.price, is_active(o.status) ? "在途" : "终态");
    }
}
// ---------- 状态机请求 ----------
void TdGateway::req_auth() {
    CThostFtdcReqAuthenticateField f{};
    std::snprintf(f.BrokerID,  sizeof(f.BrokerID),  "%s", cfg.broker.c_str());
    std::snprintf(f.UserID,    sizeof(f.UserID),    "%s", cfg.user.c_str());
    std::snprintf(f.AppID,     sizeof(f.AppID),     "%s", cfg.appid.c_str());
    std::snprintf(f.AuthCode,  sizeof(f.AuthCode),  "%s", cfg.authcode.c_str());
    if (api_->ReqAuthenticate(&f, ++reqid_) != 0) log("ReqAuthenticate send fail");
}

void TdGateway::req_login() {
    CThostFtdcReqUserLoginField f{};
    std::snprintf(f.BrokerID, sizeof(f.BrokerID), "%s", cfg.broker.c_str());
    std::snprintf(f.UserID,   sizeof(f.UserID),   "%s", cfg.user.c_str());
    std::snprintf(f.Password, sizeof(f.Password), "%s", cfg.pass.c_str());
    if (api_->ReqUserLogin(&f, ++reqid_) != 0) log("ReqUserLogin send fail");
}

void TdGateway::req_settle() {
    CThostFtdcSettlementInfoConfirmField f{};
    std::snprintf(f.BrokerID,   sizeof(f.BrokerID),   "%s", cfg.broker.c_str());
    std::snprintf(f.InvestorID, sizeof(f.InvestorID), "%s", cfg.user.c_str());
    if (api_->ReqSettlementInfoConfirm(&f, ++reqid_) != 0) log("ReqSettlementConfirm send fail");
}
// ---------- 恢复与对账 ----------
void TdGateway::start_recovery() {
    if (!(st_ == St::READY || st_ == St::SETTLED)) {
        log("跳过刷新：当前状态 %s", st_name(st_));
        return;
    }
    set(St::RECOVERING);
    cab_orders_.clear();
    cab_trade_vol_.clear();
    pos_cnt_ = 0;
    acc_done_ = pos_done_ = ord_done_ = trd_done_ = false;
    qry_q_ = { Q_ACC, Q_POS, Q_ORDER, Q_TRADE };
    qry_pending_ = true;
    qry_inflight_ = false;
    cur_qry_ = -1;
    next_qry_at_ = std::chrono::steady_clock::now();
}

void TdGateway::check() {
    if (!(acc_done_ && pos_done_ && ord_done_ && trd_done_)) {
        log("恢复未完成 acc=%d pos=%d ord=%d trd=%d",
            acc_done_, pos_done_, ord_done_, trd_done_);
        return;
    }
    reconcile();
    set(St::READY);
    log("恢复完成，可发单");
}
void TdGateway::reconcile() {
    std::set<std::string> seen;
    int cab_only = 0, local_only = 0, vol_fix = 0, st_fix = 0;

    for (auto& c : cab_orders_) {
        std::string ref = c.order_ref;
        if (ref.empty() && !c.sys_id.empty()) {
            auto it = sys2ref_.find(c.sys_id);
            if (it != sys2ref_.end()) ref = it->second;
        }
        if (ref.empty()) continue;

        auto it = book_.find(ref);
        if (it == book_.end()) {          // 柜台有本地无 → 补建
            LocalOrder lo;
            lo.order_ref   = ref;
            lo.instrument  = c.instrument;
            lo.sys_id      = c.sys_id;
            lo.exchange_id = c.exchange_id;
            lo.direction   = c.direction;
            lo.offset      = c.offset;
            lo.status      = c.status;
            lo.price       = c.price;
            lo.vol_total   = c.vol_total;
            lo.vol_traded  = c.vol_traded;
            lo.front_id    = c.front_id;
            lo.session_id  = c.session_id;
            lo.acked       = true;
            book_[ref] = lo;
            if (!c.sys_id.empty()) sys2ref_[c.sys_id] = ref;
            seen.insert(ref);
            cab_only++;
            log("  [对账] 柜台有本地无 %s ref=%s sys=%s st=%c",
                c.instrument.c_str(), ref.c_str(), c.sys_id.c_str(), c.status);
            continue;
        }

        LocalOrder& lo = it->second;
        seen.insert(ref);
        int tv = c.vol_traded;
        auto t = cab_trade_vol_.find(ref);
        if (t != cab_trade_vol_.end() && t->second > tv) tv = t->second;
        if (lo.vol_traded != tv) { vol_fix++; lo.vol_traded = tv; }
        if (lo.status != c.status) { st_fix++; lo.status = c.status; }
        if (!c.sys_id.empty() && lo.sys_id != c.sys_id) {
            lo.sys_id = c.sys_id;
            sys2ref_[c.sys_id] = ref;
        }
        lo.acked = true;
    }
    for (auto& kv : book_) {              // 本地有柜台无 → 标终态
        LocalOrder& lo = kv.second;
        if (seen.count(lo.order_ref)) continue;
        if (!is_active(lo.status)) continue;
        lo.status = OST_Canceled;
        lo.acked = true;
        local_only++;
        log("  [对账] 本地有柜台无 %s ref=%s -> 标终态",
            lo.instrument.c_str(), lo.order_ref.c_str());
    }

    // 本地最大 OrderRef 顶上去，避免重启后新单与历史单撞号
    for (auto& kv : book_) {
        int n = atoi(kv.first.c_str());
        if (n > max_order_ref_) max_order_ref_ = n;
    }

    log("对账完成 cab=%zu local=%zu 柜台有本地无=%d 本地有柜台无=%d 量修正=%d 状态修正=%d",
        cab_orders_.size(), book_.size(), cab_only, local_only, vol_fix, st_fix);
}
// ---------- 演练：断网 ----------
void TdGateway::run_cut() { std::thread([this] { net_cycle(); }).detach(); }

void TdGateway::run_drill(double px, int vol) {
    if (!submit_order(cfg.drill_inst, DIR_Buy, OF_Open, px, vol)) return;
    log("DRILL 已报单 %s px=%.2f vol=%d，300ms 后阻断", cfg.drill_inst.c_str(), px, vol);
    std::thread([this] { net_cycle(); }).detach();
}

void TdGateway::run_lost(double px, int vol) {
    std::thread([this, px, vol] {
        int r1 = net_block();
        post_drill("LOST 阻断 rc=" + std::to_string(r1) + "，1s 后下单（仍 READY）");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        std::ostringstream os;
        os << "__lost_order " << px << " " << vol;
        post_cmd(os.str());
        std::this_thread::sleep_for(std::chrono::seconds(20));
        int r2 = net_unblock();
        post_drill("LOST 解除 rc=" + std::to_string(r2) + "，等恢复对账");
        }).detach();
}

void TdGateway::run_fill(double px, int vol) {
    if (!submit_order(cfg.drill_inst, DIR_Buy, OF_Open, px, vol)) return;
    log("FILL 已挂单 %.2f，500ms 后阻断，赌 25s 窗口内成交", px);
    std::thread([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        int r1 = net_block();
        post_drill("FILL 阻断 rc=" + std::to_string(r1));
        std::this_thread::sleep_for(std::chrono::seconds(25));
        int r2 = net_unblock();
        post_drill("FILL 解除 rc=" + std::to_string(r2));
        }).detach();
}
void TdGateway::net_cycle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int r1 = net_block();
    post_drill(std::string("阻断已下发 rc=") + std::to_string(r1));
    std::this_thread::sleep_for(std::chrono::seconds(20));
    int r2 = net_unblock();
    post_drill(std::string("阻断已解除 rc=") + std::to_string(r2) + "，等待自动恢复");
    if (r1 != 0 || r2 != 0)
        post_drill("提示：netsh/iptables 非 0 —— Linux 下需 sudo，或改用 tc/nft");
}

int TdGateway::net_block() {
#ifdef _WIN32
    std::string c = "netsh advfirewall firewall add rule name=\"blkCTPd\" dir=out action=block protocol=TCP remoteip="
        + cfg.drill_ip + " remoteport=" + cfg.drill_port + " >nul 2>&1";
#else
    std::string c = "sudo -n /usr/sbin/iptables -A OUTPUT -d " + cfg.drill_ip
        + " -p tcp --dport " + cfg.drill_port + " -j DROP";
#endif
    return system(c.c_str());
}

int TdGateway::net_unblock() {
#ifdef _WIN32
    std::string c = "netsh advfirewall firewall delete rule name=\"blkCTPd\" >nul 2>&1";
#else
    std::string c = "sudo -n /usr/sbin/iptables -D OUTPUT -d " + cfg.drill_ip
        + " -p tcp --dport " + cfg.drill_port + " -j DROP";
#endif
    return system(c.c_str());
}

void TdGateway::post_drill(const std::string& s) {
    Event e; e.ev = Ev::DRILL_LOG; e.tag = s; q_.push(e);
}

} // namespace ctp

