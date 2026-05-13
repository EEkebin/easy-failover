#include "config/Config.hpp"
#include "core/Election.hpp"
#include "core/FailoverDecision.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using easyfailover::CandidateNode;
using easyfailover::Config;
using easyfailover::FailoverAction;
using easyfailover::LocalNodeStatus;
using easyfailover::PeerConfig;
using easyfailover::PeerStatus;
using easyfailover::chooseHighestPriorityHealthyNode;
using easyfailover::decideFailoverAction;
using easyfailover::loadConfigFromFile;

class TestRunner {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << message << '\n';
        }
    }

    template <typename Func>
    void run(const std::string_view name, Func&& func) {
        const auto failures_before = failures_;
        try {
            func();
            if (failures_ == failures_before) {
                std::cout << "PASS: " << name << '\n';
            } else {
                std::cerr << "FAIL: " << name << '\n';
            }
        } catch (const std::exception& error) {
            ++failures_;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures_;
            std::cerr << "FAIL: " << name << ": unknown exception\n";
        }
    }

    [[nodiscard]] int failures() const {
        return failures_;
    }

  private:
    int failures_ = 0;
};

bool contains(const std::vector<std::string>& values, const std::string_view expected) {
    return std::find(values.begin(), values.end(), expected) != values.end();
}

Config validConfig() {
    Config config;
    config.node_id = "node-a";
    config.priority = 100;
    config.vip.address = "10.0.0.50/24";
    config.vip.interface = "eth0";
    config.heartbeat.bind = "0.0.0.0:7432";
    config.heartbeat.interval_ms = 1000;
    config.heartbeat.timeout_ms = 3000;
    config.health.command = "curl -fsS http://127.0.0.1:8080/health";
    config.health.interval_ms = 1000;
    config.health.timeout_ms = 2000;
    config.api.enabled = false;
    config.api.bind = "127.0.0.1:8743";
    config.peers = {PeerConfig{.id = "node-b", .address = "10.0.0.12:7432"}};
    return config;
}

void testValidConfigHasNoValidationErrors(TestRunner& runner) {
    const auto config = validConfig();
    runner.expect(config.validate().empty(), "valid config should not produce validation errors");
}

void testConfigValidationRequiredFields(TestRunner& runner) {
    auto config = validConfig();
    config.node_id.clear();
    config.priority = 0;
    config.vip.address.clear();
    config.vip.interface.clear();
    config.heartbeat.bind.clear();
    config.heartbeat.interval_ms = 0;
    config.heartbeat.timeout_ms = 0;
    config.health.interval_ms = 0;
    config.health.timeout_ms = 0;
    config.peers = {PeerConfig{.id = "", .address = ""}};
    config.api.enabled = true;
    config.api.bind.clear();

    const auto errors = config.validate();
    runner.expect(contains(errors, "node_id must not be empty"), "node_id should be required");
    runner.expect(contains(errors, "priority must be positive"), "positive priority should be required");
    runner.expect(contains(errors, "vip.address must not be empty"), "VIP address should be required");
    runner.expect(contains(errors, "vip.interface must not be empty"), "VIP interface should be required");
    runner.expect(contains(errors, "heartbeat.bind must not be empty"),
                  "heartbeat bind should be required");
    runner.expect(contains(errors, "heartbeat.interval_ms must be positive"),
                  "heartbeat interval should be required");
    runner.expect(contains(errors, "heartbeat.timeout_ms must be positive"),
                  "heartbeat timeout should be required");
    runner.expect(contains(errors, "health.interval_ms must be positive"),
                  "health interval should be required");
    runner.expect(contains(errors, "health.timeout_ms must be positive"),
                  "health timeout should be required");
    runner.expect(contains(errors, "peers[].id must not be empty"), "peer id should be required");
    runner.expect(contains(errors, "peers[].address must not be empty"),
                  "peer address should be required");
    runner.expect(contains(errors, "api.bind must not be empty when api.enabled is true"),
                  "API bind should be required when API is enabled");
}

void testInvalidHeartbeatConfigFixture(TestRunner& runner) {
    const auto config = loadConfigFromFile("tests/fixtures/config/invalid-heartbeat.toml");
    const auto errors = config.validate();

    runner.expect(contains(errors, "heartbeat.bind must not be empty"),
                  "invalid heartbeat fixture should require bind");
    runner.expect(contains(errors, "heartbeat.interval_ms must be positive"),
                  "invalid heartbeat fixture should require positive interval");
    runner.expect(contains(errors, "heartbeat.timeout_ms must be positive"),
                  "invalid heartbeat fixture should require positive timeout");
}

void testInvalidHealthConfigFixture(TestRunner& runner) {
    const auto config = loadConfigFromFile("tests/fixtures/config/invalid-health.toml");
    const auto errors = config.validate();

    runner.expect(contains(errors, "health.interval_ms must be positive"),
                  "invalid health fixture should require positive interval");
    runner.expect(contains(errors, "health.timeout_ms must be positive"),
                  "invalid health fixture should require positive timeout");
}

void testInvalidApiConfigFixture(TestRunner& runner) {
    const auto config = loadConfigFromFile("tests/fixtures/config/invalid-api.toml");
    const auto errors = config.validate();

    runner.expect(contains(errors, "api.bind must not be empty when api.enabled is true"),
                  "invalid API fixture should require bind when enabled");
}

void testMinimalConfigAppliesDefaults(TestRunner& runner) {
    const auto config = loadConfigFromFile("tests/fixtures/config/minimal.toml");

    runner.expect(!config.node_id.empty(), "minimal config should default node_id from hostname");
    runner.expect(config.priority == 100, "minimal config should default priority");
    runner.expect(config.vip.address == "10.0.0.50/24", "minimal config should load VIP address");
    runner.expect(config.vip.interface == "eth0", "minimal config should load VIP interface");
    runner.expect(config.heartbeat.bind == "0.0.0.0:7432",
                  "minimal config should default heartbeat bind");
    runner.expect(config.heartbeat.interval_ms == 1000,
                  "minimal config should default heartbeat interval");
    runner.expect(config.heartbeat.timeout_ms == 3000,
                  "minimal config should default heartbeat timeout");
    runner.expect(config.health.command.empty(),
                  "minimal config should default to no health command");
    runner.expect(config.health.interval_ms == 1000,
                  "minimal config should default health interval");
    runner.expect(config.health.timeout_ms == 2000,
                  "minimal config should default health timeout");
    runner.expect(!config.election.require_quorum,
                  "minimal config should default quorum requirement off");
    runner.expect(config.election.preempt, "minimal config should default preemption on");
    runner.expect(!config.api.enabled, "minimal config should default API disabled");
    runner.expect(config.api.bind == "127.0.0.1:8743", "minimal config should default API bind");
    runner.expect(config.api.read_only, "minimal config should default API read-only");
    runner.expect(config.peers.size() == 2, "minimal config should load peers");
    runner.expect(config.validate().empty(), "minimal config with defaults should validate");
}

void testConfigRequiresAtLeastOnePeer(TestRunner& runner) {
    const auto config = loadConfigFromFile("tests/fixtures/config/no-peers.toml");
    const auto errors = config.validate();

    runner.expect(contains(errors, "at least one peer must be configured"),
                  "config without peers should fail validation");
}

void testConfigRejectsInvalidOptionalScalarType(TestRunner& runner) {
    bool threw = false;
    try {
        static_cast<void>(loadConfigFromFile("tests/fixtures/config/invalid-priority-type.toml"));
    } catch (const std::runtime_error& error) {
        threw = true;
        runner.expect(std::string{error.what()}.find("Invalid type for config key: priority") !=
                          std::string::npos,
                      "invalid optional scalar type should report config key");
    }
    runner.expect(threw, "invalid optional scalar type should throw");
}

void testConfigRejectsInvalidOptionalTableType(TestRunner& runner) {
    bool threw = false;
    try {
        static_cast<void>(
            loadConfigFromFile("tests/fixtures/config/invalid-heartbeat-table-type.toml"));
    } catch (const std::runtime_error& error) {
        threw = true;
        runner.expect(std::string{error.what()}.find("Invalid type for TOML table: heartbeat") !=
                          std::string::npos,
                      "invalid optional table type should report table key");
    }
    runner.expect(threw, "invalid optional table type should throw");
}

void testSampleConfigLoads(TestRunner& runner) {
    const auto config = loadConfigFromFile("configs/easy-failover.toml");

    runner.expect(config.node_id == "node-a", "sample node_id should load");
    runner.expect(config.priority == 100, "sample priority should load");
    runner.expect(config.vip.address == "10.0.0.50/24", "sample VIP address should load");
    runner.expect(config.vip.interface == "eth0", "sample VIP interface should load");
    runner.expect(config.heartbeat.bind == "0.0.0.0:7432", "sample heartbeat bind should load");
    runner.expect(config.health.command == "curl -fsS http://127.0.0.1:8080/health",
                  "sample health command should load");
    runner.expect(config.election.preempt, "sample election preempt should load");
    runner.expect(!config.election.require_quorum, "sample election quorum should load");
    runner.expect(!config.api.enabled, "sample API enabled flag should load");
    runner.expect(config.api.read_only, "sample API read-only flag should load");
    runner.expect(config.peers.size() == 2, "sample peers should load");
    runner.expect(config.validate().empty(), "loaded sample config should validate");
}

void testElectionChoosesHighestHealthyPriority(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-b", .priority = 200, .healthy = true},
        CandidateNode{.node_id = "node-c", .priority = 50, .healthy = true},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(winner.has_value(), "healthy candidates should produce a winner");
    runner.expect(winner->node_id == "node-b", "highest priority healthy node should win");
}

void testElectionIgnoresUnhealthyNodes(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-b", .priority = 500, .healthy = false},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(winner.has_value(), "healthy candidate should win over unhealthy candidate");
    runner.expect(winner->node_id == "node-a", "unhealthy high-priority node should be ignored");
}

void testElectionTieBreaksByLowestNodeId(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-c", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-b", .priority = 100, .healthy = true},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(winner.has_value(), "healthy tied candidates should produce a winner");
    runner.expect(winner->node_id == "node-a", "lexicographically lowest node_id should win ties");
}

void testElectionReturnsNoWinnerWithoutHealthyNodes(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = false},
        CandidateNode{.node_id = "node-b", .priority = 200, .healthy = false},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(!winner.has_value(), "no healthy candidates should produce no winner");
}

void testElectionEmptyCandidatesReturnsNoWinner(TestRunner& runner) {
    const std::vector<CandidateNode> candidates;

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(!winner.has_value(), "empty candidate list should produce no winner");
}

void testElectionTieBreakIsIndependentOfInputOrder(TestRunner& runner) {
    std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-z", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-m", .priority = 100, .healthy = true},
    };

    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateNode& left, const CandidateNode& right) {
                  return left.node_id < right.node_id;
              });

    do {
        const auto winner = chooseHighestPriorityHealthyNode(candidates);
        runner.expect(winner.has_value(), "healthy tied candidates should produce a winner");
        runner.expect(winner->node_id == "node-a",
                      "tie-break should choose lexicographically lowest node_id regardless of input order");
    } while (std::next_permutation(
        candidates.begin(), candidates.end(),
        [](const CandidateNode& left, const CandidateNode& right) {
            return left.node_id < right.node_id;
        }));
}

void testElectionDuplicateNodeIdsUseHighestPriorityDuplicate(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = true},
        CandidateNode{.node_id = "node-a", .priority = 200, .healthy = true},
        CandidateNode{.node_id = "node-b", .priority = 150, .healthy = true},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(winner.has_value(), "duplicate candidate ids should still produce a winner");
    runner.expect(winner->node_id == "node-a", "highest-priority duplicate should win");
    runner.expect(winner->priority == 200, "winning duplicate should keep its priority");
}

void testElectionNonPositivePriorityCurrentBehavior(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-a", .priority = 0, .healthy = true},
        CandidateNode{.node_id = "node-b", .priority = -10, .healthy = true},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(winner.has_value(), "healthy non-positive priority candidates can still win");
    runner.expect(winner->node_id == "node-a", "priority zero should beat lower healthy priority");
}

void testElectionUnhealthyDuplicateDoesNotOverrideHealthyDuplicate(TestRunner& runner) {
    const std::vector<CandidateNode> candidates{
        CandidateNode{.node_id = "node-a", .priority = 1000, .healthy = false},
        CandidateNode{.node_id = "node-a", .priority = 100, .healthy = true},
    };

    const auto winner = chooseHighestPriorityHealthyNode(candidates);
    runner.expect(winner.has_value(), "healthy duplicate should produce a winner");
    runner.expect(winner->node_id == "node-a", "healthy duplicate should win");
    runner.expect(winner->priority == 100, "unhealthy duplicate priority should be ignored");
}

LocalNodeStatus localStatus(const std::string& node_id, const int priority, const bool healthy,
                            const easyfailover::NodeState state) {
    return LocalNodeStatus{
        .node_id = node_id,
        .priority = priority,
        .healthy = healthy,
        .state = state,
    };
}

PeerStatus peerStatus(const std::string& node_id, const int priority, const bool healthy,
                      const bool heartbeat_seen) {
    return PeerStatus{
        .node_id = node_id,
        .priority = priority,
        .healthy = healthy,
        .heartbeat_seen = heartbeat_seen,
    };
}

void testUnhealthyLocalEntersFault(TestRunner& runner) {
    const auto local = localStatus("node-a", 100, false, easyfailover::NodeState::Backup);
    const std::vector<PeerStatus> peers{peerStatus("node-b", 200, true, true)};

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::EnterFault,
                  "unhealthy local node should enter fault");
    runner.expect(!decision.selected_master.has_value(), "fault decision should not select a master");
}

void testHealthyLocalWithoutLivePeersBecomesMaster(TestRunner& runner) {
    const auto local = localStatus("node-a", 100, true, easyfailover::NodeState::Backup);
    const std::vector<PeerStatus> peers;

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::BecomeMaster,
                  "healthy local node should become master when no peers are live");
    runner.expect(decision.selected_master == "node-a", "local node should be selected as master");
}

void testLocalWinnerStaysMaster(TestRunner& runner) {
    const auto local = localStatus("node-a", 200, true, easyfailover::NodeState::Master);
    const std::vector<PeerStatus> peers{peerStatus("node-b", 100, true, true)};

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::StayMaster,
                  "winning local master should stay master");
    runner.expect(decision.selected_master == "node-a", "local master should remain selected");
}

void testPeerWinnerKeepsLocalBackup(TestRunner& runner) {
    const auto local = localStatus("node-a", 100, true, easyfailover::NodeState::Backup);
    const std::vector<PeerStatus> peers{peerStatus("node-b", 200, true, true)};

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::StayBackup,
                  "backup should stay backup when peer wins");
    runner.expect(decision.selected_master == "node-b", "winning peer should be selected");
}

void testPeerWinnerDemotesLocalMaster(TestRunner& runner) {
    const auto local = localStatus("node-a", 100, true, easyfailover::NodeState::Master);
    const std::vector<PeerStatus> peers{peerStatus("node-b", 200, true, true)};

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::BecomeBackup,
                  "local master should become backup when peer wins");
    runner.expect(decision.selected_master == "node-b", "winning peer should be selected");
}

void testDecisionIgnoresUnhealthyAndMissingHeartbeatPeers(TestRunner& runner) {
    const auto local = localStatus("node-a", 100, true, easyfailover::NodeState::Backup);
    const std::vector<PeerStatus> peers{
        peerStatus("node-b", 500, false, true),
        peerStatus("node-c", 400, true, false),
    };

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::BecomeMaster,
                  "ignored peers should allow local node to win");
    runner.expect(decision.selected_master == "node-a", "local node should be selected");
}

void testDecisionTieBreaksByLowestNodeId(TestRunner& runner) {
    const auto local = localStatus("node-b", 100, true, easyfailover::NodeState::Backup);
    const std::vector<PeerStatus> peers{
        peerStatus("node-a", 100, true, true),
        peerStatus("node-c", 100, true, true),
    };

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::StayBackup,
                  "local backup should stay backup when peer wins tie-break");
    runner.expect(decision.selected_master == "node-a",
                  "lexicographically lowest node_id should win tie-break");
}

void testDecisionTieBreaksInFavorOfLocalNode(TestRunner& runner) {
    const auto local = localStatus("node-a", 100, true, easyfailover::NodeState::Backup);
    const std::vector<PeerStatus> peers{
        peerStatus("node-b", 100, true, true),
        peerStatus("node-c", 100, true, true),
    };

    const auto decision = decideFailoverAction(local, peers);
    runner.expect(decision.action == FailoverAction::BecomeMaster,
                  "local node should become master when it wins tie-break");
    runner.expect(decision.selected_master == "node-a", "local node should be selected");
}

} // namespace

int main() {
    TestRunner runner;

    runner.run("valid config has no validation errors", [&runner] {
        testValidConfigHasNoValidationErrors(runner);
    });
    runner.run("config validation reports required fields", [&runner] {
        testConfigValidationRequiredFields(runner);
    });
    runner.run("invalid heartbeat config fixture reports validation errors", [&runner] {
        testInvalidHeartbeatConfigFixture(runner);
    });
    runner.run("invalid health config fixture reports validation errors", [&runner] {
        testInvalidHealthConfigFixture(runner);
    });
    runner.run("invalid API config fixture reports validation errors", [&runner] {
        testInvalidApiConfigFixture(runner);
    });
    runner.run("minimal config applies defaults", [&runner] {
        testMinimalConfigAppliesDefaults(runner);
    });
    runner.run("config requires at least one peer", [&runner] {
        testConfigRequiresAtLeastOnePeer(runner);
    });
    runner.run("config rejects invalid optional scalar type", [&runner] {
        testConfigRejectsInvalidOptionalScalarType(runner);
    });
    runner.run("config rejects invalid optional table type", [&runner] {
        testConfigRejectsInvalidOptionalTableType(runner);
    });
    runner.run("sample config loads", [&runner] { testSampleConfigLoads(runner); });
    runner.run("election chooses highest healthy priority", [&runner] {
        testElectionChoosesHighestHealthyPriority(runner);
    });
    runner.run("election ignores unhealthy nodes", [&runner] {
        testElectionIgnoresUnhealthyNodes(runner);
    });
    runner.run("election tie-breaks by lowest node id", [&runner] {
        testElectionTieBreaksByLowestNodeId(runner);
    });
    runner.run("election returns no winner without healthy nodes", [&runner] {
        testElectionReturnsNoWinnerWithoutHealthyNodes(runner);
    });
    runner.run("election empty candidates returns no winner", [&runner] {
        testElectionEmptyCandidatesReturnsNoWinner(runner);
    });
    runner.run("election tie-break is independent of input order", [&runner] {
        testElectionTieBreakIsIndependentOfInputOrder(runner);
    });
    runner.run("election duplicate node ids use highest-priority duplicate", [&runner] {
        testElectionDuplicateNodeIdsUseHighestPriorityDuplicate(runner);
    });
    runner.run("election non-positive priority current behavior", [&runner] {
        testElectionNonPositivePriorityCurrentBehavior(runner);
    });
    runner.run("election unhealthy duplicate does not override healthy duplicate", [&runner] {
        testElectionUnhealthyDuplicateDoesNotOverrideHealthyDuplicate(runner);
    });
    runner.run("unhealthy local enters fault", [&runner] { testUnhealthyLocalEntersFault(runner); });
    runner.run("healthy local without live peers becomes master", [&runner] {
        testHealthyLocalWithoutLivePeersBecomesMaster(runner);
    });
    runner.run("local winner stays master", [&runner] { testLocalWinnerStaysMaster(runner); });
    runner.run("peer winner keeps local backup", [&runner] { testPeerWinnerKeepsLocalBackup(runner); });
    runner.run("peer winner demotes local master", [&runner] {
        testPeerWinnerDemotesLocalMaster(runner);
    });
    runner.run("decision ignores unhealthy and missing-heartbeat peers", [&runner] {
        testDecisionIgnoresUnhealthyAndMissingHeartbeatPeers(runner);
    });
    runner.run("decision tie-breaks by lowest node id", [&runner] {
        testDecisionTieBreaksByLowestNodeId(runner);
    });
    runner.run("decision tie-breaks in favor of local node", [&runner] {
        testDecisionTieBreaksInFavorOfLocalNode(runner);
    });

    if (runner.failures() != 0) {
        std::cerr << runner.failures() << " test failure(s)\n";
        return 1;
    }

    return 0;
}
