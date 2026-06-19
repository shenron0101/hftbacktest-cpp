// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

#include "hbt/hbt.hpp"

namespace {

using Asset = hbt::LinearAsset;
using Latency = hbt::ConstantLatency;
using Queue = hbt::RiskAdverseQueueModel;
using Depth = hbt::HashMapMarketDepth;
using Fees = hbt::TradingValueFeeModel<hbt::CommonFees>;
using Local = hbt::Local<Asset, Latency, Depth, Fees>;
using Exchange = hbt::NoPartialFillExchange<Asset, Latency, Queue, Depth, Fees>;

hbt::Event depth_event(std::uint64_t kind, double price, double quantity, std::int64_t exchange_ts,
                       std::int64_t local_ts) {
    hbt::Event event{};
    event.ev = kind;
    event.exch_ts = exchange_ts;
    event.local_ts = local_ts;
    event.px = price;
    event.qty = quantity;
    return event;
}

hbt::Event trade_event(std::uint64_t kind, double price, double quantity, std::int64_t exchange_ts,
                       std::int64_t local_ts) {
    return depth_event(kind, price, quantity, exchange_ts, local_ts);
}

bool is_filled(const hbt::Backtest &backtest, hbt::OrderId order_id) {
    const auto &orders = backtest.orders(0);
    const auto order = orders.find(order_id);
    return order != orders.end() && order->second.status == hbt::Status::Filled;
}

} // namespace

int main() {
    constexpr double tick_size = 0.1;
    constexpr double lot_size = 0.001;
    constexpr std::int64_t entry_latency_ns = 100'000;
    constexpr std::int64_t response_latency_ns = 50'000;

    std::vector<hbt::Event> local_feed{
        depth_event(hbt::LOCAL_BID_DEPTH_EVENT, 99.9, 5.0, 1'000'000, 1'010'000),
        depth_event(hbt::LOCAL_ASK_DEPTH_EVENT, 100.1, 5.0, 1'000'000, 1'010'000),
        trade_event(hbt::LOCAL_SELL_TRADE_EVENT, 99.8, 5.0, 2'000'000, 2'010'000),
        trade_event(hbt::LOCAL_BUY_TRADE_EVENT, 100.2, 5.0, 4'000'000, 4'010'000),
    };
    std::vector<hbt::Event> exchange_feed{
        depth_event(hbt::EXCH_BID_DEPTH_EVENT, 99.9, 5.0, 1'000'000, 1'010'000),
        depth_event(hbt::EXCH_ASK_DEPTH_EVENT, 100.1, 5.0, 1'000'000, 1'010'000),
        trade_event(hbt::EXCH_SELL_TRADE_EVENT, 99.8, 5.0, 2'000'000, 2'010'000),
        trade_event(hbt::EXCH_BUY_TRADE_EVENT, 100.2, 5.0, 4'000'000, 4'010'000),
    };

    auto [exchange_to_local, local_to_exchange] =
        hbt::make_order_buses<Latency>(Latency{entry_latency_ns, response_latency_ns});
    auto local = std::make_unique<Local>(
        Depth{tick_size, lot_size},
        hbt::State<Asset, Fees>{Asset{1.0}, Fees{hbt::CommonFees{0.0001, 0.0007}}}, 16,
        std::move(local_to_exchange));
    auto exchange =
        std::make_unique<Exchange>(Depth{tick_size, lot_size},
                                   hbt::State<Asset, Fees, Queue::OrderState>{
                                       Asset{1.0}, Fees{hbt::CommonFees{0.0001, 0.0007}}},
                                   Queue{}, std::move(exchange_to_local));

    hbt::Backtest backtest;
    backtest.add_asset(std::move(local), std::move(exchange), {{std::move(local_feed)}},
                       {{std::move(exchange_feed)}});

    if (backtest.elapse(50'000) != hbt::ElapseResult::Ok || std::isnan(backtest.mid(0))) {
        std::cerr << "failed to initialize synthetic order book\n";
        return EXIT_FAILURE;
    }
    std::cout << std::fixed << std::setprecision(3) << "book ts_ns=" << backtest.current_timestamp()
              << " bid=" << backtest.best_bid(0) << " ask=" << backtest.best_ask(0)
              << " mid=" << backtest.mid(0) << '\n';

    constexpr hbt::OrderId bid_id = 1;
    backtest.submit_buy_order(0, bid_id, backtest.best_bid(0), 1.0, hbt::TimeInForce::GTC,
                              hbt::OrdType::Limit, false);
    backtest.wait_order_response(0, bid_id);
    std::cout << "order id=" << bid_id << " side=buy price=" << backtest.best_bid(0)
              << " ack_ts_ns=" << backtest.current_timestamp() << '\n';
    backtest.elapse(1'000'000);
    if (!is_filled(backtest, bid_id)) {
        std::cerr << "synthetic buy order did not fill\n";
        return EXIT_FAILURE;
    }
    std::cout << "fill id=" << bid_id << " side=buy price=99.900 qty=1.000 maker=true\n";

    constexpr hbt::OrderId ask_id = 2;
    backtest.submit_sell_order(0, ask_id, backtest.best_ask(0), 1.0, hbt::TimeInForce::GTC,
                               hbt::OrdType::Limit, false);
    backtest.wait_order_response(0, ask_id);
    std::cout << "order id=" << ask_id << " side=sell price=" << backtest.best_ask(0)
              << " ack_ts_ns=" << backtest.current_timestamp() << '\n';
    backtest.elapse(2'000'000);
    if (!is_filled(backtest, ask_id)) {
        std::cerr << "synthetic sell order did not fill\n";
        return EXIT_FAILURE;
    }
    std::cout << "fill id=" << ask_id << " side=sell price=100.100 qty=1.000 maker=true\n";

    const auto &state = backtest.state_values(0);
    std::cout << "summary fills=" << state.num_trades << " position=" << state.position
              << " balance=" << state.balance << " fee=" << state.fee
              << " volume=" << state.trading_volume
              << " final_ts_ns=" << backtest.current_timestamp() << '\n';
    std::cout << "latency entry_ns=" << entry_latency_ns << " response_ns=" << response_latency_ns
              << '\n';
    return state.num_trades == 2 && std::abs(state.position) < 1e-12 ? EXIT_SUCCESS : EXIT_FAILURE;
}
