#include "chronoring/raft_cluster.hpp"

#include <algorithm>

namespace chronoring {

// ============================================================================
// RaftNode
// ============================================================================

std::uint64_t RaftNode::last_log_index() const noexcept {
    return static_cast<std::uint64_t>(log_.size());
}

std::uint64_t RaftNode::last_log_term() const noexcept {
    return log_.empty() ? 0ULL : log_.back().term;
}

std::uint64_t RaftNode::term_at(std::uint64_t index) const noexcept {
    if (index == 0 || index > log_.size()) {
        return 0ULL;
    }
    return log_[static_cast<std::size_t>(index - 1ULL)].term;
}

std::vector<EventFrame> RaftNode::committed_frames() const {
    const std::size_t limit =
        std::min<std::size_t>(static_cast<std::size_t>(commit_index_), log_.size());
    std::vector<EventFrame> out;
    out.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
        out.push_back(log_[i].frame);
    }
    return out;
}

RequestVoteReq RaftNode::start_election() {
    ++current_term_;
    role_ = RaftRole::Candidate;
    voted_for_ = node_id_;
    return RequestVoteReq{current_term_, node_id_, last_log_index(), last_log_term()};
}

void RaftNode::step_down(std::uint64_t new_term) noexcept {
    if (new_term > current_term_) {
        current_term_ = new_term;
        voted_for_ = -1;
    }
    role_ = RaftRole::Follower;
}

RequestVoteResp RaftNode::handle_request_vote(const RequestVoteReq& req) noexcept {
    if (req.term < current_term_) {
        return RequestVoteResp{current_term_, false};
    }
    if (req.term > current_term_) {
        step_down(req.term);
    }

    const std::uint64_t my_term = last_log_term();
    const std::uint64_t my_idx = last_log_index();
    const bool log_ok = (req.last_log_term > my_term) ||
                        (req.last_log_term == my_term && req.last_log_index >= my_idx);
    const bool vote_ok = (voted_for_ == -1 || voted_for_ == req.candidate_id);

    if (vote_ok && log_ok) {
        voted_for_ = req.candidate_id;
        return RequestVoteResp{current_term_, true};
    }
    return RequestVoteResp{current_term_, false};
}

void RaftNode::become_leader() noexcept {
    role_ = RaftRole::Leader;
}

std::uint64_t RaftNode::append_local_entry(const EventFrame& frame) {
    const std::uint64_t idx = last_log_index() + 1ULL;
    log_.push_back(RaftLogEntry{current_term_, idx, frame});
    return idx;
}

void RaftNode::advance_commit(std::uint64_t new_commit_index) noexcept {
    const std::uint64_t max_idx = last_log_index();
    commit_index_ = std::max(commit_index_, std::min(new_commit_index, max_idx));
}

AppendEntriesResp RaftNode::handle_append_entries(const AppendEntriesReq& req) {
    if (req.term < current_term_) {
        return AppendEntriesResp{current_term_, false, 0};
    }
    if (req.term > current_term_ || role_ != RaftRole::Follower) {
        step_down(req.term);
    }

    if (req.prev_log_index > 0) {
        if (req.prev_log_index > last_log_index() ||
            term_at(req.prev_log_index) != req.prev_log_term) {
            return AppendEntriesResp{current_term_, false, 0};
        }
    }

    std::uint64_t next_idx = req.prev_log_index + 1ULL;
    for (const RaftLogEntry& incoming : req.entries) {
        if (next_idx <= last_log_index()) {
            if (term_at(next_idx) != incoming.term) {
                log_.resize(static_cast<std::size_t>(next_idx - 1ULL));
                log_.push_back(incoming);
            }
        } else {
            log_.push_back(incoming);
        }
        ++next_idx;
    }

    advance_commit(req.leader_commit);
    return AppendEntriesResp{current_term_, true, last_log_index()};
}

// ============================================================================
// RaftRingCluster
// ============================================================================

RaftRingCluster::RaftRingCluster(std::size_t num_nodes)
    : online_(num_nodes, 1u) {
    nodes_.reserve(num_nodes);
    for (std::size_t i = 0; i < num_nodes; ++i) {
        nodes_.emplace_back(static_cast<int>(i));
    }
}

void RaftRingCluster::set_node_online(std::size_t node_id, bool online) noexcept {
    if (node_id < online_.size()) {
        online_[node_id] = online ? 1u : 0u;
    }
}

bool RaftRingCluster::is_node_online(std::size_t node_id) const noexcept {
    return node_id < online_.size() && online_[node_id] != 0u;
}

std::optional<int> RaftRingCluster::leader_id() const noexcept {
    std::optional<int> best_id;
    std::uint64_t best_term = 0;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (is_node_online(i) && nodes_[i].role() == RaftRole::Leader &&
            nodes_[i].current_term() >= best_term) {
            best_term = nodes_[i].current_term();
            best_id = nodes_[i].id();
        }
    }
    return best_id;
}

RingStatus RaftRingCluster::elect_leader(std::size_t candidate_id) {
    if (candidate_id >= nodes_.size() || !is_node_online(candidate_id)) {
        return RingStatus::InvalidArgument;
    }
    // Step down any previous leaders first if reachable at higher term
    std::uint64_t max_term = nodes_[candidate_id].current_term();
    for (const auto& n : nodes_) {
        max_term = std::max(max_term, n.current_term());
    }
    while (nodes_[candidate_id].current_term() < max_term) {
        nodes_[candidate_id].step_down(max_term);
    }

    const RequestVoteReq req = nodes_[candidate_id].start_election();
    std::size_t votes = 1; // Self vote

    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (i == candidate_id || !is_node_online(i)) {
            continue;
        }
        const RequestVoteResp resp = nodes_[i].handle_request_vote(req);
        if (resp.term > nodes_[candidate_id].current_term()) {
            nodes_[candidate_id].step_down(resp.term);
            return RingStatus::IoError;
        }
        if (resp.vote_granted) {
            ++votes;
        }
    }

    if (votes < quorum_size()) {
        nodes_[candidate_id].step_down(req.term);
        return RingStatus::IoError;
    }

    nodes_[candidate_id].become_leader();
    // Send heartbeat to assert leadership and sync followers
    AppendEntriesReq hb{};
    hb.term = nodes_[candidate_id].current_term();
    hb.leader_id = nodes_[candidate_id].id();
    hb.prev_log_index = nodes_[candidate_id].last_log_index();
    hb.prev_log_term = nodes_[candidate_id].last_log_term();
    hb.leader_commit = nodes_[candidate_id].commit_index();
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (i != candidate_id && is_node_online(i)) {
            (void)nodes_[i].handle_append_entries(hb);
        }
    }
    return RingStatus::Ok;
}

RingStatus RaftRingCluster::replicate_frame(const EventFrame& frame) {
    const auto lid = leader_id();
    if (!lid.has_value()) {
        return RingStatus::Closed;
    }
    const std::size_t lidx = static_cast<std::size_t>(*lid);

    // Pre-check online quorum so failed quorum does not leave uncommitted tail divergence.
    std::size_t online_cnt = 0;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (is_node_online(i)) {
            ++online_cnt;
        }
    }
    if (online_cnt < quorum_size()) {
        return RingStatus::IoError;
    }

    const std::uint64_t new_index = nodes_[lidx].append_local_entry(frame);
    const auto& l_log = nodes_[lidx].log();
    std::size_t acks = 1; // Leader itself

    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (i == lidx || !is_node_online(i)) {
            continue;
        }
        // Catch up follower from matching prefix index.
        std::uint64_t prev_idx = new_index - 1ULL;
        for (;;) {
            AppendEntriesReq req{};
            req.term = nodes_[lidx].current_term();
            req.leader_id = nodes_[lidx].id();
            req.prev_log_index = prev_idx;
            req.prev_log_term = nodes_[lidx].term_at(prev_idx);
            req.leader_commit = nodes_[lidx].commit_index();
            for (std::size_t j = static_cast<std::size_t>(prev_idx); j < l_log.size(); ++j) {
                req.entries.push_back(l_log[j]);
            }
            const AppendEntriesResp resp = nodes_[i].handle_append_entries(req);
            if (resp.success) {
                ++acks;
                break;
            }
            if (prev_idx == 0) {
                break;
            }
            --prev_idx;
        }
    }

    if (acks < quorum_size()) {
        return RingStatus::IoError;
    }

    nodes_[lidx].advance_commit(new_index);
    // Propagate updated leader_commit across online followers
    AppendEntriesReq commit_sync{};
    commit_sync.term = nodes_[lidx].current_term();
    commit_sync.leader_id = nodes_[lidx].id();
    commit_sync.prev_log_index = new_index;
    commit_sync.prev_log_term = nodes_[lidx].term_at(new_index);
    commit_sync.leader_commit = nodes_[lidx].commit_index();
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (i != lidx && is_node_online(i)) {
            (void)nodes_[i].handle_append_entries(commit_sync);
        }
    }
    return RingStatus::Ok;
}

std::size_t RaftRingCluster::replicate_batch(std::span<const EventFrame> frames) {
    std::size_t replicated = 0;
    for (const EventFrame& f : frames) {
        if (replicate_frame(f) != RingStatus::Ok) {
            break;
        }
        ++replicated;
    }
    return replicated;
}

} // namespace chronoring
