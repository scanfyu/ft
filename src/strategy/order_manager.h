// Copyright [2020] <Copyright Kevin, kevin.lau.gd@gmail.com>

#ifndef FT_SRC_STRATEGY_ORDER_MANAGER_H_
#define FT_SRC_STRATEGY_ORDER_MANAGER_H_

#include <unordered_map>

#include "protocol/data_types.h"
#include "utils/misc.h"

namespace ft {

struct OrderInfo {
  uint32_t client_order_id = 0;
  uint32_t order_id = 0;
  uint32_t ticker_id = 0;
  Direction direction = Direction::kUnknown;
  Offset offset = Offset::kUnknown;
  int original_volume = 0;
  int traded_volume = 0;
};

class OrderManager {
 public:
  void UpdateOrderStatus(const OrderResponse& rsp);

  void ForEach(std::function<bool(const OrderInfo&)>&& f) {
    for (const auto& [order_id, order_info] : order_map_) {
      UNUSED(order_id);
      if (!f(order_info)) return;
    }
  }

 private:
  std::unordered_map<uint64_t, OrderInfo> order_map_;
};

}  // namespace ft

#endif  // FT_SRC_STRATEGY_ORDER_MANAGER_H_
