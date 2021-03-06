// Copyright [2020-2021] <Copyright Kevin, kevin.lau.gd@gmail.com>

#include "gateway/back_test/back_test_gateway.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>

#include "utils/contract_table.h"
#include "utils/protocol_utils.h"

namespace ft {

namespace {

uint64_t GetTradeTime(const TickData& tick) {
  uint64_t trade_time = 0;
  trade_time += (tick.date[0] - '0') * 10000000000000000UL;
  trade_time += (tick.date[1] - '0') * 1000000000000000UL;
  trade_time += (tick.date[2] - '0') * 100000000000000UL;
  trade_time += (tick.date[3] - '0') * 10000000000000UL;
  trade_time += (tick.date[4] - '0') * 1000000000000UL;
  trade_time += (tick.date[5] - '0') * 100000000000UL;
  trade_time += (tick.date[6] - '0') * 10000000000UL;
  trade_time += (tick.date[7] - '0') * 1000000000UL;

  trade_time += ((tick.time_us) / 1000) % 1000;

  uint64_t current_sec = tick.time_us / 1000000;
  trade_time += (current_sec % 60) * 1000;
  trade_time += ((current_sec % 3600) / 60) * 100000;
  trade_time += (current_sec / 3600) * 10000000;

  return trade_time;
}

}  // namespace

bool BackTestGateway::Login(BaseOrderManagementSystem* oms, const Config& config) {
  if (!ContractTable::is_inited()) {
    spdlog::error("BackTestGateway: ContractTable is not inited");
    return false;
  }

  if (!oms) {
    spdlog::error("BackTestGateway: oms is nullptr");
    return false;
  }
  oms_ = oms;

  ctx_.account.account_id = std::stoul(config.investor_id);
  ctx_.account.cash = 10000000.0;
  ctx_.account.total_asset = ctx_.account.cash;
  ctx_.account.frozen = 0.0;
  ctx_.account.margin = 0.0;

  ctx_.portfolio.Init(ContractTable::size(), false);

  if (!LoadHistoryData(config.arg0)) {
    spdlog::error("failed to load history data");
    return false;
  }

  std::thread([this] { BackgroudTask(); }).detach();
  return true;
}

void BackTestGateway::Logout() {}

bool BackTestGateway::SendOrder(const OrderRequest& order, uint64_t* privdata_ptr) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!CheckOrder(order)) {
    return false;
  }
  if (!CheckAndUpdateContext(order)) {
    return false;
  }
  msg_queue_.push(OrderAcceptance{order.order_id});

  auto& tick = get_current_tick(order.contract->ticker_id);
  if (!MatchOrder(order, tick)) {
    if (order.type == OrderType::kLimit) {
      ctx_.pending_orders[order.contract->ticker_id].emplace_back(order);
    } else {
      UpdateCanceled(order);
    }
  }

  *privdata_ptr = order.contract->ticker_id;
  cv_.notify_one();
  return true;
}

bool BackTestGateway::CancelOrder(uint64_t order_id, uint64_t privdata) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto iter = ctx_.pending_orders.find(static_cast<uint32_t>(privdata));
  if (iter == ctx_.pending_orders.end()) {
    return false;
  }

  auto& pending_list = iter->second;
  for (auto it = pending_list.begin(); it != pending_list.end(); ++it) {
    auto& order = *it;
    if (order_id == order.order_id) {
      UpdateCanceled(order);
      pending_list.erase(it);
      cv_.notify_one();
      return true;
    }
  }

  return false;
}

bool BackTestGateway::Subscribe(const std::vector<std::string>& sub_list) { return true; }

bool BackTestGateway::QueryPosition(const std::string& ticker, Position* result) { return true; }

bool BackTestGateway::QueryPositionList(std::vector<Position>* result) { return true; }

bool BackTestGateway::QueryAccount(Account* result) {
  std::unique_lock<std::mutex> lock(mutex_);
  UpdateAccount();
  *result = ctx_.account;
  return true;
}

bool BackTestGateway::QueryTradeList(std::vector<Trade>* result) { return true; }

void BackTestGateway::OnNotify(uint64_t signal) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (current_tick_pos_ >= history_data_.size()) {
    spdlog::info("backtest finished");
    exit(0);
  }
  msg_queue_.push(history_data_[current_tick_pos_++]);
  cv_.notify_one();
}

bool BackTestGateway::LoadHistoryData(const std::string& history_data_file) {
  std::ifstream tick_ifs(history_data_file);
  if (!tick_ifs) {
    spdlog::error("BackTestGateway: tick file not found {}", history_data_file);
    return false;
  }
  std::string line;
  std::vector<std::string> tokens;
  std::getline(tick_ifs, line);
  while (std::getline(tick_ifs, line)) {
    StringSplit(line, ",", &tokens, false);
    if (tokens.size() != 15) {
      spdlog::error("BackTestGateway: invalid tick file {}");
      exit(1);
    }
    history_data_.emplace_back(TickData{});

    auto& tick = history_data_.back();
    struct tm _tm;
    strptime(tokens[0].data() + 11, "%H:%M:%S", &_tm);
    tick.time_us = (_tm.tm_sec + _tm.tm_min * 60UL + _tm.tm_hour * 3600UL) * 1000000UL +
                   std::stoul(tokens[0].data() + 20) * 1000UL;
    std::copy(tokens[0].data(), tokens[0].data() + 4, tick.date);
    std::copy(tokens[0].data() + 5, tokens[0].data() + 7, tick.date + 4);
    std::copy(tokens[0].data() + 8, tokens[0].data() + 10, tick.date + 6);

    tick.last_price = tokens[1].empty() ? 0.0 : std::stod(tokens[1]);
    tick.level = 1;
    tick.ask[0] = tokens[3].empty() ? 0.0 : std::stod(tokens[3]);
    tick.ask_volume[0] = tokens[4].empty() ? 0 : std::stoi(tokens[4]);
    tick.bid[0] = tokens[5].empty() ? 0.0 : std::stoi(tokens[5]);
    tick.bid_volume[0] = tokens[6].empty() ? 0 : std::stoi(tokens[6]);
    tick.highest_price = tokens[7].empty() ? 0.0 : std::stod(tokens[7]);
    tick.lowest_price = tokens[8].empty() ? 0.0 : std::stod(tokens[8]);
    tick.volume = tokens[9].empty() ? 0 : std::stoul(tokens[9]);
    tick.open_interest = tokens[11].empty() ? 0 : std::stoul(tokens[11]);
    auto* contract = ContractTable::get_by_ticker(tokens[13]);
    if (!contract) {
      spdlog::error("BackTestGateway: ticker {} not found", tokens[13]);
      exit(1);
    }
    tick.ticker_id = contract->ticker_id;

    tokens.clear();
  }

  return true;
}

bool BackTestGateway::CheckOrder(const OrderRequest& order) const {
  if (order.volume <= 0) {
    spdlog::error("BackTestGateway::CheckOrder: invalid volume {}", order.volume);
    return false;
  }
  if (order.type != OrderType::kLimit && order.type != OrderType::kMarket &&
      order.type != OrderType::kFak && order.type != OrderType::kFok) {
    spdlog::error("BackTestGateway::CheckOrder: unsupported order type {}", ToString(order.type));
    return false;
  }
  if (order.price <= 0.0) {
    spdlog::error("BackTestGateway::CheckOrder: invalid price {}", order.price);
    return false;
  }
  if (order.direction != Direction::kBuy && order.direction != Direction::kSell) {
    spdlog::error("BackTestGateway::CheckOrder: unsupported direction {}",
                  ToString(order.direction));
    return false;
  }
  return true;
}

bool BackTestGateway::CheckAndUpdateContext(const OrderRequest& order) {
  if (IsOffsetOpen(order.offset)) {
    double fund_needed = order.contract->size * order.volume * order.price;
    if (ctx_.account.cash < fund_needed) {
      spdlog::error("BackTestGateway::CheckAndUpdateContext: 资金不足");
      return false;
    }
    ctx_.account.cash -= fund_needed;
    ctx_.account.frozen += fund_needed;
  } else {
    int available = 0;
    const auto* pos = ctx_.portfolio.get_position(order.contract->ticker_id);
    if (pos) {
      const auto& detail = order.direction == Direction::kSell ? pos->long_pos : pos->short_pos;
      available = detail.holdings - detail.close_pending;
    }

    if (available < order.volume) {
      spdlog::error("BackTestGateway::CheckAndUpdateContext: 仓位不足");
      return false;
    }
  }

  ctx_.portfolio.UpdatePending(order.contract->ticker_id, order.direction, order.offset,
                               order.volume);
  return true;
}

void BackTestGateway::UpdateTraded(const OrderRequest& order, const TickData& tick) {
  double price = order.direction == Direction::kBuy ? tick.ask[0] : tick.bid[0];

  if (IsOffsetOpen(order.offset)) {
    double fund_returned = order.contract->size * order.volume * order.price;
    double cost = order.contract->size * order.volume * price;
    ctx_.account.frozen -= fund_returned;
    ctx_.account.cash += fund_returned;
    ctx_.account.margin += cost;
    ctx_.account.cash -= cost;
  } else {
    UpdateAccount(tick);
    auto* pos = ctx_.portfolio.get_position(order.contract->ticker_id);
    if (!pos) {
      abort();
    }

    auto& pos_detail = order.direction == Direction::kSell ? pos->long_pos : pos->short_pos;
    double fund_returned =
        pos_detail.float_pnl + pos_detail.holdings * pos_detail.cost_price * order.contract->size;

    fund_returned *= (static_cast<double>(order.volume) / pos_detail.holdings);
    ctx_.account.margin -= fund_returned;
    ctx_.account.cash += fund_returned;
  }

  ctx_.portfolio.UpdateTraded(order.contract->ticker_id, order.direction, order.offset,
                              order.volume, price);

  Trade trade{};
  trade.ticker_id = order.contract->ticker_id;
  trade.amount = price * order.volume * order.contract->size;
  trade.direction = order.direction;
  trade.offset = order.offset;
  trade.order_id = order.order_id;
  trade.volume = order.volume;
  trade.price = price;
  trade.trade_type = TradeType::kSecondaryMarket;
  trade.trade_time = GetTradeTime(tick);
  msg_queue_.push(trade);
}

void BackTestGateway::UpdateCanceled(const OrderRequest& order) {
  if (IsOffsetOpen(order.offset)) {
    double fund_returned = order.contract->size * order.volume * order.price;
    ctx_.account.frozen -= fund_returned;
    ctx_.account.cash += fund_returned;
  }
  ctx_.portfolio.UpdatePending(order.contract->ticker_id, order.direction, order.offset,
                               -order.volume);
  msg_queue_.push(OrderCancellation{order.order_id, order.volume});
}

void BackTestGateway::UpdatePnl(const TickData& tick) {
  ctx_.portfolio.UpdateFloatPnl(tick.ticker_id, tick.bid[0], tick.ask[0]);
}

void BackTestGateway::UpdateAccount(const TickData& tick) {
  UpdatePnl(tick);
  ctx_.account.margin = ctx_.portfolio.total_assets();
  ctx_.account.total_asset = ctx_.account.cash + ctx_.account.margin + ctx_.account.frozen;
}

void BackTestGateway::UpdateAccount() {
  for (const auto& [ticker_id, tick] : current_ticks_) {
    UNUSED(ticker_id);
    UpdateAccount(tick);
  }
}

bool BackTestGateway::MatchOrder(const OrderRequest& order, const TickData& tick) {
  if ((order.direction == Direction::kBuy && tick.ask[0] > 0 && order.price >= tick.ask[0]) ||
      (order.direction == Direction::kSell && tick.bid[0] > 0 && order.price <= tick.bid[0])) {
    UpdateTraded(order, tick);
    return true;
  }
  return false;
}

void BackTestGateway::MatchOrders(const TickData& tick) {
  auto& pending_list = ctx_.pending_orders[tick.ticker_id];
  for (auto iter = pending_list.begin(); iter != pending_list.end();) {
    auto& order = *iter;
    if (MatchOrder(order, tick)) {
      iter = pending_list.erase(iter);
    } else {
      ++iter;
    }
  }
}

void BackTestGateway::BackgroudTask() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    cv_.wait(lock, [this] { return !msg_queue_.empty(); });
    while (!msg_queue_.empty()) {
      auto& msg = msg_queue_.front();
      std::visit(*this, msg);
      msg_queue_.pop();
    }
  }
}

REGISTER_GATEWAY(::ft::BackTestGateway);

}  // namespace ft
