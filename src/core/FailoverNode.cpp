#include "core/FailoverNode.hpp"

#include <spdlog/spdlog.h>

namespace easyfailover {

FailoverNode::FailoverNode(std::string node_id, std::string vip_address, const int priority)
    : node_id_(std::move(node_id)), vip_address_(std::move(vip_address)), priority_(priority) {}

const std::string& FailoverNode::nodeId() const {
    return node_id_;
}

const std::string& FailoverNode::vipAddress() const {
    return vip_address_;
}

int FailoverNode::priority() const {
    return priority_;
}

NodeState FailoverNode::state() const {
    return state_;
}

void FailoverNode::printStatus() const {
    spdlog::info("node_id={} state={} priority={} vip={}", node_id_, toString(state_), priority_,
                 vip_address_);
}

void FailoverNode::becomeMaster() {
    setState(NodeState::Master);
}

void FailoverNode::becomeBackup() {
    setState(NodeState::Backup);
}

void FailoverNode::enterFault() {
    setState(NodeState::Fault);
}

void FailoverNode::setState(const NodeState next_state) {
    if (state_ == next_state) {
        return;
    }

    spdlog::info("node {} state change: {} -> {}", node_id_, toString(state_), toString(next_state));
    state_ = next_state;
}

} // namespace easyfailover
