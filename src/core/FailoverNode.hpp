#pragma once

#include "core/NodeState.hpp"

#include <string>

namespace easyfailover {

class FailoverNode {
  public:
    FailoverNode(std::string node_id, std::string vip_address, int priority);

    [[nodiscard]] const std::string& nodeId() const;
    [[nodiscard]] const std::string& vipAddress() const;
    [[nodiscard]] int priority() const;
    [[nodiscard]] NodeState state() const;

    void printStatus() const;
    void becomeMaster();
    void becomeBackup();
    void enterFault();

  private:
    void setState(NodeState next_state);

    std::string node_id_;
    std::string vip_address_;
    int priority_ = 0;
    NodeState state_ = NodeState::Backup;
};

} // namespace easyfailover
