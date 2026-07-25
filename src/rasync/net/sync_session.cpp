#include "net/sync_session.h"

#include "net/sync_service.h"

#include "core/diff.h"
#include "core/hash.h"

#include <algorithm>
#include <filesystem>
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

// A peer-supplied relative path is safe only if it stays inside the sync root:
// non-empty, relative, no drive letter, no "."/".." component, forward slashes.
bool is_safe_rel(const std::string& p) {
    if (p.empty() || p.front() == '/' || p.front() == '\\') return false;
    if (p.find('\\') != std::string::npos) return false;
    if (p.size() >= 2 && p[1] == ':') return false;  // C:...
    size_t start = 0;
    while (start <= p.size()) {
        size_t slash = p.find('/', start);
        std::string seg = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg == "." || seg == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

void set_mtime(const std::string& path, int64_t mtime) {
    utimbuf_t t{};
    t.actime = static_cast<decltype(t.actime)>(mtime);
    t.modtime = static_cast<decltype(t.modtime)>(mtime);
    RASYNC_UTIME(path.c_str(), &t);
}

// Move `from` onto `to`, replacing an existing `to`, creating parent dirs.
bool move_replace(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::path dst(to);
    if (dst.has_parent_path()) fs::create_directories(dst.parent_path(), ec);
    fs::rename(from, dst, ec);
    if (!ec) return true;
    // Some platforms won't rename over an existing file — drop it and retry.
    ec.clear();
    fs::remove(dst, ec);
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
    : service_(service), peer_(std::move(peer)) {}

SyncSession::~SyncSession() { stop(); }

void SyncSession::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    sender_ = std::thread([this] { sender_loop(); });
    send_hello();
}

void SyncSession::stop() {
    if (!running_.exchange(false)) return;
    { std::lock_guard<std::mutex> lk(mtx_); queue_cv_.notify_all(); }
    { std::lock_guard<std::mutex> lk(send_mtx_); send_cv_.notify_all(); }
    if (sender_.joinable()) sender_.join();
    std::lock_guard<std::mutex> lk(mtx_);
    incoming_.clear();  // Incoming dtor drops temp files
}

// ── inbound dispatch (reactor thread) ────────────────────────────────────────

void SyncSession::handle(librats::ByteView payload) {
    BinaryReader r(payload.data(), payload.size());
    auto op = static_cast<proto::Op>(r.u8());
    if (!r.ok()) return;
    switch (op) {
        case proto::Op::Hello:        handle_hello(r); break;
        case proto::Op::Manifest:     handle_manifest(r); break;
        case proto::Op::Request:      handle_request(r); break;
        case proto::Op::FileStart:    handle_file_start(r); break;
        case proto::Op::FileLiteral:  handle_file_literal(r); break;
        case proto::Op::FileCopy:     handle_file_copy(r); break;
        case proto::Op::FileEnd:      handle_file_end(r); break;
        case proto::Op::FileAck:      handle_file_ack(r); break;
        case proto::Op::NotFound:     handle_not_found(r); break;
        case proto::Op::Changed:      send_manifest_if_changed(); break;
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
    auto peer_mode = static_cast<SyncMode>(r.u8());
    r.u8();          // conflict policy (informational)
    r.u64();         // base generation (informational)
    r.str16();       // name
    if (!r.ok()) return;
    if (version != proto::kVersion)
        service_.log(2, "peer speaks protocol v" + std::to_string(version) +
                        " (we are v" + std::to_string(proto::kVersion) + ")");
    if (peer_mode != service_.config().mode)
        service_.log(2, "peer sync mode differs from ours — results may be surprising");
    // Bootstrap the exchange: advertise our current tree.
    send_manifest_if_changed();
}

void SyncSession::handle_manifest(BinaryReader& r) {
    auto m = Manifest::decode(r);
    if (!m) { service_.log(2, "dropping malformed manifest"); return; }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        remote_ = std::move(*m);
        have_remote_ = true;
    }
    reconcile_and_act();
}

// ── advertising + reconciliation ─────────────────────────────────────────────

void SyncSession::send_manifest_if_changed() {
    Manifest local = service_.local_manifest();
    Hash fp = local.fingerprint();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (last_sent_valid_ && fp == last_sent_fp_) return;  // nothing new to advertise
        last_sent_fp_ = fp;
        last_sent_valid_ = true;
    }
    auto w = proto::message(proto::Op::Manifest);
    local.encode(w);
    send_msg(w);
}

void SyncSession::local_changed() {
    send_manifest_if_changed();
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
        std::error_code ec;
        fs::remove(service_.abs_path(path), ec);
        service_.note_local_removed(path);
        mutated = true;
        service_.log(0, "deleted " + path);
    }

    // 2. Request the files we need (with a signature when we can delta).
    for (const auto& path : plan.pull) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!pulling_.insert(path).second) continue;  // already requested
        }
        auto w = proto::message(proto::Op::Request);
        w.str16(path);

        const FileMeta* have = local.find(path);
        std::string abs = service_.abs_path(path);
        bool sent_sig = false;
        if (cfg.use_delta && have && have->size <= cfg.delta_max_bytes &&
            librats::file_exists(abs)) {
            if (auto sig = signature_of_file(abs, cfg.block_size)) {
                w.u8(1);
                sig->encode(w);
                sent_sig = true;
            }
        }
        if (!sent_sig) w.u8(0);
        send_msg(w);
    }

    if (mutated) send_manifest_if_changed();  // tell the peer our tree shrank
    maybe_synced();
}

void SyncSession::maybe_synced() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!pulling_.empty() || !incoming_.empty()) return;
    }
    if (outstanding_serves_.load() != 0) return;

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
    job.has_sig = r.u8() != 0;
    if (job.has_sig) {
        auto sig = Signature::decode(r);
        if (!sig) return;
        job.sig = std::move(*sig);
    }
    if (!r.ok() || !is_safe_rel(job.rel_path)) return;
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
    size_t sz = 0;
    void* raw = librats::read_file_binary(abs.c_str(), &sz);
    if (!raw) { serve_whole(job, abs, size, mtime, mode); return; }
    std::vector<uint8_t> data(static_cast<uint8_t*>(raw), static_cast<uint8_t*>(raw) + sz);
    librats::free_file_buffer(raw);

    Delta delta = compute_delta(job.sig, data.data(), data.size());
    Hash digest = sha256(data.data(), data.size());

    auto start = proto::message(proto::Op::FileStart);
    start.u64(job.xid); start.str16(job.rel_path);
    start.u64(data.size()); start.i64(mtime); start.u32(mode); start.u8(1);
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
                    " of " + std::to_string(data.size()) + " bytes on wire)");
    if (service_.events().file_done)
        service_.events().file_done({TransferInfo::Send, job.rel_path, data.size(), data.size(), true, on_wire});
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
    if (!r.ok() || !is_safe_rel(in->rel_path)) return;

    in->final_path = service_.abs_path(in->rel_path);
    std::string tmpdir = librats::combine_paths(service_.config().root, ".rasync-tmp");
    librats::create_directories(tmpdir.c_str());
    in->temp_path = librats::combine_paths(tmpdir, std::to_string(in->xid) + ".part");

    if (!in->out.open_write(in->temp_path.c_str())) {
        service_.log(2, "cannot open temp file for " + in->rel_path);
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
        std::string path = in->rel_path;
        bool was_delta = in->is_delta;
        fail_incoming(in, "hash mismatch");
        if (was_delta) {
            service_.log(2, "delta verify failed for " + path + " — retrying whole-file");
            auto w = proto::message(proto::Op::Request);
            w.str16(path); w.u8(0);
            send_msg(w);  // path stays in pulling_, so the round still waits for it
        } else {
            std::lock_guard<std::mutex> lk(mtx_);
            pulling_.erase(path);
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
        std::lock_guard<std::mutex> lk(mtx_);
        pulling_.erase(in->rel_path);
        incoming_.erase(in->xid);
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
        pulling_.erase(in->rel_path);
        incoming_.erase(in->xid);
    }
    service_.log(0, "received " + in->rel_path);
    if (service_.events().file_done)
        service_.events().file_done({TransferInfo::Recv, in->rel_path, in->written, in->size, in->is_delta, in->on_wire});

    // Our tree grew: advertise it so the peer sees convergence, then check quiescence.
    send_manifest_if_changed();
    maybe_synced();
}

void SyncSession::fail_incoming(const std::shared_ptr<Incoming>& in, const std::string& why) {
    in->failed = true;
    in->out.close();
    in->base.close();
    service_.log(2, "transfer of " + in->rel_path + " failed: " + why);
    std::lock_guard<std::mutex> lk(mtx_);
    incoming_.erase(in->xid);
}

void SyncSession::handle_not_found(BinaryReader& r) {
    std::string path = r.str16();
    if (!r.ok()) return;
    service_.log(2, "peer no longer has " + path);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pulling_.erase(path);
    }
    maybe_synced();
}

} // namespace rasync
