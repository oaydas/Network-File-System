# Network File System Server

A multithreaded, networked file system server implemented in **C++**, supporting concurrent client access, hierarchical directories, and block-based file operations with strong consistency guarantees.

This project was designed as a systems-level exercise in **network programming**, **concurrency control**, and **file system design**, emphasizing correctness, synchronization, and robustness under concurrent access.

---

## Features

### Core File System Operations
The server supports the following filesystem requests from remote clients:

- **Create** files and directories  
- **Delete** files and empty directories  
- **Read** fixed-size file blocks  
- **Write** fixed-size file blocks (with automatic file growth)  

All operations are validated against:
- File ownership  
- Directory permissions  
- Path correctness  
- File size and block limits  

---

### Network Architecture

- TCP server using POSIX sockets  
- Dynamically assigned or user-specified port  
- One thread per client connection (via `boost::thread`)  
- Robust message framing with null-terminated request headers  
- Graceful handling of malformed or partial client requests  

Each client request is handled independently, allowing multiple clients to safely operate on the file system concurrently.

---

## File System Design

### On-Disk Layout
- Fixed-size disk blocks (`FS_BLOCKSIZE`)  
- Inodes stored directly in disk blocks  
- Each inode contains:
  - Type (`file` or `directory`)
  - Owner username
  - Size (number of blocks)
  - Direct block pointers

### Directories
- Directories store arrays of `fs_direntry`  
- Each directory entry maps a filename to an inode block  
- Supports dynamic growth up to a maximum block limit  
- Automatic directory block allocation and compaction on delete  

---

## Concurrency & Synchronization

This project places heavy emphasis on **correct concurrent behavior**.

### Locking Strategy
- **Per-inode locks** using `shared_mutex`  
- Lock table managed via `weak_ptr` to avoid leaks  
- Supports:
  - `shared_lock` for readers  
  - `upgrade_lock` for read-then-write transitions  
  - `unique_lock` for writers  

### Hand-Over-Hand Path Traversal
- Safe directory traversal using lock coupling  
- Parent lock released only after child lock is acquired  
- Prevents races during concurrent path resolution  

### Free Block Management
- Centralized free-block set protected by a mutex  
- Disk blocks reclaimed safely on delete  
- File growth and directory expansion are atomic  

---

## Path Resolution & Permissions

- Absolute paths resolved component-by-component  
- Ownership enforced at every directory boundary  
- Root directory (`/`) is globally accessible  
- Only owners (or root) may modify filesystem objects  

Failure at any point results in a clean abort with no partial state changes.

---

## Robustness Guarantees

- No partial writes: metadata updates follow data writes  
- Directory updates are ordered to avoid dangling references  
- Disk blocks are never leaked or double-freed  
- Server remains stable under malformed or adversarial requests  

---

## Key Implementation Highlights

- DFS-based filesystem scan on startup to reconstruct free block state  
- Explicit ordering of disk writes to preserve consistency  
- `MSG_WAITALL` used to ensure full block transfers  
- Safe cleanup on early exits and failures  
- Clear separation between networking, parsing, and filesystem logic  

---

## Technologies Used

- **C++17**  
- **POSIX sockets**  
- **Boost threads & synchronization primitives**  
- **Custom disk abstraction**  
- **RAII-based locking**  
- **Modern concurrency patterns**  

---

## Summary

This project demonstrates a fully functional **networked file system server** with:

- Strong correctness guarantees  
- Fine-grained synchronization  
- Clean separation of concerns  
- Realistic filesystem semantics  

It reflects real-world systems challenges such as concurrency, crash safety, and multi-client coordination, and was implemented with an emphasis on clarity, safety, and performance.
