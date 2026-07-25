# rasync

**Real-time, two-way directory synchronization over a peer-to-peer network.**

`rasync` keeps two directories on different machines identical — continuously and
in both directions. Think `rsync`, but automatic: point it at a folder on each
side, and any file you create, edit, move, or delete is detected and mirrored to
the other end within seconds. Under the hood it uses the [librats](src/librats)
P2P stack for encrypted transport and peer discovery, and the classic rsync
rolling-checksum algorithm so that editing one byte of a huge file sends only the
bytes that actually changed.

```
$ rasync -p 9000 ./photos                     # host A: share ./photos
$ rasync --peer a.example.com:9000 ./photos    # host B: keep ./photos identical
```

```
● connected to peer 0dbc3630
  ↓ 2024/summer/IMG_2231.jpg (4.2 MiB)
  ↑ notes.md (1.1 KiB, delta 240 B on wire)
✓ in sync with 0dbc3630 — 1,284 files, 6.1 GiB
```

---

## Features

- **Two-way merge or one-way mirror.** Two-way keeps both sides converged with a
  proper three-way merge (it can tell "created here" from "deleted there"). Mirror
  makes a replica byte-identical to an authoritative source.
- **Real-time.** A background scan loop detects changes and reconciles on a short
  interval; changes propagate automatically while it runs.
- **Efficient on big trees.** Change detection is a size+mtime quick-check backed
  by a persistent hash cache, so re-scanning a large tree hashes only what changed.
- **Efficient on the wire.** Files a peer already has an older copy of are sent as
  an **rsync-style delta** — only changed blocks travel; unchanged blocks are
  reconstructed from the receiver's own copy. Renames/duplicates are free (matched
  by content hash).
- **Encrypted & authenticated.** All traffic rides librats' Noise_XX transport;
  peers are self-certifying (a peer's identity *is* its public key).
- **Automatic peer discovery.** Optionally find the other side over the DHT and/or
  mDNS by a shared key — no fixed IPs required.
- **Integrity end-to-end.** Every transferred file is verified against a whole-file
  SHA-256 before it atomically replaces the old one; a mismatch retries safely.
- **Safe by construction.** Downloads land in a temp dir and are renamed into place
  only after verification; peer-supplied paths are validated against traversal.
- **Nice to use.** Coloured, human-readable output, a real `--help`, gitignore-style
  excludes, and quiet/verbose modes.

---

## How it works

### The sync protocol

rasync runs one small, symmetric protocol over a single librats application
channel. Both peers behave identically:

1. **Exchange manifests.** A *manifest* is a snapshot of a tree: every file's path,
   size, mtime and SHA-256. On connect (and whenever a side's tree changes) each
   peer sends its manifest.
2. **Reconcile locally.** Each side independently runs a **three-way merge** of
   `base` (the last-synced manifest, persisted on disk), `local` (its current tree)
   and `remote` (the peer's manifest). The result tells it exactly which files to
   *pull* and which to *delete locally*. Because the decision is a deterministic
   pure function and both sides share the same `base`, the two plans are perfect
   complements — the trees converge with no central coordinator.
3. **Transfer.** Each side requests the files it needs. If it already has an older
   version, it attaches an rsync *signature* so the sender replies with a compact
   **delta**; otherwise the file is streamed whole. Transfers are windowed
   (acknowledged) so a large file never stalls or floods the connection.
4. **Verify & converge.** The receiver reconstructs each file into a temp file,
   checks its SHA-256, sets its mtime, and atomically moves it into place. When a
   side has nothing left to do it re-advertises its manifest so the peer sees the
   convergence, persists the new baseline, and prints `✓ in sync`.

Conflicts (both sides edited the same file since the last sync) are resolved by a
deterministic policy — newest mtime wins by default (`--conflict larger|local|remote`
to change it) — so both ends always pick the same winner.

The two peers must run **complementary** policies. `newer` and `larger` compare the
two versions themselves, so both sides reach the same verdict from the same setting.
`local` and `remote` name a *side*, and "local" is a different tree on each peer — so
they pair with each other: `--conflict local` on the side that should win, `--conflict
remote` on the other. Two peers both set to `local` would each keep their own version
and never converge, so rasync refuses that pairing at the handshake instead of
stalling silently.

### The delta algorithm

This is the rsync trick that makes editing a 1 GiB file cheap:

1. The receiver splits its copy into fixed-size blocks and sends, per block, a fast
   rolling (Adler-style) checksum plus a strong SHA-256.
2. The sender slides a byte-at-a-time window over its newer file, cheaply testing
   each position's rolling checksum against the receiver's set and only confirming a
   hit with the strong hash. Matches become `COPY(offset, len)` instructions
   ("reuse your own bytes"); the gaps become `LITERAL` bytes actually sent.
3. The receiver reconstructs the new file from `COPY` (read from its old copy) and
   `LITERAL` (from the wire) instructions. The whole-file SHA-256 is the safety net:
   if a block ever matched wrongly, verification fails and rasync re-pulls the file
   whole.

### Architecture

```
src/rasync/
  core/     pure, network-free, exhaustively unit-tested logic
    serialize.h   compact bounds-checked big-endian codec
    hash.*        SHA-256 for buffers and streamed files
    manifest.*    path → {size, mtime, mode, hash}; wire + disk form; fingerprint
    scanner.*     directory walk → manifest, with an incremental hash cache
    ignore.*      gitignore-style path filtering
    delta.*       rsync rolling-checksum signature / delta / patch
    diff.*        three-way merge → per-side sync plan
  net/        the protocol, built on librats' Node
    protocol.*      opcodes + wire layout
    sync_session.*  per-peer reconcile + transfer state machine
    sync_service.*  ties a librats Node to the sessions; owns the manifests
  app/        the CLI
    terminal.*  colours + human-readable sizes/rates (no dependencies)
    cli.*       argument parsing + --help
    daemon.*    node + discovery + scan loop + terminal UI
  main.cpp
```

The `core` layer knows nothing about networking, which is why it can be — and is —
tested in isolation. `net` builds the protocol on librats' narrow subsystem
contract; `app` is the user-facing shell.

---

## Building

Requirements: a C++17 compiler, CMake ≥ 3.15, and (on first configure) network
access to fetch GoogleTest. librats is included as a git submodule.

```bash
git clone --recurse-submodules <this-repo> rasync   # or: git submodule update --init --recursive
cd rasync
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The binary lands at `build/bin/rasync`. To skip the test build:

```bash
cmake -S . -B build -DRASYNC_BUILD_TESTS=OFF
```

### Running the tests

```bash
cd build
ctest --output-on-failure
```

66 tests cover the codec, hashing, manifests, ignore matching, the delta algorithm,
the three-way merge, the scanner, the wire protocol, and — end to end — two live
nodes converging over loopback (initial populate, two-way merge, delta-based
modification, deletion propagation, and mirror mode).

---

## Usage

```
rasync [options] <directory>
```

### Connecting two peers

**Explicit address** — one side listens, the other dials:

```bash
# host A
rasync -p 9000 ./data
# host B
rasync --peer a.example.com:9000 ./data
```

**Automatic discovery** — both sides find each other by a shared secret key over
the DHT and local network; no fixed IP needed:

```bash
# run on both hosts
rasync --key my-project ./data
```

Add `--lan` to restrict discovery to the local network (mDNS only).

### One-way mirror (backup)

```bash
# on the source of truth
rasync --mirror --source -p 9000 ./website
# on the backup box — becomes byte-identical to the source
rasync --mirror --replica --peer web.example.com:9000 ./website-backup
```

### Options

| Option | Description |
|---|---|
| `-p, --port <n>` | Listen on TCP port `<n>` (default: ephemeral). |
| `--peer <host:port>` | Dial a specific peer. Repeatable. |
| `--key <secret>` | Auto-discover peers sharing this key (enables DHT + mDNS). |
| `--discover` | Enable DHT + mDNS discovery without a key. |
| `--lan` | Discover on the local network only (mDNS). |
| `--mirror` | One-way mirror instead of two-way merge. |
| `--source` / `--replica` | Mirror role (the source is authoritative). |
| `--conflict <policy>` | `newer` (default), `larger`, `local`, or `remote`. Peers must pair: `newer`/`newer`, `larger`/`larger`, `local`/`remote`. |
| `--no-delete` | Never delete files — additive sync only. |
| `--no-delta` | Send whole files instead of rsync deltas. |
| `--ignore <pattern>` | Exclude paths (gitignore syntax). Repeatable. |
| `--ignore-file <path>` | Load ignore patterns from a file. |
| `--interval <secs>` | Rescan cadence for change detection (default: 3). |
| `--once` | Reconcile once with connected peers, then exit. |
| `--data-dir <path>` | State directory (default: `<directory>/.rasync`). |
| `-v, --verbose` | Show every file operation (and librats internals). |
| `-q, --quiet` | Only warnings and errors. |
| `--no-color` | Disable coloured output. |
| `-h, --help` / `--version` | Help / version. |

### Ignoring files

Patterns follow gitignore conventions. Put them in a `.rasyncignore` file at the
root of the synced directory (auto-loaded), pass `--ignore-file <path>`, or add
individual `--ignore <pattern>` flags. rasync's own `.rasync/` state directory is
always excluded.

```gitignore
# .rasyncignore
node_modules/
*.tmp
*.log
/build
!keep.log
```

---

## Notes & limitations

- **Change detection uses size + mtime** (mtime at 1-second resolution), the same
  fast heuristic as rsync's default. A change that keeps a file's exact size *and*
  happens within the same wall-clock second as the previous scan can be missed
  until the next real change. (A full-checksum scan mode is a natural future
  addition.)
- **Baseline & delete propagation.** Two-way delete detection relies on the
  persisted baseline in `<dir>/.rasync/`. Deleting that state makes rasync treat the
  next sync as a first-time union (nothing is deleted). Use `--no-delete` for purely
  additive syncing.
- **Two peers per session.** The design converges any number of peers pairwise, but
  it is built and tested for the two-directory case.
- **Large single files & delta.** Delta reconstruction reads the sender's file into
  memory to run the scan; files above `256 MiB` fall back to whole-file streaming
  (always bounded memory). Everything else streams in fixed windows.
- **Symlinks and empty directories** are not tracked in this version (regular files
  only).
- Both peers should run with the **same mode** (`two-way` vs `mirror`); rasync warns
  on a mismatch but does not enforce it. Conflict policies, by contrast, **are**
  enforced: they must be complementary (`newer`/`newer`, `larger`/`larger`,
  `local`/`remote`), because a mismatched pair either stalls forever or swaps the two
  versions back and forth on every round. A peer that offers a non-complementary
  policy is refused at the handshake, with a message naming the flag to fix it.

---

## License

See [LICENSE](LICENSE). librats is included under its own license (see
[src/librats/LICENSE](src/librats/LICENSE)).
