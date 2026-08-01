# rasync

<p align="center"><a href="https://github.com/librats/rasync"><img src="https://raw.githubusercontent.com/librats/rasync/master/docs/logo.png"></a></p>

[![MIT License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)]()
[![Release](https://img.shields.io/github/release/librats/rasync.svg)](https://github.com/librats/rasync/releases)

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

Several folders travel over one connection — name as many directories as you like:

```
$ rasync --key s3cr3t ./documents ./photos ./code
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
- **Many folders, one connection.** Name as many directories as you like; they are
  paired with the peer by folder name and multiplexed over the single encrypted
  connection, each with its own mode, conflict policy, ignore rules and baseline.
- **Efficient on big trees.** Change detection is a size+mtime quick-check backed
  by a persistent hash cache, so re-scanning a large tree hashes only what changed.
- **Efficient on the wire.** Files a peer already has an older copy of are sent as
  an **rsync-style delta** — only changed blocks travel; unchanged blocks are
  reconstructed from the receiver's own copy. Renames/duplicates are free (matched
  by content hash).
- **Encrypted & access-controlled.** All traffic rides librats' Noise_XX transport;
  peers are self-certifying (a peer's identity *is* its public key). `--key` makes
  that key a shared secret bound into the handshake, so a peer without it cannot
  connect at all; `--allow` pins the exact peer ids you sync with. See
  [Access control](#access-control).
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

0. **Pair up the folders.** Every message opens with a 32-bit *folder tag* derived
   from the folder's **name**, so one connection carries every folder the two peers
   share and each message lands in the right tree. Local paths cannot be that name —
   the same tree is `~/docs` here and `D:\Documents` there — so the name defaults to
   the directory's own leaf and `--name` overrides it. A folder only one side has
   costs a single `Hello`, and each side reports the other's unmatched folders by
   name rather than quietly doing nothing.
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
    protocol.*      folder tag, opcodes + wire layout
    sync_session.*  per (folder, peer) reconcile + transfer state machine
    sync_service.*  one synced folder: its manifests and its per-peer sessions
    sync_router.*   the one bridge to the Node: routes each message to its folder
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

151 tests cover the codec, hashing, manifests, ignore matching, the delta algorithm,
the three-way merge, the scanner, path safety, key derivation, the state directory,
the wire protocol, command-line parsing, and — end to end — two live nodes converging
over loopback (initial populate, two-way merge, delta-based modification, deletion
propagation, mirror mode, incremental manifest traffic, access control, the handshake
refusals, and several folders multiplexed over one connection).

They build into three binaries, by layer: `rasync_core_tests` (pure logic, no
networking), `rasync_net_tests` (protocol + live loopback), `rasync_app_tests` (CLI).

---

## Usage

```
rasync [options] <directory> [[options] <directory>]...
```

One run syncs one directory or many. Options split in two: **node-level** ones
(`--port`, `--key`, `--peer`, `--allow`, `--discover`, `--interval`, `--once`,
`--data-dir`, output flags) apply to the whole run, because one connection carries
every folder. The rest describe a single folder and are **positional** — they apply
to the directory they follow, and any given before the first directory become the
default for every folder:

```bash
rasync --conflict newer ./docs ./photos --mirror --source
#       \_ default for both _/          \_ photos only _/
```

### Several folders

```bash
# both hosts, all three folders over one connection
rasync --key s3cr3t ./documents ./photos ./code

# two-way for one folder, one-way backup for another
rasync --key s3cr3t ./work ./archive --mirror --source
```

Folders pair up **by name**, which defaults to the directory's own leaf. When the
two machines keep a tree under different names, say so with `--name`:

```bash
# host A
rasync --key s3cr3t ~/Documents --name documents ~/Pictures --name photos
# host B
rasync --key s3cr3t "D:\Docs" --name documents "D:\Photos" --name photos
```

A folder the other side does not have is simply inert — and reported, so a name
typo reads as a message rather than as a sync that never starts:

```
! peer c449bbd9 offers folder 'docs', which is not configured here
  (run both sides with the same folder name — see --name)
```

Two folders in one run may not share a name, and one synced folder may not sit
inside another; both are refused at startup.

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

### Access control

Encryption alone does not decide *who* may sync. Two levers do:

**`--key <secret>` — a shared secret, and the one you should use.** The key is
hashed into the librats handshake protocol id, which librats binds into the
Noise_XX prologue. A peer that does not hold the same key **cannot complete the
handshake**: no session, no manifest exchange, not one byte of the tree. That
holds however the peer reached you — a DHT lookup, an mDNS neighbour on the LAN,
or a hand-typed `--peer` straight at your port.

```bash
# both hosts — and nobody else
rasync --key "$(openssl rand -hex 24)" ./data
```

The key is a password. Anyone holding it can read *and* overwrite the whole
synced tree. It is also exposed to offline guessing: rasync announces a hash of
it as the DHT rendezvous point, which is public by design, so a short or
dictionary key can be brute-forced by anyone crawling the DHT. Use a long random
one, and pass it via a file or environment variable rather than a shell history.

**`--allow <peer-id>` — pin the exact peers.** Repeatable; when present, only
those peer ids may sync. A peer id is self-certifying (it *is* the SHA-256 of the
peer's Noise public key, proven by the handshake), so it cannot be forged. Each
side prints its own id at startup:

```
rasync 0.1.0
  id       3f9c…64 hex chars…
```

```bash
rasync --key hunter2 --allow 3f9c...  ./data
```

Use it as a second line of defence if a key might leak, or on its own when
running keyless on a trusted network. A peer that is not listed still completes
the handshake but is then ignored outright — it is never told what the tree
contains and never has a request served.

**With neither flag, the sync is open**: any rasync build reaching your listening
port can pull the directory and push into it. The startup banner says so in
yellow. Without a key, prefer a firewalled port and explicit `--peer` dialling,
and leave `--discover` off.

### One-way mirror (backup)

```bash
# on the source of truth
rasync --mirror --source -p 9000 ./website
# on the backup box — becomes byte-identical to the source
rasync --mirror --replica --peer web.example.com:9000 ./website-backup
```

### Options

Node-level — one connection carries every folder, so these are not per-folder:

| Option | Description |
|---|---|
| `-p, --port <n>` | Listen on TCP port `<n>` (default: ephemeral). |
| `--peer <host:port>` | Dial a specific peer. Repeatable. |
| `--key <secret>` | Shared secret. Peers must match it to complete the handshake; also keys auto-discovery (enables DHT + mDNS). See [Access control](#access-control). |
| `--allow <peer-id>` | Only sync with this peer id (64 hex, as printed at startup). Repeatable. |
| `--discover` | Enable DHT + mDNS discovery without a key. |
| `--lan` | Discover on the local network only (mDNS). |

Per folder — applies to the directory it follows (or, before the first directory,
to all of them):

| Option | Description |
|---|---|
| `--name <label>` | The name both peers know this folder by (default: the directory's leaf name). Both sides must agree, or the folder never pairs. |
| `--mirror` | One-way mirror instead of two-way merge. |
| `--source` / `--replica` | Mirror role (the source is authoritative). |
| `--conflict <policy>` | `newer` (default), `larger`, `local`, or `remote`. Peers must pair: `newer`/`newer`, `larger`/`larger`, `local`/`remote`. |
| `--no-delete` | Never delete files — additive sync only. |
| `--no-delta` | Send whole files instead of rsync deltas. |
| `--follow-symlinks` | Sync what a symbolic link points at as if it were a real file or directory. See [Symbolic links](#symbolic-links). |
| `--ignore <pattern>` | Exclude paths (gitignore syntax). Repeatable. |
| `--ignore-file <path>` | Load ignore patterns from a file. |

Run mode and output, node-level again:

| Option | Description |
|---|---|
| `--interval <secs>` | Rescan cadence for change detection (default: 3). |
| `--once` | Reconcile once with connected peers, then exit (once *every* folder has). |
| `--data-dir <path>` | Keep the sync state here instead of in the per-user state directory (see [State](#state-where-rasync-keeps-its-own-files)). |
| `-v, --verbose` | Show every file operation (and librats internals). |
| `-q, --quiet` | Only warnings and errors. |
| `--no-color` | Disable coloured output. |
| `-h, --help` / `--version` | Help / version. |

### State: where rasync keeps its own files

**The synced directory holds your files and nothing else.** Everything rasync
remembers between runs lives in the per-user state root:

| Platform | Location |
|---|---|
| Windows | `%LOCALAPPDATA%\rasync\` |
| macOS | `~/Library/Application Support/rasync/` |
| Linux / BSD | `$XDG_DATA_HOME/rasync/` (default `~/.local/share/rasync/`) |

Under it, two kinds of state:

- `node/` — the **node's** identity key and DHT routing table. It belongs to the
  process, not to any folder: one run syncs many folders over one node, and the
  peer id `--allow` names must not depend on which directory was listed first.
- `dirs/<name>-<hash>/` — one **folder's** baseline manifest. `<name>` is the
  directory's own name and `<hash>` a prefix of the SHA-256 of its absolute path,
  so two directories that share a name never share state. Each holds a
  `directory.txt` naming the tree it belongs to.

The node path is printed as `state` in the startup banner. Set `RASYNC_HOME` to
move the whole root elsewhere. `--data-dir <path>` overrides it with a root of
the same shape — `node/` and `dirs/` under the path you give (e.g. a portable
drive, so the state travels with the data). The shape does not depend on how
many directories you sync: adding a second folder to an existing `--data-dir`
must not move the identity key or a baseline out from under itself.

The one thing rasync writes into the synced tree is `.rasync-tmp/`, the scratch
directory for in-flight downloads. It has to be there: a finished transfer is
verified and then *renamed* into place, which must not cross a filesystem. It is
created on demand, hidden on Windows, never synced, removed as soon as the last
transfer of a round lands, and wiped at startup and shutdown — so an idle rasync
leaves nothing behind.

### Ignoring files

Patterns follow gitignore conventions. Put them in a `.rasyncignore` file at the
root of the synced directory (auto-loaded), pass `--ignore-file <path>`, or add
individual `--ignore <pattern>` flags. rasync's own `.rasync-tmp/` scratch
directory (and any `.rasync/` left by an older version) is always excluded.

```gitignore
# .rasyncignore
node_modules/
*.tmp
*.log
/build
!keep.log
```

### Symbolic links

**By default a symbolic link is skipped, never followed.** A manifest describes
files, not links, so following one would either copy data from outside the synced
tree to the peer without saying so, or — for a link pointing back into the tree —
walk in circles forever. Each scan reports how many it passed over, so a file that
is not syncing is not a mystery.

**On Windows a directory junction counts as a link too.** It carries a different
reparse tag and no C++ standard library calls it a link, but it points at a
directory exactly as a symlink does — including at its own parent, which is the
shape Windows puts in every user profile (`AppData\Local\Application Data`).
rasync classifies by the tag the OS itself marks as a name surrogate, so a
junction is skipped, followed and counted wherever this section says "link".
Reparse points that are still ordinary files — OneDrive placeholders,
deduplicated and WIM-backed files — are not links and sync as the files they are.

`--follow-symlinks` makes rasync read a link as **the thing it points at**: a link
to a file is scanned as that file, a link to a directory is descended into, and the
peer receives ordinary files and directories. That is what lets a Linux tree built
out of links arrive on a Windows machine — where creating one needs a privilege
the user may not have — as the same structure in plain form. It is a per-folder, purely local decision: nothing
about it is negotiated, and the peer needs no matching flag.

What follows from treating a link as the real thing:

- **Content from outside the tree is synced.** That is the point of the flag, and
  the reason it is not the default: `--follow-symlinks` on a tree with a link to
  `/etc` sends `/etc` to the peer. Combine it with `--ignore` for links you do not
  want followed — an excluded path is never even probed.
- **An update coming back is written to the link's target,** not over the link. The
  link stays a link and the data it points at is what changes — which is already
  how a file *inside* a linked directory behaves, since the OS resolves that path
  for us. Overwriting the link with a copy instead would silently detach the tree
  from the data it was deliberately pointed at.
- **A delete removes the link, not its target.** The path disappears here and both
  trees agree, but data outside the synced directory is never deleted on a peer's
  say-so. (A file *inside* a linked directory is a normal file at a resolved path,
  and is deleted like any other.)
- **Loops terminate.** Following turns the tree into a graph, and rasync refuses to
  descend into a directory that is already on the chain it walked through — the
  rule `find -L` uses. Two separate links to one directory are not a loop and are
  both walked, so the peer sees the structure that is really there (and the content
  twice).
- **A broken link is skipped and counted,** never synced as an empty file.

---

## Notes & limitations

- **Change detection uses size + mtime** (mtime at 1-second resolution), the same
  fast heuristic as rsync's default. A change that keeps a file's exact size *and*
  happens within the same wall-clock second as the previous scan can be missed
  until the next real change. (A full-checksum scan mode is a natural future
  addition.)
- **Baseline & delete propagation.** Two-way delete detection relies on the
  persisted baseline in the [state directory](#state-where-rasync-keeps-its-own-files).
  Deleting that state makes rasync treat the next sync as a first-time union (nothing
  is deleted). Use `--no-delete` for purely additive syncing. Note that the state is
  per user and per machine: moving a synced directory to a new path gives it a fresh
  state directory, and therefore a first-time sync.
- **Two peers per session.** The design converges any number of peers pairwise, but
  it is built and tested for the two-directory case.
- **The shared key is symmetric, and node-wide.** There is one secret and it grants
  full read and write access to every folder the run carries; there is no read-only
  or per-folder credential, and no way to revoke one peer short of changing the key
  (or listing the peers you keep with `--allow`). Changing the key changes the
  handshake protocol id, so every peer must be restarted with the new one at the
  same time. One process is therefore **one trust domain**: to share different
  folders with different people under different secrets, run one rasync per secret.
- **Folder names are matched byte for byte.** They are how two machines agree that
  two paths are one tree, so `docs` and `Docs` are different folders. A folder only
  one side has is inert and reported by name; nothing pairs it up automatically.
- **Folders cost threads per peer.** A shared folder opens a session, and a session
  runs a sender and a requester thread. A folder the peer does not have costs
  neither — but many folders times many peers is still many threads.
- **Folders are scanned in turn**, on one thread, on the `--interval` tick. A steady
  state pass reads metadata only and is cheap; a folder in the middle of a large
  import delays its neighbours' next scan by its own scan time.
- **Large single files & delta.** Delta reconstruction reads the sender's file into
  memory to run the scan; files above `256 MiB` fall back to whole-file streaming
  (always bounded memory). Everything else streams in fixed windows.
- **Regular files only.** A symlink — and on Windows a directory junction — is
  skipped unless `--follow-symlinks` is given, in which case it is synced as the
  file or directory it points at; a link itself is never reproduced on the peer,
  because a manifest cannot represent one. See
  [Symbolic links](#symbolic-links). Empty directories are not tracked either; the
  only ones rasync removes are those its own deletions emptied.
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
