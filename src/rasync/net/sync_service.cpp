#include "net/sync_service.h"

#include "net/sync_session.h"

#include "core/state_dir.h"
#include "util/fs.h"

#include <filesystem>
#include <utility>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace rasync {
namespace {

/// A dot-prefixed name is already hidden on Unix; Windows needs to be told.
/// Best-effort — the temp directory works either way.
void hide_from_listings(const std::string& path) {
#ifdef _WIN32
    SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_HIDDEN);
#else
    (void)path;
#endif
}

} // namespace

SyncService::SyncService(librats::Node& node, SyncConfig config, SyncEvents events)
    : node_(node), config_(std::move(config)), events_(std::move(events)) {
    // Default the state out of the synced tree: the tree is the user's, and
    // anything rasync leaves there is clutter it has to clean up later.
    if (config_.data_dir.empty())
        config_.data_dir = state_dir_for(config_.root);
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
        manifest_version_.fetch_add(1);
        log(0, "loaded baseline with " + std::to_string(base_.size()) + " entries");
    }
}

void SyncService::clean_temp_dir() {
    std::error_code ec;
    auto removed = std::filesystem::remove_all(temp_dir(), ec);
    if (!ec && removed > 1)  // >1 = the directory itself plus at least one leftover
        log(0, "cleared " + std::to_string(removed - 1) + " stale temp file(s)");
}

std::string SyncService::ensure_temp_dir() const {
    std::string dir = temp_dir();
    if (!librats::directory_exists(dir.c_str())) {
        librats::create_directories(dir.c_str());
        hide_from_listings(dir);
    }
    return dir;
}

void SyncService::prune_temp_dir() const {
    // Non-recursive on purpose: this runs whenever *a* session goes idle, while
    // another may still have a transfer in flight. The remove then simply fails
    // (directory not empty) and the tree is cleaned by whoever finishes last.
    std::error_code ec;
    std::filesystem::remove(temp_dir(), ec);
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
    manifest_version_.fetch_add(1);
    librats::create_directories(config_.data_dir.c_str());
    m.save(librats::combine_paths(config_.data_dir, "baseline.man"));
}

void SyncService::note_local_set(const std::string& path, const FileMeta& meta) {
    std::lock_guard<std::mutex> lk(manifest_mutex_);
    local_.set(path, meta);
    manifest_version_.fetch_add(1);
}

void SyncService::note_local_removed(const std::string& path) {
    std::lock_guard<std::mutex> lk(manifest_mutex_);
    local_.remove(path);
    manifest_version_.fetch_add(1);
}

void SyncService::note_advert_sent(size_t bytes) {
    advert_messages_.fetch_add(1);
    advert_bytes_.fetch_add(bytes);
}

void SyncService::set_local_manifest(Manifest m) {
    {
        std::lock_guard<std::mutex> lk(manifest_mutex_);
        local_ = std::move(m);
        manifest_version_.fetch_add(1);
    }
    notify_sessions();
}

void SyncService::apply_local_patch(const ManifestPatch& patch) {
    if (patch.empty()) return;
    {
        std::lock_guard<std::mutex> lk(manifest_mutex_);
        local_.apply(patch);
        manifest_version_.fetch_add(1);
    }
    notify_sessions();
}

void SyncService::notify_sessions() {
    // Never call into a session while holding manifest_mutex_: a session takes
    // its own lock first and this one second, so the reverse order would deadlock.
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
        // After shutdown, a message still in flight on the reactor must not
        // resurrect a session: it would re-create the temp directory we just
        // removed, and write into a tree the caller believes we have let go of.
        if (stopped_) return nullptr;
        auto it = sessions_.find(id);
        if (it != sessions_.end()) return it->second;
        session = std::make_shared<SyncSession>(*this, id);
        sessions_.emplace(id, session);
        created = true;
    }
    if (created) session->start();
    return session;
}

bool SyncService::peer_allowed(const librats::PeerId& id) const {
    return config_.allowed_peers.empty() || config_.allowed_peers.count(id) != 0;
}

void SyncService::on_peer_up(const librats::PeerId& id) {
    if (stopped_) return;
    if (!peer_allowed(id)) {
        // Reported once, here, rather than per dropped message: the connection
        // itself is librats' to keep or close, and it will sit there idle. What
        // matters is that no session exists, so nothing of the tree is ever
        // described or served to it.
        log(2, "peer " + id.to_hex() + " is not in the allow-list — ignoring it");
        return;
    }
    session_for(id);
    if (events_.peer_up) events_.peer_up(id);
}

void SyncService::on_peer_down(const librats::PeerId& id) {
    // Symmetric with on_peer_up: a peer we never counted as up must not be
    // reported down, or the UI's peer count drifts negative.
    if (!peer_allowed(id)) return;
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
    // Dropped silently: a disallowed peer is free to keep talking, and one log
    // line per message it sends would be its own denial of service.
    if (!peer_allowed(from)) return;
    if (auto session = session_for(from)) session->handle(payload);
}

void SyncService::shutdown() {
    std::vector<std::shared_ptr<SyncSession>> sessions;
    {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        if (stopped_) return;
        stopped_ = true;
        for (auto& [_, s] : sessions_) sessions.push_back(s);
        sessions_.clear();
    }
    for (auto& s : sessions) s->stop();
    // Every session has joined its threads, so nothing can be mid-transfer:
    // whatever is left in the temp directory is scrap. Leave the synced tree
    // exactly as the user's own files found it.
    clean_temp_dir();
}

} // namespace rasync
