# Network File System

**A distributed, fault-tolerant network file system written from scratch in C** — featuring a central name server, replicated storage servers, concurrent multi-client access with sentence-level locking, access control, versioning via checkpoints, and automatic failover.

![Language](https://img.shields.io/badge/language-C11-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20POSIX-lightgrey.svg)
![Concurrency](https://img.shields.io/badge/concurrency-pthreads-orange.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)

Built with raw POSIX sockets and pthreads — no external libraries or frameworks. ~5,200 lines of C implementing the kind of problems real distributed systems face: metadata management, replication, failure detection, concurrent writes, and crash recovery.

---

## Architecture

```
┌─────────────┐          ┌──────────────┐          ┌─────────────────┐
│   Clients   │◄────────►│ Name Server  │◄────────►│ Storage Servers │
│  (multiple) │   TCP    │  (metadata,  │   TCP    │  (file data,    │
└──────┬──────┘          │ coordination)│          │  replication)   │
       │                 └──────┬───────┘          └────────▲────────┘
       │                        ▼                           │
       │                 ┌─────────────┐                    │
       │                 │  Journal &  │                    │
       │                 │ Checkpoints │                    │
       │                 └─────────────┘                    │
       └────────────── direct data transfer ────────────────┘
```

| Component | Role |
|-----------|------|
| **Name Server** | Single source of truth: file metadata, access control, file→server mapping, replication orchestration, heartbeat monitoring, metadata journaling |
| **Storage Servers** | Store actual file data, serve reads/writes/streams directly to clients, handle replication and checkpoints, respond to heartbeats |
| **Clients** | Interactive CLI; contact the Name Server for metadata, then transfer file data **directly** with Storage Servers to avoid a central bottleneck |

Default ports (defined in [common.h](include/common.h)): `9000` client→NS, `9001` SS registration, `9002` NS→SS commands, `9003` client→SS data.

## Key Features

### Core File Operations
- **Full file lifecycle** — `CREATE`, `READ`, `WRITE`, `DELETE`, `MOVE`, plus hierarchical folders (`CREATEFOLDER`, `VIEWFOLDER`)
- **Interactive word-by-word writing** with **sentence-level locking**, so multiple users can edit *different sentences of the same file concurrently*
- **Streaming reads** (`STREAM`) — file content delivered word-by-word
- **Remote script execution** (`EXEC`) — run shell scripts stored in the file system and get output + exit status back

### Reliability & Fault Tolerance
- **Automatic replication** — files are asynchronously replicated across all available storage servers on creation
- **Heartbeat failure detection** — PING/PONG protocol (5 s interval, 15 s timeout) marks servers DOWN/ONLINE
- **Transparent read failover** — if the primary server dies, reads are silently redirected to a live replica
- **Recovery synchronization** — a storage server that comes back online automatically fetches every file it missed
- **Versioning** — named checkpoints (`CHECKPOINT`, `VIEWCHECKPOINT`, `REVERT`) plus one-step `UNDO` from automatic backups
- **Metadata journaling** on the Name Server for crash recovery

### Security & Access Control
- **Per-file ownership and ACLs** — only the owner and users on the access list can read/write/execute
- **Owner-managed permissions** — `ADDACCESS` / `REMACCESS`
- **Request/approve workflow** — users can request access (`REQACCESS`); owners review (`SHOW_REQUESTS`) and grant (`APPROVE_REQ`)

### Performance
- **Trie-based path lookup** — O(k) resolution, where k is the path length
- **LRU cache** in front of the trie — O(1) hits for hot files, 20 entries with automatic eviction
- **Fully multi-threaded** — every client and storage server connection is handled on its own thread, with fine-grained mutex protection of shared state

## Getting Started

### Prerequisites
- Linux (or any POSIX system) with GCC supporting C11
- `make`, `pthread` (part of glibc)

### Build

```bash
git clone https://github.com/the-ronnie/Network-File-System.git
cd Network-File-System
make
```

This produces three executables in `bin/`: `nameserver`, `storageserver`, and `client`.

### Run (single machine)

Open three terminals from the repository root:

```bash
# Terminal 1 — Name Server
make run-ns

# Terminal 2 — Storage Server (enter 127.0.0.1 when prompted for the NS IP)
make run-ss

# Terminal 3 — Client (enter 127.0.0.1 and pick a username)
make run-client
```

### Run (across machines)

```bash
# Machine A — Name Server (it prints its own IP on startup)
./bin/nameserver

# Machine B — Storage Server (enter Machine A's IP when prompted)
./bin/storageserver

# Any machine — Client
./bin/client <nameserver-ip>
```

## Command Reference

| Command | Description |
|---------|-------------|
| `VIEW` / `LIST` | List files and registered users |
| `CREATE <file>` | Create a file (you become its owner) |
| `CREATEFOLDER <path>` / `VIEWFOLDER <path>` | Create / browse folders |
| `READ <file>` | Read a file (access-controlled, failover-aware) |
| `WRITE <file> <sentence#>` | Interactive write with sentence locking; finish with `ETIRW` |
| `STREAM <file>` | Stream file content word-by-word |
| `DELETE <file>` / `MOVE <src> <dst>` | Delete or move files |
| `UNDO <file>` | Revert to the automatic `.bak` backup |
| `CHECKPOINT <file> <tag>` | Create a named version snapshot |
| `LISTCHECKPOINTS <file>` / `VIEWCHECKPOINT <file> <tag>` | Inspect snapshots |
| `REVERT <file> <tag>` | Restore a checkpoint |
| `INFO <file>` | Show owner, size, timestamps, and access list |
| `ADDACCESS <file> <user>` / `REMACCESS <file> <user>` | Grant / revoke access (owner only) |
| `REQACCESS <file>` | Request access to someone else's file |
| `SHOW_REQUESTS` / `APPROVE_REQ <file> <user>` | Review / approve pending requests (owner only) |
| `EXEC <file>` | Execute a stored shell script and return its output |
| `EXIT` | Quit the client |

## Project Structure

```
Network-File-System/
├── include/
│   ├── common.h              # Shared constants, ports, error codes
│   └── socket_utils.h        # Socket helper declarations
├── src/
│   ├── nameserver/           # Name Server: metadata, ACLs, replication,
│   │   └── nameserver.c      #   heartbeats, trie + LRU cache (~2,900 LOC)
│   ├── storageserver/        # Storage Server: file I/O, checkpoints,
│   │   └── storageserver.c   #   replication handlers (~1,400 LOC)
│   ├── client/               # Interactive CLI client (~700 LOC)
│   │   └── client.c
│   └── common/
│       └── socket_utils.c    # Shared TCP connection helpers
├── docs/
│   ├── TECHNICAL_GUIDE.md    # Deep dive: data structures, sync, design rationale
│   ├── FAULT_TOLERANCE.md    # Replication & access-request implementation notes
│   └── TESTING.md            # Step-by-step manual test procedures
├── Makefile
├── LICENSE
└── README.md
```

## Documentation

| Document | Contents |
|----------|----------|
| [docs/TECHNICAL_GUIDE.md](docs/TECHNICAL_GUIDE.md) | Complete technical deep dive — architecture, data structures, mutex strategy, per-feature implementation walkthroughs, and design Q&A |
| [docs/FAULT_TOLERANCE.md](docs/FAULT_TOLERANCE.md) | How replication, heartbeats, failover, recovery sync, and the access-request system are implemented |
| [docs/TESTING.md](docs/TESTING.md) | Manual test procedures, including kill-a-server failover and recovery ("Phoenix") scenarios |

## Design Highlights

- **Why a central name server?** One source of truth for metadata keeps consistency simple, while direct client↔storage data transfer keeps it off the hot path.
- **Why sentence-level locks?** Whole-file locking would serialize collaborating writers; per-sentence locks let them work simultaneously without conflicts.
- **Why a trie + LRU cache?** Path lookup dominates every operation. The trie gives worst-case O(k) resolution and the cache makes repeated access O(1).
- **Why asynchronous replication?** File creation stays fast; durability catches up in the background, and recovery sync repairs any replica that was offline.

## Known Limitations

- Storage server ports are compile-time constants, so running two instances on one machine requires editing `common.h` (multi-machine deployment works out of the box).
- Replication is triggered on `CREATE` and recovery sync; `WRITE` updates reach replicas on the next sync rather than immediately.
- Replication is best-effort (no quorum writes or version vectors) — a deliberate scope decision, discussed in [docs/FAULT_TOLERANCE.md](docs/FAULT_TOLERANCE.md).

## License

Released under the [MIT License](LICENSE).

## Acknowledgments

Originally built as a team course project for an Operating Systems & Networks course (Monsoon 2025), then extended with fault tolerance, versioning, and access-request workflows.
