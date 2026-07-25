#pragma once

/**
 * @file auth.h
 * @brief Turning the shared key into the two values that reach the wire.
 *
 * The shared key (`--key`) is rasync's access control. It feeds two independent,
 * domain-separated derivations, so neither can be replayed as the other and
 * neither reveals the key:
 *
 *  - `protocol_id()` — the librats handshake protocol id. librats binds it into
 *    the Noise prologue, so a peer holding a different key **cannot complete the
 *    handshake**: no session, no manifest, not one byte of the tree. That makes
 *    it the authentication, and it covers every way a peer can reach us — a DHT
 *    hit, an mDNS neighbour, or a hand-typed `--peer` — not just the discovery
 *    path.
 *  - `discovery_id()` — the rendezvous label announced to the DHT. It is public
 *    by construction (the DHT stores its hash alongside our address for anyone
 *    to look up), which is exactly why it must be neither the key itself nor
 *    equal to the protocol id.
 *
 * Both are truncated hex digests: 128 bits is far more than a handshake tag or a
 * rendezvous label needs, and a short string keeps logs and banners readable.
 *
 * The key is a secret in the password sense — anyone holding it may read and
 * overwrite the whole synced tree — and it is exposed to offline guessing by
 * anyone crawling the DHT for the announced rendezvous hash. Use a long random
 * one. With no key at all both values fall back to the bare protocol name, which
 * every rasync build shares: the tree is then open to any peer that reaches the
 * port (see SyncConfig::allowed_peers for the other lever).
 */

#include <string>

namespace rasync {

/// librats handshake protocol id for this key. Empty key → the bare protocol
/// name, i.e. any rasync peer may handshake.
std::string protocol_id(const std::string& key);

/// DHT rendezvous label for this key. Empty key → the bare protocol name, i.e.
/// the rendezvous every keyless rasync shares.
std::string discovery_id(const std::string& key);

} // namespace rasync
