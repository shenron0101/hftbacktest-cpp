// SPDX-License-Identifier: MIT
// Port of HftBacktest: https://github.com/nkaz001/hftbacktest
// Copyright (c) 2022 nkaz001@protonmail.com
// Copyright (c) 2026 shenron0101

#pragma once
// hbt/types.hpp — Core types mirroring hftbacktest/src/types.rs
//
// All integer values and field layouts must remain binary-compatible with the
// numpy event_dtype / order_dtype defined in
//   https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/py-hftbacktest/hftbacktest/types.py

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>

namespace hbt {

// ── Event flag constants ──────────────────────────────────────────────────────
// Low 8 bits encode the event kind; upper bits encode directional/routing flags.

inline constexpr uint64_t BUY_EVENT = 1ULL << 29;
inline constexpr uint64_t SELL_EVENT = 1ULL << 28;
inline constexpr uint64_t LOCAL_EVENT = 1ULL << 30;
inline constexpr uint64_t EXCH_EVENT = 1ULL << 31;

inline constexpr uint64_t DEPTH_EVENT = 1;
inline constexpr uint64_t TRADE_EVENT = 2;
inline constexpr uint64_t DEPTH_CLEAR_EVENT = 3;
inline constexpr uint64_t DEPTH_SNAPSHOT_EVENT = 4;
inline constexpr uint64_t DEPTH_BBO_EVENT = 5;
inline constexpr uint64_t ADD_ORDER_EVENT = 10;
inline constexpr uint64_t CANCEL_ORDER_EVENT = 11;
inline constexpr uint64_t MODIFY_ORDER_EVENT = 12;
inline constexpr uint64_t FILL_EVENT = 13;

// ── Composed local feed events ────────────────────────────────────────────────
inline constexpr uint64_t LOCAL_DEPTH_CLEAR_EVENT = DEPTH_CLEAR_EVENT | LOCAL_EVENT;
inline constexpr uint64_t LOCAL_BID_DEPTH_EVENT = DEPTH_EVENT | BUY_EVENT | LOCAL_EVENT;
inline constexpr uint64_t LOCAL_ASK_DEPTH_EVENT = DEPTH_EVENT | SELL_EVENT | LOCAL_EVENT;
inline constexpr uint64_t LOCAL_BID_DEPTH_CLEAR_EVENT = DEPTH_CLEAR_EVENT | BUY_EVENT | LOCAL_EVENT;
inline constexpr uint64_t LOCAL_ASK_DEPTH_CLEAR_EVENT =
    DEPTH_CLEAR_EVENT | SELL_EVENT | LOCAL_EVENT;
inline constexpr uint64_t LOCAL_BID_DEPTH_SNAPSHOT_EVENT =
    DEPTH_SNAPSHOT_EVENT | BUY_EVENT | LOCAL_EVENT;
inline constexpr uint64_t LOCAL_ASK_DEPTH_SNAPSHOT_EVENT =
    DEPTH_SNAPSHOT_EVENT | SELL_EVENT | LOCAL_EVENT;
inline constexpr uint64_t LOCAL_BID_DEPTH_BBO_EVENT = DEPTH_BBO_EVENT | BUY_EVENT | LOCAL_EVENT;
inline constexpr uint64_t LOCAL_ASK_DEPTH_BBO_EVENT = DEPTH_BBO_EVENT | SELL_EVENT | LOCAL_EVENT;
inline constexpr uint64_t LOCAL_TRADE_EVENT = TRADE_EVENT | LOCAL_EVENT;
inline constexpr uint64_t LOCAL_BUY_TRADE_EVENT = LOCAL_TRADE_EVENT | BUY_EVENT;
inline constexpr uint64_t LOCAL_SELL_TRADE_EVENT = LOCAL_TRADE_EVENT | SELL_EVENT;

// ── Composed exchange feed events ─────────────────────────────────────────────
inline constexpr uint64_t EXCH_DEPTH_CLEAR_EVENT = DEPTH_CLEAR_EVENT | EXCH_EVENT;
inline constexpr uint64_t EXCH_BID_DEPTH_EVENT = DEPTH_EVENT | BUY_EVENT | EXCH_EVENT;
inline constexpr uint64_t EXCH_ASK_DEPTH_EVENT = DEPTH_EVENT | SELL_EVENT | EXCH_EVENT;
inline constexpr uint64_t EXCH_BID_DEPTH_CLEAR_EVENT = DEPTH_CLEAR_EVENT | BUY_EVENT | EXCH_EVENT;
inline constexpr uint64_t EXCH_ASK_DEPTH_CLEAR_EVENT = DEPTH_CLEAR_EVENT | SELL_EVENT | EXCH_EVENT;
inline constexpr uint64_t EXCH_BID_DEPTH_SNAPSHOT_EVENT =
    DEPTH_SNAPSHOT_EVENT | BUY_EVENT | EXCH_EVENT;
inline constexpr uint64_t EXCH_ASK_DEPTH_SNAPSHOT_EVENT =
    DEPTH_SNAPSHOT_EVENT | SELL_EVENT | EXCH_EVENT;
inline constexpr uint64_t EXCH_BID_DEPTH_BBO_EVENT = DEPTH_BBO_EVENT | BUY_EVENT | EXCH_EVENT;
inline constexpr uint64_t EXCH_ASK_DEPTH_BBO_EVENT = DEPTH_BBO_EVENT | SELL_EVENT | EXCH_EVENT;
inline constexpr uint64_t EXCH_TRADE_EVENT = TRADE_EVENT | EXCH_EVENT;
inline constexpr uint64_t EXCH_BUY_TRADE_EVENT = EXCH_TRADE_EVENT | BUY_EVENT;
inline constexpr uint64_t EXCH_SELL_TRADE_EVENT = EXCH_TRADE_EVENT | SELL_EVENT;

// ── L3 order events ───────────────────────────────────────────────────────────
inline constexpr uint64_t LOCAL_ADD_ORDER_EVENT = LOCAL_EVENT | ADD_ORDER_EVENT;
inline constexpr uint64_t LOCAL_BID_ADD_ORDER_EVENT = BUY_EVENT | LOCAL_ADD_ORDER_EVENT;
inline constexpr uint64_t LOCAL_ASK_ADD_ORDER_EVENT = SELL_EVENT | LOCAL_ADD_ORDER_EVENT;
inline constexpr uint64_t LOCAL_CANCEL_ORDER_EVENT = LOCAL_EVENT | CANCEL_ORDER_EVENT;
inline constexpr uint64_t LOCAL_MODIFY_ORDER_EVENT = LOCAL_EVENT | MODIFY_ORDER_EVENT;
inline constexpr uint64_t LOCAL_FILL_EVENT = LOCAL_EVENT | FILL_EVENT;
inline constexpr uint64_t EXCH_ADD_ORDER_EVENT = EXCH_EVENT | ADD_ORDER_EVENT;
inline constexpr uint64_t EXCH_BID_ADD_ORDER_EVENT = BUY_EVENT | EXCH_ADD_ORDER_EVENT;
inline constexpr uint64_t EXCH_ASK_ADD_ORDER_EVENT = SELL_EVENT | EXCH_ADD_ORDER_EVENT;
inline constexpr uint64_t EXCH_CANCEL_ORDER_EVENT = EXCH_EVENT | CANCEL_ORDER_EVENT;
inline constexpr uint64_t EXCH_MODIFY_ORDER_EVENT = EXCH_EVENT | MODIFY_ORDER_EVENT;
inline constexpr uint64_t EXCH_FILL_EVENT = EXCH_EVENT | FILL_EVENT;

// ── Sentinel values ───────────────────────────────────────────────────────────
inline constexpr int64_t UNTIL_END_OF_DATA = std::numeric_limits<int64_t>::max();

// ── Event struct ──────────────────────────────────────────────────────────────
// Binary-identical to numpy event_dtype (8 × 8-byte fields, 64 bytes total).
// Reference:
// https://github.com/nkaz001/hftbacktest/blob/5f3ec40b2afb764e0fea112f941ed85523ef4e88/hftbacktest/src/types.rs
// Event  (repr(C, align(64)))
struct alignas(64) Event {
    uint64_t ev;       ///< event flag bitmask
    int64_t exch_ts;   ///< exchange timestamp (ns)
    int64_t local_ts;  ///< local receipt timestamp (ns)
    double px;         ///< price
    double qty;        ///< quantity
    uint64_t order_id; ///< L3 only — order ID
    int64_t ival;      ///< reserved integer
    double fval;       ///< reserved float
};
static_assert(sizeof(Event) == 64, "Event must be 64 bytes");
static_assert(alignof(Event) == 64, "Event must be 64-byte aligned");
static_assert(std::is_trivially_copyable_v<Event>);

/// Port of Event::is() from types.rs.
/// Checks: (a) all upper flag bits in `event` are set in ev, AND
///          (b) if event has a non-zero low-8-bit kind, ev's low 8 bits match exactly.
[[nodiscard]] inline bool event_is(const Event &e, uint64_t event) noexcept {
    if ((e.ev & event) != event)
        return false;
    const uint64_t kind = event & 0xffULL;
    if (kind == 0)
        return true;
    return (e.ev & 0xffULL) == kind;
}

// ── Side ──────────────────────────────────────────────────────────────────────
enum class Side : int8_t {
    Buy = 1,
    Sell = -1,
    None = 0,
    Unsupported = 127,
};

[[nodiscard]] inline double side_sign(Side s) noexcept {
    return static_cast<double>(static_cast<int8_t>(s));
}

// ── Status ────────────────────────────────────────────────────────────────────
enum class Status : uint8_t {
    None = 0,
    New = 1,
    Expired = 2,
    Filled = 3,
    Canceled = 4,
    PartiallyFilled = 5,
    Rejected = 6,
    Replaced = 7,
    Unsupported = 255,
};

// ── TimeInForce ───────────────────────────────────────────────────────────────
enum class TimeInForce : uint8_t {
    GTC = 0,
    GTX = 1, ///< Post-only
    FOK = 2,
    IOC = 3,
    Unsupported = 255,
};

// ── OrdType ───────────────────────────────────────────────────────────────────
enum class OrdType : uint8_t {
    Limit = 0,
    Market = 1,
    Unsupported = 255,
};

// ── OrderId ───────────────────────────────────────────────────────────────────
using OrderId = uint64_t;

// ── WaitOrderResponse ─────────────────────────────────────────────────────────
struct WaitOrderResponse {
    enum class Kind : int { None, Any, Specified } kind;
    std::size_t asset_no;
    OrderId order_id;

    static WaitOrderResponse none() { return {Kind::None, 0, 0}; }
    static WaitOrderResponse any() { return {Kind::Any, 0, 0}; }
    static WaitOrderResponse specified(std::size_t an, OrderId oid) {
        return {Kind::Specified, an, oid};
    }
};

// ── Order<Q> ─────────────────────────────────────────────────────────────────
// Q = queue-model scratch data.  Default = no-op monostate (zero size).
// The exchange processor uses Order<QM::OrderState>; the Bot API exposes Order<>.
//
// Field layout deliberately matches order_dtype from types.py (for binary I/O).
template <typename Q = std::monostate> struct Order {
    double qty = 0.0;
    double leaves_qty = 0.0;
    double exec_qty = 0.0;
    int64_t exec_price_tick = 0;
    int64_t price_tick = 0;
    double tick_size = 0.0;
    int64_t exch_timestamp = 0;
    int64_t local_timestamp = 0;
    OrderId order_id = 0;
    Q q{}; ///< queue-model scratch (zero-size when Q = std::monostate)
    bool maker = false;
    OrdType order_type = OrdType::Limit;
    Status req = Status::None;
    Status status = Status::None;
    Side side = Side::None;
    TimeInForce time_in_force = TimeInForce::GTC;

    // ── Constructors ─────────────────────────────────────────────────────────
    Order() = default;

    // Explicitly defaulted copy/move to ensure they're available even when
    // the converting constructor below has the same parameter shape for Q=monostate.
    Order(const Order &) = default;
    Order(Order &&) = default;
    Order &operator=(const Order &) = default;
    Order &operator=(Order &&) = default;

    Order(OrderId oid, int64_t price_tick_, double tick_size_, double qty_, Side side_,
          OrdType ord_type, TimeInForce tif)
        : qty(qty_), leaves_qty(qty_), price_tick(price_tick_), tick_size(tick_size_),
          order_id(oid), order_type(ord_type), side(side_), time_in_force(tif) {}

    /// Converting constructor: promote a plain Order<> into Order<Q>.
    /// Used by the exchange to add queue scratch to an order received from the bus.
    /// This is a template constructor so it never suppresses the copy constructor.
    template <typename Q2>
        requires(std::is_same_v<Q2, std::monostate> && !std::is_same_v<Q, std::monostate>)
    explicit Order(const Order<Q2> &plain)
        : qty(plain.qty), leaves_qty(plain.leaves_qty), exec_qty(plain.exec_qty),
          exec_price_tick(plain.exec_price_tick), price_tick(plain.price_tick),
          tick_size(plain.tick_size), exch_timestamp(plain.exch_timestamp),
          local_timestamp(plain.local_timestamp), order_id(plain.order_id), maker(plain.maker),
          order_type(plain.order_type), req(plain.req), status(plain.status), side(plain.side),
          time_in_force(plain.time_in_force) {}

    // ── Accessors ─────────────────────────────────────────────────────────────
    [[nodiscard]] double price() const noexcept { return price_tick * tick_size; }
    [[nodiscard]] double exec_price() const noexcept { return exec_price_tick * tick_size; }

    [[nodiscard]] bool cancellable() const noexcept {
        return (status == Status::New || status == Status::PartiallyFilled) && req == Status::None;
    }
    [[nodiscard]] bool active() const noexcept {
        return status == Status::New || status == Status::PartiallyFilled;
    }
    [[nodiscard]] bool pending() const noexcept { return req != Status::None; }

    // ── update() — used by processors to apply response from exchange ─────────
    void update(const Order &o) noexcept {
        if (o.exch_timestamp < exch_timestamp) {
            // Warning: out-of-order timestamp (matches Rust behaviour — print + continue)
            // In production code this would go to a proper logger.
        }
        qty = o.qty;
        leaves_qty = o.leaves_qty;
        price_tick = o.price_tick;
        tick_size = o.tick_size;
        side = o.side;
        time_in_force = o.time_in_force;
        if (o.exch_timestamp > 0)
            exch_timestamp = o.exch_timestamp;
        status = o.status;
        req = o.req;
        exec_price_tick = o.exec_price_tick;
        exec_qty = o.exec_qty;
        order_id = o.order_id;
        q = o.q;
        maker = o.maker;
        order_type = o.order_type;
    }

    /// Strip queue scratch and return a plain Order<> view (for Bot API).
    [[nodiscard]] Order<> to_plain() const noexcept {
        Order<> r;
        r.qty = qty;
        r.leaves_qty = leaves_qty;
        r.exec_qty = exec_qty;
        r.exec_price_tick = exec_price_tick;
        r.price_tick = price_tick;
        r.tick_size = tick_size;
        r.exch_timestamp = exch_timestamp;
        r.local_timestamp = local_timestamp;
        r.order_id = order_id;
        r.maker = maker;
        r.order_type = order_type;
        r.req = req;
        r.status = status;
        r.side = side;
        r.time_in_force = time_in_force;
        return r;
    }
};

// ── StateValues ───────────────────────────────────────────────────────────────
// Binary-compatible with state_values_dtype from types.py.
struct StateValues {
    double position = 0.0;
    double balance = 0.0;
    double fee = 0.0;
    int64_t num_trades = 0;
    double trading_volume = 0.0;
    double trading_value = 0.0;
};
static_assert(std::is_trivially_copyable_v<StateValues>);

// ── ElapseResult ──────────────────────────────────────────────────────────────
enum class ElapseResult {
    Ok,
    EndOfData,
    MarketFeed,
    OrderResponse,
};

} // namespace hbt
