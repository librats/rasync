#include "net/sync_router.h"

#include <utility>

namespace rasync {
namespace {

/// Key for the "already told the user about this" set. The peer id is hex and the
/// tag is appended behind a separator that hex cannot contain, so no two pairs
/// can produce one key.
std::string unknown_key(const librats::PeerId& id, uint32_t tag) {
    return id.to_hex() + ":" + std::to_string(tag);
}

} // namespace

SyncRouter::SyncRouter(librats::Node& node, RouterEvents events)
    : node_(node), events_(std::move(events)), gate_(node, proto::kChannel) {}

SyncRouter::~SyncRouter() { shutdown(); }

SyncService* SyncRouter::add_folder(SyncConfig config, SyncEvents events) {
    // Enforced, not merely documented: dispatch reads by_tag_ from reactor threads
    // without a lock, which is only sound because the table stops changing here.
    if (attached_) return nullptr;
    auto svc = std::make_unique<SyncService>(node_, std::move(config), std::move(events), &gate_);
    SyncService* raw = svc.get();
    if (!by_tag_.emplace(raw->tag(), raw).second) return nullptr;
    folders_.push_back(std::move(svc));
    return raw;
}

void SyncRouter::attach() {
    if (attached_) return;
    attached_ = true;
    node_.on(proto::kChannel, [this](const librats::Peer& peer, librats::ByteView payload) {
        on_message(peer.id(), payload);
    });
    node_.on_peer_connected([this](const librats::Peer& peer) {
        if (stopped_.load()) return;
        // Offer every folder before telling the UI, so a "connected" line is never
        // followed by a folder that was silently skipped.
        for (auto& f : folders_) f->peer_connected(peer.id());
        if (events_.peer_up) events_.peer_up(peer.id());
    });
    node_.on_peer_disconnected([this](const librats::PeerId& id) {
        // Before the folders: stopping a session joins its sender thread, and that
        // thread may be parked on the gate waiting for a peer that has just gone.
        // Waking it first turns a join that waits out the poll tick into one that
        // returns at once.
        gate_.wake_all();
        for (auto& f : folders_) f->peer_disconnected(id);
        if (events_.peer_down) events_.peer_down(id);
    });
    // The transport's half of the backpressure contract: send() says "stop", this
    // says "go again". Registered here because it is a node-level handler, like
    // the channel and the peer up/down callbacks — a folder never sees the Node.
    node_.on_peer_writable([this](const librats::Peer&) { gate_.wake_all(); });
}

void SyncRouter::start_folders() {
    for (auto& f : folders_) {
        f->load_baseline();
        f->clean_temp_dir();
    }
}

void SyncRouter::shutdown() {
    if (stopped_.exchange(true)) return;
    // Release every parked sender first, for the same reason as on disconnect:
    // shutdown joins those threads.
    gate_.stop();
    for (auto& f : folders_) f->shutdown();
}

SyncService* SyncRouter::folder_for(uint32_t tag) const {
    auto it = by_tag_.find(tag);
    return it == by_tag_.end() ? nullptr : it->second;
}

SyncService* SyncRouter::folder_named(const std::string& name) const {
    for (const auto& f : folders_)
        if (f->folder() == name) return f.get();
    return nullptr;
}

void SyncRouter::on_message(const librats::PeerId& from, librats::ByteView payload) {
    if (stopped_.load()) return;
    if (payload.size() < proto::kHeaderBytes) return;   // truncated: nothing to route on

    BinaryReader r(payload.data(), payload.size());
    const uint32_t tag = r.u32();
    const auto     op  = static_cast<proto::Op>(r.u8());

    // No lock: by_tag_ is fixed before attach(), and attach() runs before the node
    // starts — so nothing can be writing it while a reactor thread reads it.
    SyncService* folder = folder_for(tag);
    if (!folder) { report_unknown(from, tag, payload); return; }

    folder->handle_message(from, op, r);
}

void SyncRouter::report_unknown(const librats::PeerId& from, uint32_t tag,
                                librats::ByteView payload) {
    // Only a Hello is worth a word: it is the one message that names its folder,
    // and a peer sends exactly one per folder we turn out not to have. Everything
    // else goes quietly, or a peer could make us log for every chunk it sends.
    //
    // An unknown tag means an unknown *folder*, nothing else. A peer speaking a
    // different protocol version still puts the version where we look for it —
    // the folder it names either matches one of ours, and handle_hello refuses it
    // by version, or it does not, and it is reported here like any other.
    const auto op = static_cast<proto::Op>(payload.data()[proto::kHeaderBytes - 1]);
    if (op != proto::Op::Hello) return;

    {
        // Bounded: a peer that has completed the handshake can still send a Hello
        // under any tag it likes, and one remembered string per attempt would let
        // it grow this set without limit. Past the cap we stop remembering — and
        // therefore stop reporting — rather than keep paying for its noise.
        constexpr size_t kMaxRemembered = 256;
        std::lock_guard<std::mutex> lk(unknown_mutex_);
        if (unknown_seen_.size() >= kMaxRemembered) return;
        if (!unknown_seen_.insert(unknown_key(from, tag)).second) return;
    }

    BinaryReader r(payload.data(), payload.size());
    r.u32();                    // tag, already read
    r.u8();                     // opcode
    r.u8();                     // version
    r.u8();                     // mode
    r.u8();                     // conflict
    const std::string name = r.str16();
    if (!r.ok() || name.empty()) return;   // not a Hello we can quote from

    log(2, "peer " + from.short_hex() + " offers folder '" + name +
           "', which is not configured here" +
           (folders_.empty() ? std::string()
                             : " (run both sides with the same folder name — see --name)"));
}

void SyncRouter::log(int level, const std::string& msg) const {
    if (events_.log) events_.log(level, msg);
}

} // namespace rasync
