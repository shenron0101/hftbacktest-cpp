#include <hbt/hbt.hpp>

#include <iostream>

int main() {
    hbt::HashMapMarketDepth depth{0.01, 0.001};
    depth.update_bid_depth(99.99, 1.0, 1);
    depth.update_ask_depth(100.01, 1.0, 1);
    std::cout << depth.best_bid() << ' ' << depth.best_ask() << '\n';
}
