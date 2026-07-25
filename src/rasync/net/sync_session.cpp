#include "net/sync_session.h"

#include "net/sync_service.h"

#include "core/diff.h"
#include "core/hash.h"
#include "core/path.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <sys/utime.h>
using utimbuf_t = struct _utimbuf;
#define RASYNC_UTIME _utime
#else
#include <utime.h>
using utimbuf_t = struct utimbuf;
#define RASYNC_UTIME utime
#endif

namespace fs = std::filesystem;

namespace rasync {
namespace {

constexpr uint64_t kAckInterval = 256 * 1024;  // receiver acks at least this often

// Drop the entries of a peer-supplied patch that we must never act on, and report
// how many went. A patch names files we would create, so a path that could escape
// the sync root is discarded here — before it reaches `remote_`, is planned as a
// pull, and then holds one of the few pull-window slots for the rest of the
// session waiting on a file we would refuse to write anyway. Filtering here and
// not in the codec is deliberate: the codec also decodes our own baseline off
// disk, and one unrepresentable path should cost that path, not the whole tree.
// Removals need no filtering — they can only erase a path that was accepted.
size_t sanitize(ManifestPatch& patch) {
    const size_t before = patch.set.size();
    patch.set.erase(std::remove_if(patch.set.begin(), patch.set.end(),
                                   [](const std::pair<std::string, FileMeta>& e) {
                                       return !is_safe_relpath(e.first);
                                   }),
                    patch.set.end());
    return before - patch.set.size();
}

// Name a conflict policy that arrived as a raw byte. A peer is free to send a
// value that is no policy at all, and the diagnostic has to stay readable when it
// does — so this never casts an unknown byte into the enum.
std::string describe_conflict(uint8_t raw) {
    switch (static_cast<ConflictPolicy>(raw)) {
        case ConflictPolicy::Newer:
        case ConflictPolicy::Larger:
        case ConflictPolicy::PreferLocal:
        case ConflictPolicy::PreferRemote:
            return conflict_policy_name(static_cast<ConflictPolicy>(raw));
    }
    return "unknown policy " + std::to_string(raw);
}

void set_mtime(const std::string& path, int64_t mtime) {
    utimbuf_t t{};
    t.actime = static_cast<decltype(t.actime)>(mtime);
    t.modtime = static_cast<decltype(t.modtime)>(mtime);
    RASYNC_UTIME(path.c_str(), &t);
}

// Unlink `path`, tolerating a reader that happens to hold it open. Windows keeps
// a file undeletable while any handle is open (fopen shares read and write, never
// delete), and the likeliest such handle is a scanner hashing the very file we are
// dropping — a window measured in microseconds. A few short retries clear that;
// anything longer is a real lock, and the caller must not pretend the file is gone.
bool remove_file(const std::string& path) {
    constexpr int kRetries = 3;
    for (int attempt = 0;; ++attempt) {
        std::error_code ec;
        if (fs::remove(path, ec) || !fs::exists(path)) return true;
        if (attempt == kRetries) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

// Drop the directories a removed file left behind, innermost first, stopping at
// the sync root. Transfers create parent directories on the way in, so without
// this a deleted subtree survives as a skeleton of empty folders that nothing
// will ever clean up — and the two trees stop looking alike to their owners even
// though every tracked file agrees.
//
// `fs::remove` refuses a non-empty directory, so a parent that still holds
// anything — an ignored file, a sibling, a file that landed a moment ago — is
// left alone by construction: there is no emptiness check here to race against.
// Only directories rasync itself emptied are considered; one the user emptied by
// hand is not rasync's to remove. `rel_path` comes from our own scanner (nothing
// else can reach `delete_local`), so joining it onto the root stays inside it.
void prune_empty_dirs(const std::string& root, const std::string& rel_path) {
    const fs::path root_path(root);
    for (fs::path dir = fs::path(rel_path).parent_path(); !dir.empty();
         dir = dir.parent_path()) {
        std::error_code ec;
        if (!fs::remove(root_path / dir, ec) || ec) return;  // not empty, or gone
    }
}

// Move `from` onto `to`, replacing an existing `to`, creating parent dirs.
bool move_replace(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::path dst(to);
    if (dst.has_parent_path()) fs::create_directories(dst.parent_path(), ec);
    fs::rename(from, dst, ec);
    if (!ec) return true;
    // Some platforms won't rename over an existing file — drop it and retry.
    // The old copy can be held open just as briefly, hence the same tolerance.
    remove_file(to);
    ec.clear();
    fs::rename(from, dst, ec);
    return !ec;
}

} // namespace

// ── Incoming ─────────────────────────────────────────────────────────────────

SyncSession::Incoming::~Incoming() {
    out.close();
    base.close();
    if (!temp_path.empty()) {
        std::error_code ec;
        fs::remove(temp_path, ec);
    }
}

// ── lifecycle ────────────────────────────────────────────────────────────────

SyncSession::SyncSession(SyncService& service, librats::PeerId peer)
    : service_(service), peer_(std::move(peer)) {
    // Temp files are named per (peer, transfer). xid alone is not unique: it
    // restarts at 1 in every session, so two peers syncing the same root would
    // pick the same name and interleave their writes into one file. 16 hex chars
    // of the peer's public key make that collision infeasible to hit or to forge.
    temp_tag_ = peer_.to_hex().substr(0, 16);
}

SyncSession::~SyncSession() { stop(); }

void SyncSession::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    sender_ = std::thread([this] { sender_loop(); });
    requester_ = std::thread([this] { requester_loop(); });
    send_hello();
}

void SyncSession::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_cv_.notify_all();
        pull_cv_.notify_all();
    }
    { std::lock_guard<std::mutex> lk(send_mtx_); send_cv_.notify_all(); }
    if (sender_.joinable()) sender_.join();
    if (requester_.joinable()) requester_.join();
    std::lock_guard<std::mutex> lk(mtx_);
    incoming_.clear();  // Incoming dtor drops temp files
    pull_queue_.clear();
}

// ── inbound dispatch (reactor thread) ────────────────────────────────────────

void SyncSession::handle(librats::ByteView payload) {
    if (incompatible_.load()) return;  // nothing this peer says can be acted on
    BinaryReader r(payload.data(), payload.size());
    auto op = static_cast<proto::Op>(r.u8());
    if (!r.ok()) return;
    switch (op) {
        case proto::Op::Hello:          handle_hello(r); break;
        case proto::Op::ManifestUpdate: handle_manifest_update(r); break;
        case proto::Op::Request:      handle_request(r); break;
        case proto::Op::FileStart:    handle_file_start(r); break;
        case proto::Op::FileLiteral:  handle_file_literal(r); break;
        case proto::Op::FileCopy:     handle_file_copy(r); break;
        case proto::Op::FileEnd:      handle_file_end(r); break;
        case proto::Op::FileAck:      handle_file_ack(r); break;
        case proto::Op::NotFound:     handle_not_found(r); break;
        case proto::Op::Changed:      advertise(); break;
        case proto::Op::RequestsDone: maybe_synced(); break;
        case proto::Op::Bye:          break;
    }
}

void SyncSession::send_msg(BinaryWriter& w) { service_.send(peer_, w.buffer()); }

void SyncSession::send_hello() {
    const auto& cfg = service_.config();
    auto w = proto::message(proto::Op::Hello);
    w.u8(proto::kVersion);
    w.u8(static_cast<uint8_t>(cfg.mode));
    w.u8(static_cast<uint8_t>(cfg.conflict));
    w.u64(service_.base_manifest().generation());
    w.str16(fs::path(cfg.root).filename().string());
    send_msg(w);
}

void SyncSession::handle_hello(BinaryReader& r) {
    uint8_t version = r.u8();
    auto    peer_mode = static_cast<SyncMode>(r.u8());
    uint8_t peer_conflict = r.u8();
    r.u64();         // base generation (informational)
    r.str16();       // name
    if (!r.ok()) return;

    // Go inert and say so once. A peer we cannot converge with is worse than no
    // peer: the two sides keep talking and the sync silently never finishes, which
    // is indistinguishable from a stall. Bye is understood by every version, so the
    // peer learns of it too.
    auto refuse = [&](const std::string& why) {
        service_.log(2, why + " — not syncing with this peer");
        incompatible_.store(true);
        auto w = proto::message(proto::Op::Bye);
        send_msg(w);
    };

    if (version != proto::kVersion) {
        // The shape of a tree description changed in v2, so a mismatched peer's
        // updates decode as garbage: both sides would log a malformed message per
        // advertisement and never converge.
        refuse("peer speaks protocol v" + std::to_string(version) +
               ", we speak v" + std::to_string(proto::kVersion));
        return;
    }

    if (peer_mode != service_.config().mode) {
        service_.log(2, "peer sync mode differs from ours — results may be surprising");
    } else if (service_.config().mode == SyncMode::TwoWay) {
        // Reconciliation only converges because the two plans are complements, and
        // that holds only if the peer resolves conflicts with the complementary
        // policy. Mismatched policies do not merely pick a surprising winner: both
        // sides can decide they won (nothing is ever transferred, the sync stalls
        // forever) or both decide they lost (the two versions swap places on every
        // round, forever). Neither announces itself, so catch it at the handshake.
        // Mirror mode ignores the policy entirely, hence the guard.
        const ConflictPolicy ours   = service_.config().conflict;
        const ConflictPolicy wanted = complement(ours);
        // Compared as the raw byte on purpose: a value that is no policy at all
        // fails this too, rather than being cast into one.
        if (peer_conflict != static_cast<uint8_t>(wanted)) {
            refuse(std::string("peer resolves conflicts by '") +
                   describe_conflict(peer_conflict) + "', which cannot pair with our '" +
                   conflict_policy_name(ours) + "' (run the peer with --conflict " +
                   conflict_policy_name(wanted) + ")");
            return;
        }
    }
    // Bootstrap the exchange: describe our current tree.
    advertise();
}

void SyncSession::handle_manifest_update(BinaryReader& r) {
    uint8_t flags = r.u8();
    auto patch = ManifestPatch::decode(r);
    if (!patch || !r.ok()) { service_.log(2, "dropping malformed manifest update"); return; }
    if (size_t dropped = sanitize(*patch))
        service_.log(2, "ignoring " + std::to_string(dropped) +
                        " manifest entry/entries with an unsafe path");

    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (flags & proto::kUpdateReset) {
            staging_ = Manifest{};      // a fresh description of the peer's whole tree
            staging_active_ = true;
        }
        if (staging_active_) {
            staging_.apply(*patch);
            // A partially applied tree must never reach reconcile: the paths that
            // haven't arrived yet would look exactly like paths the peer deleted.
            if (flags & proto::kUpdateMore) return;
            remote_ = std::move(staging_);
            staging_ = Manifest{};
            staging_active_ = false;
        } else if (flags & proto::kUpdateMore) {
            staging_ = remote_;         // multi-chunk increment: assemble aside
            staging_active_ = true;
            staging_.apply(*patch);
            return;
        } else {
            remote_.apply(*patch);      // the common case: one small chunk, applied in place
        }
        have_remote_ = true;
        ++remote_version_;
    }
    reconcile_and_act();
}

// ── advertising + reconciliation ─────────────────────────────────────────────

void SyncSession::advertise() {
    std::lock_guard<std::mutex> lk(mtx_);
    advertise_locked();
}

void SyncSession::advertise_locked() {
    // Everything about an advertisement happens under mtx_: the diff is taken
    // against `last_sent_` and immediately becomes the new `last_sent_`, so two
    // threads advertising at once cannot emit patches that overlap or invert.
    //
    // Most calls have nothing to say (every convergence check asks). Settle that
    // against an O(1) counter before copying a manifest to diff against.
    const uint64_t version = service_.manifest_version();
    if (last_sent_valid_ && version == last_sent_version_) {
        advertise_pending_ = false;
        return;
    }
    last_sent_version_ = version;

    Manifest local = service_.local_manifest();

    ManifestPatch patch;
    const bool reset = !last_sent_valid_;
    if (reset) {
        patch.set.reserve(local.size());
        for (const auto& [path, meta] : local.entries()) patch.set.emplace_back(path, meta);
    } else {
        patch = diff_manifests(last_sent_, local);
        if (patch.empty()) { advertise_pending_ = false; return; }
    }

    send_update_locked(patch, reset);
    last_sent_ = std::move(local);
    last_sent_valid_ = true;
    advertise_pending_ = false;
}

void SyncSession::send_update_locked(const ManifestPatch& patch, bool reset) {
    ManifestPatch chunk;
    size_t est = 0;
    bool   first = true;

    auto flush = [&](bool more) {
        uint8_t flags = 0;
        if (first && reset) flags |= proto::kUpdateReset;
        if (more)           flags |= proto::kUpdateMore;
        auto w = proto::message(proto::Op::ManifestUpdate);
        w.u8(flags);
        chunk.encode(w);
        service_.note_advert_sent(w.size());
        send_msg(w);
        first = false;
        chunk = ManifestPatch{};
        est = 0;
    };

    // Entry sizes vary (paths are length-prefixed), so pace by estimated bytes
    // rather than by count: a few very long paths must not build a huge message.
    constexpr size_t kFixedEntryBytes = 54;  // str16 header + size + mtime + mode + hash
    const size_t cap = service_.config().max_update_bytes;
    for (const auto& e : patch.set) {
        chunk.set.push_back(e);
        est += kFixedEntryBytes + e.first.size();
        if (est >= cap) flush(/*more=*/true);
    }
    for (const auto& path : patch.removed) {
        chunk.removed.push_back(path);
        est += 2 + path.size();
        if (est >= cap) flush(/*more=*/true);
    }
    // Always emit a final chunk, even an empty one: it is what commits the update
    // (and what tells a peer with a reset flag that our tree is simply empty).
    flush(/*more=*/false);
}

void SyncSession::local_changed() {
    if (incompatible_.load()) return;  // describing our tree to it would be noise
    advertise();
    reconcile_and_act();  // our deletions / conflict-winning may create local work too
}

void SyncSession::reconcile_and_act() {
    Manifest local = service_.local_manifest();
    Manifest base  = service_.base_manifest();
    Manifest remote;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!have_remote_) return;
        remote = remote_;
    }

    const auto& cfg = service_.config();
    ReconcileOptions o;
    o.mode = cfg.mode;
    o.conflict = cfg.conflict;
    o.source = cfg.source;
    o.propagate_deletes = cfg.propagate_deletes;

    SyncPlan plan = reconcile(base, local, remote, o);
    if (service_.events().round) service_.events().round(peer_, plan);

    // 1. Apply our local deletions.
    bool mutated = false;
    for (const auto& path : plan.delete_local) {
        if (!remove_file(service_.abs_path(path))) {
            // Still there: say nothing to the manifest. The path stays local, so
            // the next round reconciles it again instead of resurrecting it.
            service_.log(2, "cannot delete " + path + " (in use) — will retry");
            continue;
        }
        prune_empty_dirs(service_.config().root, path);
        service_.note_local_removed(path);
        mutated = true;
        service_.log(0, "deleted " + path);
    }

    // 2. Reserve the files we need. The requester thread paces the actual
    //    Requests: a plan can name the whole tree, and every request carries a
    //    signature the peer has to hold until it serves us.
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& path : plan.pull)
            if (pulling_.insert(path).second) pull_queue_.push_back(path);
    }
    pull_cv_.notify_all();

    if (mutated) advertise();  // tell the peer our tree shrank
    maybe_synced();
}

// ── pulling: windowed requests (requester thread) ────────────────────────────

bool SyncSession::may_request_locked() const {
    if (pull_queue_.empty()) return false;
    // Every queued path is also in `pulling_`, so the difference is what we have
    // already asked for and not yet received.
    const size_t in_flight = pulling_.size() > pull_queue_.size()
                                 ? pulling_.size() - pull_queue_.size() : 0;
    return in_flight < service_.config().max_pending_pulls;
}

void SyncSession::requester_loop() {
    for (;;) {
        std::string path;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            pull_cv_.wait(lk, [this] { return !running_.load() || may_request_locked(); });
            if (!running_.load()) return;
            path = std::move(pull_queue_.front());
            pull_queue_.pop_front();
        }
        send_request(path);  // reads our copy of the file — never on the reactor
    }
}

void SyncSession::send_request(const std::string& rel_path) {
    const auto& cfg = service_.config();
    auto w = proto::message(proto::Op::Request);
    w.str16(rel_path);

    // A signature lets the peer answer with a delta. It is only worth building if
    // we actually hold a copy — the file size doubles as the existence check.
    bool sent_sig = false;
    if (cfg.use_delta) {
        std::string abs = service_.abs_path(rel_path);
        int64_t have = librats::get_file_size(abs.c_str());
        if (have >= 0 && static_cast<uint64_t>(have) <= cfg.delta_max_bytes) {
            if (auto sig = signature_of_file(abs, cfg.block_size)) {
                w.u8(1);
                sig->encode(w);
                sent_sig = true;
            }
        }
    }
    if (!sent_sig) w.u8(0);
    send_msg(w);
}

void SyncSession::release_pull(const std::string& rel_path) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pulling_.erase(rel_path);
    }
    pull_cv_.notify_one();  // a window slot just freed up
}

void SyncSession::abandon_pull(const std::string& rel_path) {
    release_pull(rel_path);
    // This may have been the last thing the session was waiting on, and nothing
    // else will look: a giving-up path produces no further protocol traffic.
    maybe_synced();
}

void SyncSession::maybe_synced() {
    Manifest remote;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!pulling_.empty() || !incoming_.empty() || outstanding_serves_.load() != 0) {
            // Busy. Whatever we concluded last time is stale by the time the work
            // lands, so force the next idle moment to look again.
            last_eval_valid_ = false;
            return;
        }
        // Inbound work has drained: this is the moment to tell the peer what we
        // picked up, in one update rather than one per file.
        if (advertise_pending_) advertise_locked();
        if (!have_remote_) return;

        const uint64_t local_ver = service_.manifest_version();
        if (last_eval_valid_ && last_eval_local_ver_ == local_ver &&
            last_eval_remote_ver_ == remote_version_)
            return;  // same inputs as last time — the answer cannot have changed
        last_eval_valid_ = true;
        last_eval_local_ver_ = local_ver;
        last_eval_remote_ver_ = remote_version_;
        remote = remote_;
    }

    Manifest local = service_.local_manifest();
    Manifest base  = service_.base_manifest();
    const auto& cfg = service_.config();
    ReconcileOptions o;
    o.mode = cfg.mode; o.conflict = cfg.conflict;
    o.source = cfg.source; o.propagate_deletes = cfg.propagate_deletes;

    if (!reconcile(base, local, remote, o).empty()) return;  // still work outstanding

    Hash fp = local.fingerprint();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (last_synced_valid_ && fp == last_synced_fp_) return;  // already announced
        last_synced_fp_ = fp;
        last_synced_valid_ = true;
    }
    if (cfg.mode == SyncMode::TwoWay) service_.set_base_manifest(local);
    if (service_.events().synced) service_.events().synced(peer_, local.size(), local.total_bytes());
}

// ── serving the peer's requests (sender thread) ──────────────────────────────

void SyncSession::handle_request(BinaryReader& r) {
    Serve job;
    job.rel_path = r.str16();
    if (!r.ok()) return;  // no path to answer to

    // Every request we decline still gets an answer. A silently dropped one leaves
    // the asking peer holding one of its few pull slots for the rest of the
    // session, waiting on a reply that is never coming — a handful of those and it
    // stops requesting anything at all.
    auto decline = [&](const std::string& why) {
        service_.log(2, "declining request for " + job.rel_path + ": " + why);
        auto w = proto::message(proto::Op::NotFound);
        w.str16(job.rel_path);
        send_msg(w);
    };

    if (!is_safe_relpath(job.rel_path)) { decline("unsafe path"); return; }
    job.has_sig = r.u8() != 0;
    if (job.has_sig) {
        auto sig = Signature::decode(r);
        if (!sig) { decline("malformed signature"); return; }
        job.sig = std::move(*sig);
    }
    if (!r.ok()) { decline("truncated request"); return; }
    job.xid = next_xid();

    std::lock_guard<std::mutex> lk(mtx_);
    outstanding_serves_.fetch_add(1);
    serve_q_.push_back(std::move(job));
    queue_cv_.notify_one();
}

void SyncSession::sender_loop() {
    for (;;) {
        Serve job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            queue_cv_.wait(lk, [this] { return !running_.load() || !serve_q_.empty(); });
            if (!running_.load() && serve_q_.empty()) return;
            if (serve_q_.empty()) continue;
            job = std::move(serve_q_.front());
            serve_q_.pop_front();
        }
        serve_one(job);
        outstanding_serves_.fetch_sub(1);
        maybe_synced();
    }
}

void SyncSession::serve_one(const Serve& job) {
    std::string abs = service_.abs_path(job.rel_path);
    if (!librats::file_exists(abs)) {
        auto w = proto::message(proto::Op::NotFound);
        w.str16(job.rel_path);
        send_msg(w);
        return;
    }
    int64_t fsize = librats::get_file_size(abs.c_str());
    if (fsize < 0) fsize = 0;
    uint64_t size = static_cast<uint64_t>(fsize);
    int64_t mtime = static_cast<int64_t>(librats::get_file_modified_time(abs));
    uint32_t mode = 0;
    if (const FileMeta* m = service_.local_manifest().find(job.rel_path)) mode = m->mode;

    {
        std::lock_guard<std::mutex> lk(send_mtx_);
        cur_xid_ = job.xid; cur_sent_ = 0; cur_acked_ = 0;
    }

    const auto& cfg = service_.config();
    if (job.has_sig && cfg.use_delta && size <= cfg.delta_max_bytes)
        serve_delta(job, abs, size, mtime, mode);
    else
        serve_whole(job, abs, size, mtime, mode);
}

void SyncSession::serve_whole(const Serve& job, const std::string& abs,
                              uint64_t size, int64_t mtime, uint32_t mode) {
    librats::FileStream in;
    if (!in.open_read(abs.c_str())) {
        auto w = proto::message(proto::Op::NotFound);
        w.str16(job.rel_path);
        send_msg(w);
        return;
    }
    auto start = proto::message(proto::Op::FileStart);
    start.u64(job.xid); start.str16(job.rel_path);
    start.u64(size); start.i64(mtime); start.u32(mode); start.u8(0);
    send_msg(start);

    sha256_context_t hash; sha256_reset(&hash);
    const uint32_t chunk = service_.config().chunk_size;
    std::vector<uint8_t> buf(chunk);
    uint64_t sent = 0;
    for (;;) {
        size_t n = in.read(buf.data(), chunk);
        if (n == 0) break;
        sha256_update(&hash, buf.data(), n);
        auto w = proto::message(proto::Op::FileLiteral);
        w.u64(job.xid); w.blob32(buf.data(), n);
        send_msg(w);
        note_sent(job.xid, n);
        window_wait(job.xid);
        sent += n;
        if (!running_.load()) return;
        if (n < chunk) break;
    }
    Hash digest{}; sha256_finish(&hash, digest.data());
    auto end = proto::message(proto::Op::FileEnd);
    end.u64(job.xid); end.bytes(digest);
    send_msg(end);

    if (service_.events().file_done)
        service_.events().file_done({TransferInfo::Send, job.rel_path, sent, size, false, sent});
}

void SyncSession::serve_delta(const Serve& job, const std::string& abs,
                              uint64_t size, int64_t mtime, uint32_t mode) {
    // Read the source into memory (bounded by delta_max_bytes) to run the scan.
    // Scan it in place: copying it into a vector first would double the peak —
    // 512 MiB for a file at the delta ceiling, for no gain.
    size_t sz = 0;
    void* raw = librats::read_file_binary(abs.c_str(), &sz);
    if (!raw) { serve_whole(job, abs, size, mtime, mode); return; }
    struct BufferGuard {
        void* p;
        ~BufferGuard() { if (p) librats::free_file_buffer(p); }
    } guard{raw};
    const auto* data = static_cast<const uint8_t*>(raw);

    Delta delta = compute_delta(job.sig, data, sz);
    Hash digest = sha256(data, sz);

    auto start = proto::message(proto::Op::FileStart);
    start.u64(job.xid); start.str16(job.rel_path);
    start.u64(sz); start.i64(mtime); start.u32(mode); start.u8(1);
    send_msg(start);

    const uint32_t chunk = service_.config().chunk_size;
    uint64_t on_wire = 0;
    for (const auto& op : delta.ops) {
        if (!running_.load()) return;
        if (op.copy) {
            auto w = proto::message(proto::Op::FileCopy);
            w.u64(job.xid); w.u64(op.offset); w.u32(op.len);
            send_msg(w);
        } else {
            // Chunk large literal runs so one message never dominates the window.
            for (size_t off = 0; off < op.literal.size(); off += chunk) {
                size_t n = std::min<size_t>(chunk, op.literal.size() - off);
                auto w = proto::message(proto::Op::FileLiteral);
                w.u64(job.xid); w.blob32(op.literal.data() + off, n);
                send_msg(w);
                note_sent(job.xid, n);
                window_wait(job.xid);
                on_wire += n;
            }
        }
    }
    auto end = proto::message(proto::Op::FileEnd);
    end.u64(job.xid); end.bytes(digest);
    send_msg(end);

    service_.log(0, "sent " + job.rel_path + " as delta (" + std::to_string(on_wire) +
                    " of " + std::to_string(sz) + " bytes on wire)");
    if (service_.events().file_done)
        service_.events().file_done({TransferInfo::Send, job.rel_path, sz, sz, true, on_wire});
}

void SyncSession::note_sent(uint64_t xid, uint64_t bytes) {
    std::lock_guard<std::mutex> lk(send_mtx_);
    if (cur_xid_ == xid) cur_sent_ += bytes;
}

void SyncSession::window_wait(uint64_t xid) {
    const uint64_t window = service_.config().window_bytes;
    std::unique_lock<std::mutex> lk(send_mtx_);
    send_cv_.wait(lk, [&] {
        if (!running_.load() || cur_xid_ != xid) return true;
        // `cur_acked_ >= cur_sent_` means fully drained. Testing it explicitly keeps
        // an ack that (benignly or maliciously) runs ahead of our own counter from
        // underflowing the subtraction into a window that can never be satisfied.
        return cur_acked_ >= cur_sent_ || (cur_sent_ - cur_acked_) <= window;
    });
}

void SyncSession::handle_file_ack(BinaryReader& r) {
    uint64_t xid = r.u64();
    uint64_t bytes = r.u64();
    if (!r.ok()) return;
    {
        std::lock_guard<std::mutex> lk(send_mtx_);
        if (cur_xid_ == xid && bytes > cur_acked_) cur_acked_ = bytes;
    }
    send_cv_.notify_all();
}

// ── receiving files (reactor thread) ─────────────────────────────────────────

void SyncSession::handle_file_start(BinaryReader& r) {
    auto in = std::make_shared<Incoming>();
    in->xid = r.u64();
    in->rel_path = r.str16();
    in->size = r.u64();
    in->mtime = r.i64();
    in->mode = r.u32();
    in->is_delta = r.u8() != 0;
    if (!r.ok()) return;
    if (!is_safe_relpath(in->rel_path)) {
        // Unsafe paths are filtered out of the peer's manifest, so we never asked
        // for this one: the peer is answering with something other than what was
        // requested. Hand the reservation back regardless — dropping the header in
        // silence would strand a pull-window slot for the rest of the session.
        service_.log(2, "refusing unsafe path from peer: " + in->rel_path);
        abandon_pull(in->rel_path);
        return;
    }

    in->final_path = service_.abs_path(in->rel_path);
    std::string tmpdir = service_.temp_dir();
    librats::create_directories(tmpdir.c_str());
    in->temp_path = librats::combine_paths(tmpdir, temp_tag_ + "-" + std::to_string(in->xid) + ".part");

    // The temp file must start empty. FileStream::open_write keeps existing
    // content (it writes positioned, never truncates), and our SHA-256 covers the
    // bytes we write — not the file — so a leftover tail from an earlier, larger
    // transfer would survive verification and be renamed into place as corruption.
    std::error_code ec;
    fs::remove(in->temp_path, ec);
    if (ec) {
        service_.log(2, "cannot clear stale temp file for " + in->rel_path);
        abandon_pull(in->rel_path);
        return;
    }
    if (!in->out.open_write(in->temp_path.c_str())) {
        service_.log(2, "cannot open temp file for " + in->rel_path);
        abandon_pull(in->rel_path);
        return;
    }
    if (in->is_delta) {
        in->base_open = in->base.open_read(in->final_path.c_str());
        if (!in->base_open) { fail_incoming(in, "delta base missing"); return; }
    }
    sha256_reset(&in->hash);
    std::lock_guard<std::mutex> lk(mtx_);
    incoming_[in->xid] = in;
}

void SyncSession::handle_file_literal(BinaryReader& r) {
    uint64_t xid = r.u64();
    Bytes data = r.blob32();
    if (!r.ok()) return;
    std::shared_ptr<Incoming> in;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = incoming_.find(xid);
        if (it == incoming_.end()) return;
        in = it->second;
    }
    if (in->failed) return;
    if (!in->out.write_at(in->written, data.data(), data.size())) { fail_incoming(in, "write error"); return; }
    sha256_update(&in->hash, data.data(), data.size());
    in->written += data.size();
    in->on_wire += data.size();

    // Ack the bytes that actually crossed the wire — never the reconstructed size.
    // The sender's window counts literals only, so acking `written` (which includes
    // COPY bytes it never sent) would run its counter backwards.
    if (in->on_wire - in->last_ack >= kAckInterval) {
        in->last_ack = in->on_wire;
        auto w = proto::message(proto::Op::FileAck);
        w.u64(xid); w.u64(in->on_wire);
        send_msg(w);
    }
    if (service_.events().progress)
        service_.events().progress({TransferInfo::Recv, in->rel_path, in->written, in->size, in->is_delta, in->on_wire});
}

void SyncSession::handle_file_copy(BinaryReader& r) {
    uint64_t xid = r.u64();
    uint64_t off = r.u64();
    uint32_t len = r.u32();
    if (!r.ok()) return;
    std::shared_ptr<Incoming> in;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = incoming_.find(xid);
        if (it == incoming_.end()) return;
        in = it->second;
    }
    if (in->failed || !in->base_open) return;

    std::vector<uint8_t> buf(std::min<uint32_t>(len, 64 * 1024));
    uint32_t left = len;
    uint64_t src = off;
    while (left > 0) {
        size_t want = std::min<size_t>(buf.size(), left);
        if (!in->base.seek(src)) { fail_incoming(in, "base seek"); return; }
        size_t got = in->base.read(buf.data(), want);
        if (got == 0) { fail_incoming(in, "base read"); return; }
        if (!in->out.write_at(in->written, buf.data(), got)) { fail_incoming(in, "write error"); return; }
        sha256_update(&in->hash, buf.data(), got);
        in->written += got; src += got; left -= static_cast<uint32_t>(got);
    }
}

void SyncSession::handle_file_end(BinaryReader& r) {
    uint64_t xid = r.u64();
    Hash expected = r.bytes<32>();
    if (!r.ok()) return;
    std::shared_ptr<Incoming> in;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = incoming_.find(xid);
        if (it == incoming_.end()) return;
        in = it->second;
    }
    if (in->failed) return;

    Hash got{}; sha256_finish(&in->hash, got.data());
    in->result_hash = got;
    if (got != expected) {
        // Integrity failure. If this was a delta, the base may have drifted — fall
        // back to a whole-file pull once before giving up.
        const std::string path = in->rel_path;
        const bool retry = in->is_delta;
        // A retry keeps the reservation: the path must stay in `pulling_` so the
        // round still waits for it and its window slot isn't handed to another pull.
        fail_incoming(in, "hash mismatch", /*keep_reservation=*/retry);
        if (retry) {
            service_.log(2, "delta verify failed for " + path + " — retrying whole-file");
            auto w = proto::message(proto::Op::Request);
            w.str16(path); w.u8(0);
            send_msg(w);
        }
        return;
    }

    finalize_incoming(in);
}

void SyncSession::finalize_incoming(const std::shared_ptr<Incoming>& in) {
    // Ack every wire byte so the sender's window drains cleanly.
    {
        auto w = proto::message(proto::Op::FileAck);
        w.u64(in->xid); w.u64(in->on_wire);
        send_msg(w);
    }
    in->out.close();
    in->base.close();
    in->base_open = false;

    if (!move_replace(in->temp_path, in->final_path)) {
        service_.log(2, "cannot move into place: " + in->rel_path);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            incoming_.erase(in->xid);
        }
        abandon_pull(in->rel_path);
        return;
    }
    in->temp_path.clear();  // moved — nothing for the dtor to clean
    set_mtime(in->final_path, in->mtime);

    FileMeta meta;
    meta.size = in->written;
    meta.mtime = in->mtime;
    meta.mode = in->mode;
    // The running digest covers exactly the bytes we wrote, in order, and FileEnd
    // already checked it against the sender's whole-file hash — re-reading the
    // file here would only confirm what we verified.
    meta.hash = in->result_hash;
    service_.note_local_set(in->rel_path, meta);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        incoming_.erase(in->xid);
        // Our tree grew. Don't describe it to the peer yet: during a bulk pull
        // that would re-describe the tree once per file, which is the quadratic
        // cost incremental updates exist to remove. maybe_synced() ships one
        // update as soon as the inbound queue drains.
        advertise_pending_ = true;
    }
    release_pull(in->rel_path);

    service_.log(0, "received " + in->rel_path);
    if (service_.events().file_done)
        service_.events().file_done({TransferInfo::Recv, in->rel_path, in->written, in->size, in->is_delta, in->on_wire});

    maybe_synced();
}

void SyncSession::fail_incoming(const std::shared_ptr<Incoming>& in, const std::string& why,
                                bool keep_reservation) {
    in->failed = true;
    in->out.close();
    in->base.close();
    service_.log(2, "transfer of " + in->rel_path + " failed: " + why);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        incoming_.erase(in->xid);
    }
    // Hand the path back. A failed transfer that stayed in `pulling_` would hold
    // one of the few window slots for the rest of the session — a handful of them
    // stops the session requesting anything at all — and would keep it from ever
    // looking converged. The one exception is a caller that immediately re-asks
    // for the same path: it needs the reservation to survive, or the round would
    // conclude while the replacement transfer is still on its way.
    if (!keep_reservation) abandon_pull(in->rel_path);
}

void SyncSession::handle_not_found(BinaryReader& r) {
    std::string path = r.str16();
    if (!r.ok()) return;
    service_.log(2, "peer no longer has " + path);
    abandon_pull(path);
}

} // namespace rasync
