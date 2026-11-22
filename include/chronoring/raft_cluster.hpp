#pragma once

#include "chronoring/event_frame.hpp"
#include "chronoring/status.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chronoring {

enum class RaftRole : std::uint8_t {
    Follower  = 0,
    Candidate = 1,
    Leader    = 2
};

struct RaftLogEntry {
    std::uint64_t term{0};
    std::uint64_t index{0}; // 1-based monotonic log index
    EventFrame frame{};
};

struct RequestVoteReq {
    std::uint64_t term{0};
    int candidate_id{-1};
    std::uint64_t last_log_index{0};
    std::uint64_t last_log_term{0};
};

struct RequestVoteResp {
    std::uint64_t term{0};
    bool vote_granted{false};
};

struct AppendEntriesReq {
    std::uint64_t term{0};
    int leader_id{-1};
    std::uint64_t prev_log_index{0};
    std::uint64_t prev_log_term{0};
    std::vector<RaftLogEntry> entries{};
    std::uint64_t leader_commit{0};
};

struct AppendEntriesResp {
    std::uint64_t term{0};
    bool success{false};
    std::uint64_t match_index{0};
};

// Raft consensus node forming a replicated logical event ring across cluster members.
class RaftNode {
public:
    explicit RaftNode(int node_id) noexcept : node_id_(node_id) {}

    [[nodiscard]] int id() const noexcept { return node_id_; }
    [[nodiscard]] RaftRole role() const noexcept { return role_; }
    [[nodiscard]] std::uint64_t current_term() const noexcept { return current_term_; }
    [[nodiscard]] std::uint64_t commit_index() const noexcept { return commit_index_; }
    [[nodiscard]] std::uint64_t last_log_index() const noexcept;
    [[nodiscard]] std::uint64_t last_log_term() const noexcept;
    [[nodiscard]] std::uint64_t term_at(std::uint64_t index) const noexcept;
    [[nodiscard]] const std::vector<RaftLogEntry>& log() const noexcept { return log_; }
    [[nodiscard]] std::vector<EventFrame> committed_frames() const;

    [[nodiscard]] RequestVoteReq start_election();
    [[nodiscard]] RequestVoteResp handle_request_vote(const RequestVoteReq& req) noexcept;
    void become_leader() noexcept;
    void step_down(std::uint64_t new_term) noexcept;

    [[nodiscard]] AppendEntriesResp handle_append_entries(const AppendEntriesReq& req);
    [[nodiscard]] std::uint64_t append_local_entry(const EventFrame& frame);
    void advance_commit(std::uint64_t new_commit_index) noexcept;

private:
    int node_id_{0};
    RaftRole role_{RaftRole::Follower};
    std::uint64_t current_term_{0};
    int voted_for_{-1};
    std::uint64_t commit_index_{0};
    std::vector<RaftLogEntry> log_{};
};

// Coordinates an N-node Raft cluster forming a single distributed logical ring
// with quorum commit enforcement and partition/failover simulation.
class RaftRingCluster {
public:
    explicit RaftRingCluster(std::size_t num_nodes);

    void set_node_online(std::size_t node_id, bool online) noexcept;
    [[nodiscard]] bool is_node_online(std::size_t node_id) const noexcept;
    [[nodiscard]] std::optional<int> leader_id() const noexcept;
    [[nodiscard]] std::size_t quorum_size() const noexcept { return (nodes_.size() / 2U) + 1U; }

    [[nodiscard]] RingStatus elect_leader(std::size_t candidate_id);
    [[nodiscard]] RingStatus replicate_frame(const EventFrame& frame);
    [[nodiscard]] std::size_t replicate_batch(std::span<const EventFrame> frames);

    [[nodiscard]] const RaftNode& node(std::size_t id) const { return nodes_[id]; }
    [[nodiscard]] std::size_t num_nodes() const noexcept { return nodes_.size(); }

private:
    std::vector<RaftNode> nodes_{};
    std::vector<std::uint8_t> online_{};
};

} // namespace chronoring
