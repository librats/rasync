#include "net/sync_service.h"

#include "net/sync_session.h"

#include "core/state_dir.h"
#include "librats/util/fs.h"

#include <chrono>
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

/// Fill in the parts of a config the caller left to us, so the tag can be derived
/// in the member initialiser list (which runs before the constructor body).
SyncConfig with_defaults(SyncConfig config) {
    // Default the state out of the synced tree: the tree is the user's, and
    // anything rasync leaves there is clutter it has to clean up later.
    if (config.data_dir.empty()) config.data_dir = state_dir_for(config.root);
    if (config.folder.empty())   config.folder   = default_folder_name(config.root);
    return config;
}

} // namespace

std::string default_folder_name(const std::string& root) {
    std::filesystem::path p(root);
    std::string leaf = p.filename().string();
    // A trailing separator ("/srv/data/") puts the leaf in the parent slot.
    if (leaf.empty()) leaf = p.parent_path().filename().string();
    // Only a filesystem root ("/", "C:\") gets this far with nothing to name it.
    return leaf.empty() ? std::string("rasync") : leaf;
}

SyncService::SyncService(librats::Node& node, SyncConfig config, SyncEvents events,
                         SendGate* gate)
    : node_(node),
      gate_(gate),
      config_(with_defaults(std::move(config))),
      events_(std::move(events)),
      tag_(proto::folder_tag(config_.folder)) {}

SyncService::~SyncService() { shutdown(); }

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

bool SyncService::send(const librats::PeerId& to, const Bytes& msg) {
    // The answer matters. It is librats saying whether its queue for this peer
    // still has room, and it is the only warning before the peer is dropped as a
    // slow consumer — see net/send_gate.h. Background senders act on it; the
    // reactor-thread callers (acks, NotFound, Bye) cannot wait and ignore it, but
    // what they send is tiny and bounded by what they are answering.
    return node_.send(to, proto::kChannel, librats::ByteView(msg));
}

bool SyncService::wait_writable(const librats::PeerId& to,
                                const std::function<bool()>& keep_waiting) const {
    if (!gate_) return true;
    return gate_->wait_writable(
        to, keep_waiting, std::chrono::milliseconds(config_.send_stall_timeout_ms));
}

void SyncService::wake_senders() const {
    if (gate_) gate_->wake_all();
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

void SyncService::send_hello(const librats::PeerId& to) {
    auto w = proto::message(tag_, proto::Op::Hello);
    w.u8(proto::kVersion);
    w.u8(static_cast<uint8_t>(config_.mode));
    w.u8(static_cast<uint8_t>(config_.conflict));
    w.str16(config_.folder);
    send(to, w.buffer());
}

void SyncService::peer_connected(const librats::PeerId& id) {
    if (stopped_) return;
    if (!peer_allowed(id)) {
        // Reported once, here, rather than per dropped message: the connection
        // itself is librats' to keep or close, and it will sit there idle. What
        // matters is that we never name this folder to it, never describe the
        // tree, and never serve a request.
        log(2, "peer " + id.to_hex() + " is not in the allow-list — not offering '" +
               config_.folder + "'");
        return;
    }
    // Announce the folder, but create nothing. A session owns two threads and a
    // transfer's worth of state; spending that on every (folder, peer) pair would
    // cost most of it on pairs that share no folder at all. The peer answers a
    // Hello it recognises, and *that* is what brings a session into being — so
    // the cost tracks folders actually shared, not folders configured.
    send_hello(id);
}

void SyncService::peer_disconnected(const librats::PeerId& id) {
    std::shared_ptr<SyncSession> session;
    {
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        auto it = sessions_.find(id);
        if (it != sessions_.end()) { session = it->second; sessions_.erase(it); }
    }
    if (session) session->stop();
}

void SyncService::handle_message(const librats::PeerId& from, proto::Op op, BinaryReader& body) {
    // Dropped silently: a disallowed peer is free to keep talking, and one log
    // line per message it sends would be its own denial of service.
    if (!peer_allowed(from)) return;
    if (auto session = session_for(from)) session->handle(op, body);
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
