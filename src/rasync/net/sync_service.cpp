#include "net/sync_service.h"

#include "net/sync_session.h"

#include "util/fs.h"

#include <filesystem>
#include <utility>
#include <vector>

namespace rasync {

SyncService::SyncService(librats::Node& node, SyncConfig config, SyncEvents events)
    : node_(node), config_(std::move(config)), events_(std::move(events)) {
    if (config_.data_dir.empty())
        config_.data_dir = librats::combine_paths(config_.root, ".rasync");
}

SyncService::~SyncService() { shutdown(); }

void SyncService::attach() {
    node_.on(proto::kChannel, [this](const librats::Peer& peer, librats::ByteView payload) {
        on_message(peer.id(), payload);
    });
    node_.on_peer_connected([this](const librats::Peer& peer) { on_peer_up(peer.id()); });
    node_.on_peer_disconnected([this](const librats::PeerId& id) { on_peer_down(id); });
}

void SyncService::load_baseline() {
    std::string path = librats::combine_paths(config_.data_dir, "baseline.man");
    if (auto m = Manifest::load(path)) {
        std::lock_guard<std::mutex> lk(manifest_mutex_);
        base_ = std::move(*m);
        log(0, "loaded baseline with " + std::to_string(base_.size()) + " entries");
    }
}

void SyncService::clean_temp_dir() {
    std::error_code ec;
    auto removed = std::filesystem::remove_all(temp_dir(), ec);
    if (!ec && removed > 1)  // >1 = the directory itself plus at least one leftover
        log(0, "cleared " + std::to_string(removed - 1) + " stale temp file(s)");
}

Manifest SyncService::local_manifest() const {
    std::lock_guard<std::mutex> lk(manifest_mutex_);
    return local_;
}

Manifest SyncService::base_manifest() const {
    std::lock_guard<std::mutex> lk(manifest_mutex_);
    return base_;
}

void SyncService::set_base_manifest(const Manifest& m) {
    // Hold the lock across the disk write too, so concurrent convergences (e.g.
    // multiple peers) can't interleave writes to the same baseline file.
    std::lock_guard<std::mutex> lk(manifest_mutex_);
    base_ = m;
    librats::create_directories(config_.data_dir.c_str());
    m.save(librats::combine_paths(config_.data_dir, "baseline.man"));
}

void SyncService::note_local_set(const std::string& path, const FileMeta& meta) {
    std::lock_guard<std::mutex> lk(manifest_mutex_);
    local_.set(path, meta);
}

void SyncService::note_local_removed(const std::string& path) {
    std::lock_guard<std::mutex> lk(manifest_mutex_);
    local_.remove(path);
}

void SyncService::set_local_manifest(Manifest m) {
    {
        std::lock_guard<std::mutex> lk(manifest_mutex_);
        local_ = std::move(m);
    }
    // Fan out to every session so it re-advertises and reconciles.
    std::vector<std::shared_ptr<SyncSession>> sessions;
    {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        for (auto& [_, s] : sessions_) sessions.push_back(s);
    }
    for (auto& s : sessions) s->local_changed();
}

std::string SyncService::abs_path(const std::string& rel) const {
    return librats::combine_paths(config_.root, rel);
}

std::string SyncService::temp_dir() const {
    return librats::combine_paths(config_.root, kTempDirName);
}

void SyncService::send(const librats::PeerId& to, const Bytes& msg) {
    node_.send(to, proto::kChannel, librats::ByteView(msg));
}

void SyncService::log(int level, const std::string& msg) const {
    if (events_.log) events_.log(level, msg);
}

std::shared_ptr<SyncSession> SyncService::session_for(const librats::PeerId& id) {
    std::shared_ptr<SyncSession> session;
    bool created = false;
    {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        auto it = sessions_.find(id);
        if (it != sessions_.end()) return it->second;
        session = std::make_shared<SyncSession>(*this, id);
        sessions_.emplace(id, session);
        created = true;
    }
    if (created) session->start();
    return session;
}

void SyncService::on_peer_up(const librats::PeerId& id) {
    session_for(id);
    if (events_.peer_up) events_.peer_up(id);
}

void SyncService::on_peer_down(const librats::PeerId& id) {
    std::shared_ptr<SyncSession> session;
    {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        auto it = sessions_.find(id);
        if (it != sessions_.end()) { session = it->second; sessions_.erase(it); }
    }
    if (session) session->stop();
    if (events_.peer_down) events_.peer_down(id);
}

void SyncService::on_message(const librats::PeerId& from, librats::ByteView payload) {
    session_for(from)->handle(payload);
}

void SyncService::shutdown() {
    std::vector<std::shared_ptr<SyncSession>> sessions;
    {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        for (auto& [_, s] : sessions_) sessions.push_back(s);
        sessions_.clear();
    }
    for (auto& s : sessions) s->stop();
}

} // namespace rasync
