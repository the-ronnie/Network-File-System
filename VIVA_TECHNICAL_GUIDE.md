# Technical Implementation Guide - Distributed Document Collaboration System
## For Viva Voce Examination

---

## Table of Contents
1. [System Architecture Overview](#1-system-architecture-overview)
2. [Key Data Structures](#2-key-data-structures)
3. [Synchronization Mechanisms](#3-synchronization-mechanisms)
4. [Feature-wise Implementation](#4-feature-wise-implementation)
5. [Important Functions](#5-important-functions)
6. [Socket Programming Details](#6-socket-programming-details)
7. [Concurrency & Thread Safety](#7-concurrency--thread-safety)
8. [File Operations & Persistence](#8-file-operations--persistence)

---

## 1. System Architecture Overview

### Component Design
```
┌─────────────┐         ┌──────────────┐         ┌─────────────────┐
│   Client    │◄───────►│ Name Server  │◄───────►│ Storage Server  │
│  (Multiple) │   TCP   │   (Single)   │   TCP   │   (Multiple)    │
└─────────────┘         └──────────────┘         └─────────────────┘
                              │
                              ▼
                        ┌──────────┐
                        │ Journal  │
                        │Checkpoint│
                        └──────────┘
```

### Why This Architecture?
- **Centralized Name Server**: Single source of truth for metadata and file locations
- **Distributed Storage**: Load balancing and scalability across multiple storage servers
- **Direct Client-SS Connection**: Reduces Name Server bottleneck for data transfer operations
- **TCP Protocol**: Ensures reliable, ordered delivery of messages

---

## 2. Key Data Structures

### 2.1 Storage Server Structure
**Location**: `src/nameserver/nameserver.c` (Lines 15-22)

```c
struct StorageServer {
    char ip[16];                // IP address of storage server
    int ns_port;                // Port for NS-to-SS communication
    int client_port;            // Port for Client-to-SS communication
    int ss_id;                  // Unique identifier
    int is_alive;               // Health status (1=alive, 0=down)
    time_t last_heartbeat;      // Last successful heartbeat timestamp
};
```

**Purpose**: Tracks all registered storage servers with their connection details and health status.

**Why This Design?**
- Separate ports for NS and Client connections allow concurrent operations
- `is_alive` flag enables fault tolerance (bonus feature)
- `ss_id` provides unique identification for logging and debugging

---

### 2.2 File Information Structure
**Location**: `src/nameserver/nameserver.c` (Lines 47-71)

```c
struct FileInfo {
    char filename[100];                           // Base filename
    char full_path[MAX_PATH_LENGTH];              // Full path (e.g., "folder1/file.txt")
    int is_folder;                                // 1=folder, 0=file
    int ss_index;                                 // Primary storage server index
    int ss_replicas[MAX_REPLICAS];                // Replica storage servers
    int replica_count;                            // Number of replicas
    char owner[100];                              // File owner username
    char access_list[MAX_ACCESS_USERS][100];     // Users with access
    int access_permissions[MAX_ACCESS_USERS];     // 1=READ, 2=WRITE
    int access_count;                             // Number of users in ACL
    time_t created_time;                          // Creation timestamp
    time_t modified_time;                         // Last modification time
    time_t accessed_time;                         // Last access time
    char last_accessed_by[100];                   // Username who last accessed
    long file_size;                               // File size in bytes
    struct AccessRequest requests[MAX_ACCESS_REQUESTS];  // Pending access requests
    int request_count;                            // Number of pending requests
};
```

**Purpose**: Complete metadata for each file/folder in the system.

**Why This Design?**
- **full_path**: Supports hierarchical folder structure (bonus feature)
- **is_folder**: Distinguishes folders from files without separate structures
- **Access Control Lists**: Array-based for simple permission checking
- **Replicas**: Supports fault tolerance through replication
- **Timestamps**: Required for INFO command and auditing
- **Access Requests**: Supports bonus feature for requesting file access

---

### 2.3 Trie Node Structure
**Location**: `src/nameserver/nameserver.c` (Lines 74-79)

```c
#define TRIE_CHILDREN 128  // ASCII characters

typedef struct TrieNode {
    struct TrieNode* children[TRIE_CHILDREN];  // Pointers to child nodes
    int file_index;                             // Index in file_list array
    int is_end;                                 // 1 if end of filename
} TrieNode;
```

**Purpose**: Efficient file lookup with O(k) complexity where k = filename length.

**Why Trie?**
- **Fast Search**: O(k) lookup vs O(N) linear search
- **Prefix Matching**: Enables potential autocomplete features
- **Memory Efficient**: Shared prefixes reduce space
- **Better than Hash Table**: Supports range queries and prefix searches

**Trade-off**: More memory per node but significantly faster lookups for large file systems.

---

### 2.4 LRU Cache Structure
**Location**: `src/nameserver/nameserver.c` (Lines 83-90)

```c
#define LRU_CACHE_SIZE 20

typedef struct CacheNode {
    char filename[100];
    int file_index;
    struct CacheNode* prev;  // Doubly-linked list for O(1) removal
    struct CacheNode* next;
} CacheNode;

typedef struct {
    CacheNode* head;
    CacheNode* tail;
    int size;
    pthread_mutex_t lock;  // Thread-safe cache operations
} LRUCache;
```

**Purpose**: Cache recently accessed files for O(1) repeat lookups.

**Why LRU Cache?**
- **Locality Principle**: 80% of requests access 20% of files
- **O(1) Operations**: Doubly-linked list enables constant-time add/remove
- **Automatic Eviction**: Least recently used items removed when cache full
- **Thread-Safe**: Mutex protects cache from concurrent modifications

**Real-world Impact**: Reduces Trie lookups for frequently accessed files.

---

### 2.5 File Lock Structure (Sentence-Level Locking)
**Location**: `src/storageserver/storageserver.c` (Lines 11-19)

```c
#define MAX_LOCKS 100

typedef struct {
    char filename[100];
    int sentence_num;        // Which sentence is locked
    pthread_mutex_t lock;    // POSIX mutex for the sentence
    int in_use;              // 1 if lock entry is active
} FileLock;

// Global lock table
FileLock lock_table[MAX_LOCKS];
pthread_mutex_t lock_table_mutex = PTHREAD_MUTEX_INITIALIZER;
```

**Purpose**: Enable fine-grained concurrent editing at sentence level.

**Why Sentence-Level Locking?**
- **Concurrency**: Multiple users can edit different sentences simultaneously
- **Granularity Balance**: More concurrent than file-level, simpler than word-level
- **Two-Phase Locking**: 
  1. Acquire lock on lock_table_mutex to find/create sentence lock
  2. Acquire lock on specific sentence mutex for actual editing

**Example Flow**:
```
User A: WRITE file.txt 0  → Locks sentence 0
User B: WRITE file.txt 1  → Locks sentence 1 (succeeds, different sentence)
User C: WRITE file.txt 0  → Blocks until User A releases sentence 0
```

---

### 2.6 Checkpoint Structure
**Location**: `src/storageserver/storageserver.c` (Lines 23-29)

```c
typedef struct {
    char filename[256];
    char tag[MAX_TAG_LENGTH];           // User-defined tag (e.g., "v1", "draft")
    char checkpoint_path[300];          // Path to .ckpt file
    time_t created_time;                // When checkpoint was created
    char created_by[100];               // Username who created it
} CheckpointInfo;

CheckpointInfo checkpoint_registry[1000];
int checkpoint_count = 0;
pthread_mutex_t checkpoint_mutex = PTHREAD_MUTEX_INITIALIZER;
```

**Purpose**: Track all file checkpoints for version control (bonus feature).

**Why This Design?**
- **Tag-based**: Users create meaningful labels ("final_draft", "v2.1")
- **Metadata Tracking**: Know who created checkpoint and when
- **File-based Storage**: Checkpoint stored as `filename.tag.ckpt` on disk
- **Thread-Safe**: Mutex protects checkpoint_registry from concurrent updates

---

## 3. Synchronization Mechanisms

### 3.1 Mutexes Used

| Mutex Name | Location | Purpose | Scope |
|------------|----------|---------|-------|
| `ss_list_mutex` | nameserver.c:96 | Protects storage server list | Global |
| `client_list_mutex` | nameserver.c:101 | Protects connected client list | Global |
| `file_list_mutex` | nameserver.c:106 | Protects file metadata array | Global |
| `trie_mutex` | nameserver.c:110 | Protects Trie operations | Global |
| `journal_mutex` | nameserver.c:115 | Protects journal write operations | Global |
| `lock_table_mutex` | storageserver.c:33 | Protects sentence lock table | Global |
| `checkpoint_mutex` | storageserver.c:38 | Protects checkpoint registry | Global |
| `FileLock.lock` | storageserver.c:17 | Protects individual sentence | Per-sentence |
| `LRUCache.lock` | nameserver.c:90 | Protects cache operations | Per-cache |

### 3.2 Why POSIX Mutexes?
- **Standard**: Cross-platform POSIX compliance
- **Blocking**: Threads wait (no busy-waiting) → CPU efficient
- **Priority Inheritance**: Prevents priority inversion issues
- **Simple API**: `pthread_mutex_lock()`, `pthread_mutex_unlock()`

### 3.3 Critical Sections Protected

#### Example 1: Adding New Storage Server
**Location**: `src/nameserver/nameserver.c` (Lines 506-563)

```c
pthread_mutex_lock(&ss_list_mutex);

if (ss_count >= 10) {
    pthread_mutex_unlock(&ss_list_mutex);
    // Send error
    return;
}

// Add new storage server to ss_list[]
strncpy(ss_list[ss_count].ip, ss_ip, sizeof(ss_list[ss_count].ip) - 1);
ss_list[ss_count].ns_port = ss_ns_port;
// ... more initialization ...
ss_count++;

pthread_mutex_unlock(&ss_list_mutex);
```

**Why Mutex Here?**
- Multiple storage servers can register simultaneously
- Prevents race condition where two SSs get same ss_id
- Ensures ss_count increment is atomic

#### Example 2: Sentence Lock Acquisition
**Location**: `src/storageserver/storageserver.c` (Lines 155-183)

```c
pthread_mutex_lock(&lock_table_mutex);

// Find existing lock for this (filename, sentence_num)
for (int i = 0; i < MAX_LOCKS; i++) {
    if (lock_table[i].in_use && 
        strcmp(lock_table[i].filename, filename) == 0 &&
        lock_table[i].sentence_num == sent_num) {
        
        pthread_mutex_unlock(&lock_table_mutex);
        pthread_mutex_lock(&lock_table[i].lock);  // Acquire sentence lock
        return 0;
    }
}

// Create new lock if not found
for (int i = 0; i < MAX_LOCKS; i++) {
    if (!lock_table[i].in_use) {
        // Initialize new lock entry
        strncpy(lock_table[i].filename, filename, ...);
        lock_table[i].sentence_num = sent_num;
        pthread_mutex_init(&lock_table[i].lock, NULL);
        lock_table[i].in_use = 1;
        
        pthread_mutex_unlock(&lock_table_mutex);
        pthread_mutex_lock(&lock_table[i].lock);  // Acquire sentence lock
        return 0;
    }
}

pthread_mutex_unlock(&lock_table_mutex);
```

**Why Two-Phase Locking?**
1. **Phase 1**: Hold `lock_table_mutex` while searching/creating lock entry
2. **Phase 2**: Release `lock_table_mutex`, acquire sentence-specific lock
3. **Benefit**: Minimizes time holding global mutex, allows parallel sentence edits

---

## 4. Feature-wise Implementation

### 4.1 VIEW Command (10 marks)
**Location**: `src/nameserver/nameserver.c` (Lines 1036-1142)

#### Implementation Flow:
1. Parse command: `VIEW`, `VIEW -a`, `VIEW -l`, `VIEW -al`
2. Acquire `file_list_mutex`
3. Iterate through `file_list[]`
4. Filter based on flags:
   - No flags: Show only files user has access to
   - `-a`: Show all files
   - `-l`: Include details (word count, size, timestamps, owner)
5. Format and send response to client

#### Key Functions:
```c
// Check if user has access to file
int has_file_access(struct FileInfo* file, const char* username) {
    if (strcmp(file->owner, username) == 0) return 1;  // Owner always has access
    
    for (int i = 0; i < file->access_count; i++) {
        if (strcmp(file->access_list[i], username) == 0) {
            return 1;
        }
    }
    return 0;
}
```

#### Why This Approach?
- **Centralized**: NS has all metadata, no SS queries needed
- **O(N) Complexity**: Acceptable for MVP with moderate file count
- **Flag Parsing**: Unix-style interface familiar to users

---

### 4.2 CREATE Command (10 marks)
**Location**: `src/nameserver/nameserver.c` (Lines 1144-1229)

#### Implementation Flow:
```
Client → NS: CREATE filename
         ↓
NS: Check if file exists (Trie lookup)
         ↓
NS: Select SS using round-robin
         ↓
NS → SS: CREATE_FILE filename
         ↓
SS: Create empty file on disk
         ↓
SS → NS: ACK_SUCCESS
         ↓
NS: Add to file_list[], update Trie, journal operation
         ↓
NS → Client: ACK_SUCCESS
```

#### Key Functions:
```c
// Round-robin load balancing
int next_ss_index = (last_ss_index + 1) % ss_count;
last_ss_index = next_ss_index;
```

#### Why Round-Robin?
- **Simple**: No complex load metrics needed
- **Fair**: Even distribution across all SSs
- **O(1)**: Instant selection
- **Stateless**: Only need last_ss_index counter

**Alternative Considered**: Least-loaded (requires continuous monitoring) - rejected for simplicity.

---

### 4.3 WRITE Command (30 marks)
**Location**: Client: `client.c` (Lines 199-341), SS: `storageserver.c` (Lines 598-723)

#### Implementation Flow:
```
Client → NS: WRITE filename sentence_num
         ↓
NS: Check write access
         ↓
NS → Client: REDIRECT <SS_IP> <SS_PORT>
         ↓
Client → SS: WRITE_SENTENCE filename sentence_num
         ↓
SS: Lock sentence (blocks if locked by another user)
         ↓
Client → SS: <word_index> <content> (multiple times)
         ↓
SS: Update sentence in memory buffer
         ↓
Client → SS: ETIRW (signals end)
         ↓
SS: Write buffer to .tmp file, rename to original (atomic), unlock
         ↓
SS → Client: ACK_SUCCESS
```

#### Sentence Update Function:
**Location**: `src/storageserver/storageserver.c` (Lines 265-430)

```c
void update_sentence(char* filename, int sent_num, int word_index, char* new_content) {
    // Read entire file
    FILE* file = fopen(filename, "r");
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* file_content = malloc(file_size + 1);
    fread(file_content, 1, file_size, file);
    file_content[file_size] = '\0';
    fclose(file);
    
    // Extract target sentence
    char* sentence = get_sentence(file_content, sent_num);
    
    // Tokenize sentence into words
    char* words[1000];
    int word_count = 0;
    char* token = strtok(sentence, " \t\n");
    while (token != NULL) {
        words[word_count++] = strdup(token);
        token = strtok(NULL, " \t\n");
    }
    
    // Insert/replace at word_index
    if (word_index <= word_count) {
        // Shift words right, insert new_content
        for (int i = word_count; i > word_index; i--) {
            words[i] = words[i-1];
        }
        words[word_index] = strdup(new_content);
        word_count++;
    }
    
    // Rebuild sentence
    char new_sentence[MAX_MSG_SIZE];
    new_sentence[0] = '\0';
    for (int i = 0; i < word_count; i++) {
        strcat(new_sentence, words[i]);
        if (i < word_count - 1) strcat(new_sentence, " ");
    }
    
    // Replace sentence in file_content
    // ... (reconstruct entire file with updated sentence) ...
    
    // Write to .tmp file atomically
    FILE* tmp_file = fopen(".tmp", "w");
    fwrite(updated_content, 1, strlen(updated_content), tmp_file);
    fclose(tmp_file);
    
    // Atomic rename
    rename(".tmp", filename);
}
```

#### Why Sentence Delimiters (. ! ?) Even in Middle of Words?
**Specification Requirement**: Every period is a sentence delimiter, including "e.g." which becomes two sentences: "e" and "g".

**Acknowledged Trade-off**: Non-ideal but ensures unambiguous parsing rules.

#### Why ETIRW?
- **Unique**: "WRITE" spelled backwards, unlikely to be actual content
- **Clear Signal**: Marks end of multi-word update session
- **No Ambiguity**: Unlike "END" or "DONE" which might appear in content

---

### 4.4 UNDO Command (15 marks)
**Location**: `src/storageserver/storageserver.c` (Lines 887-969)

#### Implementation:
```c
// Before every WRITE operation
char backup_path[300];
snprintf(backup_path, sizeof(backup_path), "%s.bak", filepath);
system("cp filepath filepath.bak");  // Create backup

// On UNDO command
void handle_undo(char* filename) {
    char backup_path[300];
    snprintf(backup_path, sizeof(backup_path), "%s.bak", filename);
    
    if (access(backup_path, F_OK) != 0) {
        send_error("ERROR: No previous version to undo");
        return;
    }
    
    // Restore from backup
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "cp %s.bak %s", filename, filename);
    system(cmd);
    
    send_success("UNDO successful");
}
```

#### Why Single-Level Undo?
- **Specification**: Only one undo level required
- **Simplicity**: Avoids version chain management
- **Storage Efficient**: Only one .bak file per document
- **Sufficient**: Meets requirement without overengineering

**Alternative Rejected**: Full version history (Git-style) is complex and out of scope.

---

### 4.5 STREAM Command (15 marks)
**Location**: `src/storageserver/storageserver.c` (Lines 971-1068)

#### Implementation:
```c
void handle_stream(int client_fd, char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        send_error(client_fd, "ERROR: File not found");
        return;
    }
    
    char word[256];
    while (fscanf(file, "%s", word) == 1) {
        // Send word to client
        write(client_fd, word, strlen(word));
        write(client_fd, " ", 1);  // Space separator
        
        // 0.1 second delay
        usleep(100000);  // 100,000 microseconds = 0.1 seconds
        
        // Check if client disconnected
        char test[1];
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        if (recv(client_fd, test, 1, MSG_PEEK) == 0) {
            // Client disconnected
            break;
        }
        fcntl(client_fd, F_SETFL, flags);
    }
    
    fclose(file);
    send_stop_signal(client_fd);
}
```

#### Why Direct SS Connection?
- **Real-time**: NS proxy would buffer and delay streaming effect
- **Bandwidth**: Doesn't burden NS for large file streaming
- **Disconnect Detection**: Client can detect SS failure immediately

#### Why usleep(100000)?
- **Specification**: 0.1 second = 100,000 microseconds
- **User Experience**: Creates visible word-by-word effect
- **System Call**: `usleep()` puts thread to sleep, doesn't busy-wait

---

### 4.6 EXEC Command (15 marks)
**Location**: `src/nameserver/nameserver.c` (Lines 1540-1627)

#### Implementation Flow:
```
Client → NS: EXEC filename
         ↓
NS → SS: READ_FILE filename
         ↓
SS → NS: <file_content>
         ↓
NS: popen(file_content, "r")  ← Execute as shell commands
         ↓
NS: Capture stdout/stderr
         ↓
NS → Client: <command_output>
```

#### Key Code:
```c
void handle_exec(int client_fd, char* filename, char* username) {
    // Get file content from SS
    char file_content[MAX_MSG_SIZE];
    int result = request_file_from_ss(filename, file_content);
    
    if (result < 0) {
        send_error(client_fd, "ERROR: Failed to read file");
        return;
    }
    
    // Execute commands using popen
    FILE* pipe = popen(file_content, "r");
    if (!pipe) {
        send_error(client_fd, "ERROR: Execution failed");
        return;
    }
    
    // Read and send output
    char output[MAX_MSG_SIZE];
    char line[1024];
    output[0] = '\0';
    while (fgets(line, sizeof(line), pipe) != NULL) {
        strcat(output, line);
    }
    
    pclose(pipe);
    write(client_fd, output, strlen(output));
}
```

#### Why NS Executes (Not SS)?
- **Specification**: "execution must happen on name server"
- **Security Centralization**: Only one component needs execution privileges
- **Resource Isolation**: Prevents untrusted scripts on storage servers

**Security Caveat**: `popen()` executes arbitrary shell commands. In production, would use sandboxing (containers, seccomp filters).

---

### 4.7 Access Control (15 marks)
**Location**: `src/nameserver/nameserver.c` (Lines 1629-1820)

#### Data Structure:
```c
struct FileInfo {
    // ...
    char access_list[MAX_ACCESS_USERS][100];     // Usernames
    int access_permissions[MAX_ACCESS_USERS];     // 1=READ, 2=WRITE
    int access_count;
};
```

#### Permission Checking:
```c
int has_write_access(struct FileInfo* file, const char* username) {
    // Owner always has write access
    if (strcmp(file->owner, username) == 0) return 1;
    
    // Check access list
    for (int i = 0; i < file->access_count; i++) {
        if (strcmp(file->access_list[i], username) == 0) {
            return (file->access_permissions[i] >= 2);  // 2=WRITE
        }
    }
    return 0;
}
```

#### Commands:
- `ADDACCESS -R filename username`: Grant read access
- `ADDACCESS -W filename username`: Grant write access (implies read)
- `REMACCESS filename username`: Revoke all access

#### Why Array-Based ACL?
- **Simple**: No complex data structures
- **Fast Lookups**: O(N) but N ≤ 10 users per file
- **Persistent**: Easy to serialize to journal
- **Sufficient**: Most files have few collaborators

**Alternative Rejected**: Bitmap permissions (Unix-style) less human-readable, requires fixed user IDs.

---

## 5. Important Functions

### 5.1 Trie Operations

#### Insert Function
**Location**: `src/nameserver/nameserver.c` (Lines 277-303)

```c
void trie_insert(TrieNode* root, const char* filename, int file_index) {
    pthread_mutex_lock(&trie_mutex);
    
    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        int index = (unsigned char)filename[i];
        
        if (index >= TRIE_CHILDREN || index < 0) {
            pthread_mutex_unlock(&trie_mutex);
            return;  // Invalid character
        }
        
        if (current->children[index] == NULL) {
            current->children[index] = (TrieNode*)malloc(sizeof(TrieNode));
            memset(current->children[index], 0, sizeof(TrieNode));
            current->children[index]->file_index = -1;
        }
        current = current->children[index];
    }
    
    current->is_end = 1;
    current->file_index = file_index;
    
    pthread_mutex_unlock(&trie_mutex);
}
```

**Complexity**: O(k) where k = filename length
**Thread Safety**: Protected by `trie_mutex`

#### Search Function
**Location**: `src/nameserver/nameserver.c` (Lines 306-325)

```c
int trie_search(TrieNode* root, const char* filename) {
    pthread_mutex_lock(&trie_mutex);
    
    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        int index = (unsigned char)filename[i];
        
        if (index >= TRIE_CHILDREN || current->children[index] == NULL) {
            pthread_mutex_unlock(&trie_mutex);
            return -1;  // Not found
        }
        current = current->children[index];
    }
    
    int result = (current->is_end) ? current->file_index : -1;
    pthread_mutex_unlock(&trie_mutex);
    return result;
}
```

**Why Mutex for Read?**
- Concurrent inserts can modify Trie structure during search
- Read lock would be more efficient (pthread_rwlock_t) but adds complexity
- For MVP, simple mutex is acceptable

---

### 5.2 LRU Cache Operations

#### Get Function
**Location**: `src/nameserver/nameserver.c` (Lines 361-377)

```c
int lru_get(LRUCache* cache, const char* filename) {
    pthread_mutex_lock(&cache->lock);
    
    CacheNode* current = cache->head;
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0) {
            // Cache hit! Move to front (most recently used)
            move_to_front(cache, current);
            int file_index = current->file_index;
            pthread_mutex_unlock(&cache->lock);
            return file_index;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&cache->lock);
    return -1;  // Cache miss
}
```

#### Put Function
**Location**: `src/nameserver/nameserver.c` (Lines 379-421)

```c
void lru_put(LRUCache* cache, const char* filename, int file_index) {
    pthread_mutex_lock(&cache->lock);
    
    // Check if already in cache
    CacheNode* current = cache->head;
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0) {
            current->file_index = file_index;
            move_to_front(cache, current);
            pthread_mutex_unlock(&cache->lock);
            return;
        }
        current = current->next;
    }
    
    // New entry
    CacheNode* new_node = (CacheNode*)malloc(sizeof(CacheNode));
    strncpy(new_node->filename, filename, 99);
    new_node->file_index = file_index;
    new_node->next = cache->head;
    new_node->prev = NULL;
    
    if (cache->head != NULL) {
        cache->head->prev = new_node;
    }
    cache->head = new_node;
    if (cache->tail == NULL) {
        cache->tail = new_node;
    }
    cache->size++;
    
    // Evict if cache full
    if (cache->size > LRU_CACHE_SIZE) {
        CacheNode* lru = cache->tail;
        remove_node(cache, lru);
        free(lru);
        cache->size--;
    }
    
    pthread_mutex_unlock(&cache->lock);
}
```

**Why Doubly-Linked List?**
- O(1) removal from middle (need prev pointer)
- O(1) move to front (need both prev and next)
- LRU eviction at tail is O(1)

---

### 5.3 Persistence Functions

#### Journal Operation
**Location**: `src/nameserver/nameserver.c` (Lines 792-817)

```c
void journal_operation(const char* operation) {
    pthread_mutex_lock(&journal_mutex);
    
    FILE* journal = fopen(JOURNAL_FILE, "a");  // Append mode
    if (!journal) {
        pthread_mutex_unlock(&journal_mutex);
        return;
    }
    
    // Write timestamped entry
    time_t now = time(NULL);
    fprintf(journal, "[%ld] %s\n", now, operation);
    fflush(journal);  // Force write to disk
    fclose(journal);
    
    pthread_mutex_unlock(&journal_mutex);
    
    operation_count++;
    if (operation_count % CHECKPOINT_INTERVAL == 0) {
        create_checkpoint();  // Periodic checkpoint
    }
}
```

**Why Append-Only?**
- **Fast Writes**: No seek operations
- **Crash Recovery**: Partial writes still readable
- **Audit Trail**: Complete history preserved

#### Checkpoint Creation
**Location**: `src/nameserver/nameserver.c` (Lines 819-865)

```c
void create_checkpoint() {
    pthread_mutex_lock(&file_list_mutex);
    
    FILE* ckpt = fopen(CHECKPOINT_FILE, "wb");  // Binary write
    if (!ckpt) {
        pthread_mutex_unlock(&file_list_mutex);
        return;
    }
    
    // Write file count
    fwrite(&file_count, sizeof(int), 1, ckpt);
    
    // Write all file metadata
    for (int i = 0; i < file_count; i++) {
        fwrite(&file_list[i], sizeof(struct FileInfo), 1, ckpt);
    }
    
    fclose(ckpt);
    pthread_mutex_unlock(&file_list_mutex);
    
    printf("Checkpoint created with %d files\n", file_count);
}
```

**Why Binary Format?**
- **Fast**: No parsing required on recovery
- **Compact**: Smaller than text format
- **Exact**: Preserves all struct fields including padding

#### Recovery Function
**Location**: `src/nameserver/nameserver.c` (Lines 867-1036)

```c
void recover_from_persistence() {
    // Step 1: Load checkpoint (full state snapshot)
    FILE* ckpt = fopen(CHECKPOINT_FILE, "rb");
    if (ckpt) {
        fread(&file_count, sizeof(int), 1, ckpt);
        for (int i = 0; i < file_count; i++) {
            fread(&file_list[i], sizeof(struct FileInfo), 1, ckpt);
            // Rebuild Trie
            trie_insert(trie_root, file_list[i].filename, i);
        }
        fclose(ckpt);
    }
    
    // Step 2: Replay journal (operations after checkpoint)
    FILE* journal = fopen(JOURNAL_FILE, "r");
    if (journal) {
        char line[1024];
        while (fgets(line, sizeof(line), journal) != NULL) {
            // Parse and replay: CREATE, DELETE, ADDACCESS, etc.
            replay_operation(line);
        }
        fclose(journal);
    }
}
```

**Why Checkpoint + Journal?**
- **Fast Recovery**: Load checkpoint (O(N)) + replay journal tail (O(M))
- **Trade-off**: Space (two files) vs Speed (fast recovery)
- **Production Pattern**: Used by databases (WAL + Snapshots)

---

## 6. Socket Programming Details

### 6.1 TCP Server Creation
**Location**: `src/common/socket_utils.c` (Lines 8-52)

```c
int create_tcp_server(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;  // Listen on all interfaces
    address.sin_port = htons(port);
    
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        close(server_fd);
        return -1;
    }
    
    if (listen(server_fd, 10) < 0) {  // Backlog of 10 connections
        perror("Listen failed");
        close(server_fd);
        return -1;
    }
    
    printf("Server listening on port %d\n", port);
    return server_fd;
}
```

**Key Points**:
- `SO_REUSEADDR`: Allows immediate rebind after crash (avoids "Address already in use")
- `SOCK_STREAM`: TCP (vs `SOCK_DGRAM` for UDP)
- `listen(10)`: Queue up to 10 pending connections

### 6.2 Client Connection
**Location**: `src/common/socket_utils.c` (Lines 76-110)

```c
int connect_to_server(const char* ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(sock);
        return -1;
    }
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sock);
        return -1;
    }
    
    return sock;
}
```

**Why TCP?**
- **Reliable**: Automatic retransmission of lost packets
- **Ordered**: Packets arrive in order sent
- **Connection-Oriented**: Established connection for multiple messages
- **Flow Control**: Prevents overwhelming receiver

**Alternative Rejected**: UDP (faster but unreliable, requires manual ordering/retransmission).

---

## 7. Concurrency & Thread Safety

### 7.1 Thread Creation Pattern

#### Name Server - Client Handler
**Location**: `src/nameserver/nameserver.c` (Lines 712-730)

```c
while (1) {
    int* client_fd = malloc(sizeof(int));
    *client_fd = accept_client(ns_fd);
    
    if (*client_fd < 0) {
        free(client_fd);
        continue;
    }
    
    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, handle_client_connection, client_fd) != 0) {
        perror("Thread creation failed");
        close(*client_fd);
        free(client_fd);
    } else {
        pthread_detach(thread_id);  // Auto-cleanup when thread exits
    }
}
```

**Why Detached Threads?**
- No need to `pthread_join()` (cleanup happens automatically)
- Suitable for long-running servers with many short-lived connections
- Prevents thread handle leaks

**Why malloc(client_fd)?**
- Each thread needs its own copy
- Stack variable would be overwritten in next loop iteration
- Thread frees the memory when done

### 7.2 Deadlock Prevention

#### Lock Ordering Convention
```
Global mutexes acquired in this order:
1. ss_list_mutex
2. client_list_mutex
3. file_list_mutex
4. trie_mutex
5. journal_mutex
```

**Why Ordering Matters?**
```
Thread A: Lock(mutex1) → Lock(mutex2)
Thread B: Lock(mutex2) → Lock(mutex1)
         ↓
      DEADLOCK!
```

**Our Solution**: Always acquire mutexes in same order → prevents circular wait.

### 7.3 Race Condition Example (Prevented)

**Scenario**: Two clients create file with same name simultaneously

**Without Mutex**:
```
Thread A: Check if "file.txt" exists → NO
Thread B: Check if "file.txt" exists → NO
Thread A: Add "file.txt" to file_list[0]
Thread B: Add "file.txt" to file_list[1]
         ↓
      DUPLICATE FILE ENTRIES!
```

**With Mutex**:
```c
pthread_mutex_lock(&file_list_mutex);
// Check if exists
for (int i = 0; i < file_count; i++) {
    if (strcmp(file_list[i].filename, filename) == 0) {
        pthread_mutex_unlock(&file_list_mutex);
        send_error("ERROR: File already exists");
        return;
    }
}
// Add file
file_list[file_count++] = new_file;
pthread_mutex_unlock(&file_list_mutex);
```

**Result**: Only one thread can check and add, atomically.

---

## 8. File Operations & Persistence

### 8.1 Atomic File Updates

**Problem**: Power loss during file write → corrupted file

**Solution**: Write-and-Rename Pattern
```c
// Write to temporary file
FILE* tmp = fopen("file.txt.tmp", "w");
fwrite(new_content, 1, strlen(new_content), tmp);
fclose(tmp);

// Atomic rename (POSIX guarantee)
rename("file.txt.tmp", "file.txt");
```

**Why Atomic?**
- `rename()` is atomic in POSIX systems
- Old file only replaced if new file complete
- Prevents partial writes visible to readers

### 8.2 Sentence Delimiter Handling

**Input**: `"Im just a mouse. I dont like PNS"`

**Parsing**:
```c
int count_complete_sentences(char* file_content) {
    int count = 0;
    for (int i = 0; file_content[i] != '\0'; i++) {
        if (file_content[i] == '.' || 
            file_content[i] == '!' || 
            file_content[i] == '?') {
            count++;
        }
    }
    return count;
}
```

**Result**:
- Sentence 0: "Im just a mouse."
- Sentence 1: "I dont like PNS"

**Edge Case**: "e.g." creates two sentences:
- Sentence 0: "e."
- Sentence 1: "g."

**Acknowledged**: Non-ideal but matches specification exactly.

---

## 9. System Requirements Implementation

### 9.1 Error Handling
**Strategy**: Consistent error format across all components

```c
// Error codes (implicit in messages)
"ERROR: File not found"
"ERROR: Access denied"
"ERROR: Sentence locked by another user"
"ERROR: Sentence index out of range"
"ERROR: Previous sentence incomplete"
```

**Why String-Based?**
- Human-readable (easier debugging)
- No need to maintain error code tables
- Flexible (can add context details)

### 9.2 Logging
**Implementation**: Timestamp + Component + Action format

```c
printf("[%s] Client %s: Command=%s File=%s Result=%s\n", 
       timestamp, username, command, filename, result);
```

**Example Output**:
```
[2025-12-04 15:23:45] Client alice: Command=WRITE File=report.txt Result=SUCCESS
[2025-12-04 15:24:12] Storage Server 1: File created: report.txt.bak
[2025-12-04 15:24:15] Client bob: Command=READ File=report.txt Result=REDIRECT SS1
```

---

## 10. Bonus Features Summary

| Feature | Points | Implementation Location | Key Technique |
|---------|--------|------------------------|---------------|
| **Hierarchical Folders** | 10 | nameserver.c:1072-1968 | Path-based (full_path field), Trie integration |
| **Checkpoints** | 15 | storageserver.c:728-1186 | File-based (.ckpt), tag system, metadata registry |
| **Fault Tolerance** | 15 | nameserver.c:212-244 | Replica tracking, heartbeat detection, failover |
| **Access Requests** | 5 | nameserver.c (AccessRequest struct) | Queue-based pending requests |
| **Unique Factor** | 5 | STREAM, EXEC, Sentence locking | Word-by-word streaming, remote execution |

---

## 11. Performance Characteristics

| Operation | Time Complexity | Space Complexity | Concurrency Level |
|-----------|----------------|------------------|-------------------|
| File Search (Trie) | O(k) | O(ALPHABET × N) | Thread-safe (mutex) |
| File Search (Cache hit) | O(1) | O(CACHE_SIZE) | Thread-safe (mutex) |
| File Creation | O(k) + O(1) | O(1) | Full (per-file) |
| Sentence Lock | O(1) amortized | O(MAX_LOCKS) | Fine-grained (per-sentence) |
| Checkpoint Creation | O(N) | O(N) | Periodic (100 ops) |
| Recovery | O(N) + O(M) | O(N) | N/A (single-threaded) |

Where:
- k = filename length
- N = total number of files
- M = journal entries since last checkpoint

---

## 12. Common Viva Questions & Answers

### Q1: Why use Trie instead of Hash Table for file search?
**Answer**: 
- Trie provides O(k) lookup (k = filename length) which is effectively O(1) for bounded filenames
- Hash tables provide O(1) average but O(N) worst case (hash collisions)
- Trie supports prefix matching (potential autocomplete feature)
- Trie enables range queries (list all files starting with "doc")
- Memory overhead acceptable for educational project

### Q2: How do you handle concurrent writes to the same sentence?
**Answer**:
1. First writer acquires sentence-level mutex via `lock_sentence(filename, sent_num)`
2. Second writer blocks on `pthread_mutex_lock(&lock_table[i].lock)` call
3. First writer completes, releases lock via `unlock_sentence()`
4. Second writer acquires lock, sees updated sentence, applies their changes
5. Sentence updates are **sequential** but different sentences can be edited **concurrently**

### Q3: What happens if Name Server crashes?
**Answer**:
- **Recovery Process**:
  1. Restart Name Server
  2. Call `recover_from_persistence()`
  3. Load checkpoint file (last saved state)
  4. Replay journal (operations after checkpoint)
  5. Rebuild Trie from recovered file_list
  6. Wait for Storage Servers to reconnect
- **Data Loss**: Zero (if last checkpoint + journal intact)
- **Downtime**: Seconds to minutes depending on journal size

### Q4: Why ETIRW instead of a normal delimiter like "END"?
**Answer**:
- "END" could appear as actual file content
- "ETIRW" is "WRITE" spelled backwards - unique and memorable
- Extremely unlikely to appear in real text
- Clear signal for end of multi-word update session
- Alternatives like "<<<EOF>>>" are verbose and ugly

### Q5: How do you prevent deadlocks with multiple mutexes?
**Answer**:
- **Global Lock Ordering**: Always acquire mutexes in same order (ss_list → client_list → file_list → trie → journal)
- **Lock Hierarchies**: Fine-grained locks (sentence-level) acquired after global locks
- **Short Critical Sections**: Release locks quickly, minimize holding time
- **No Nested Locks**: Avoid acquiring multiple locks simultaneously when possible
- **Result**: No circular wait condition → deadlock impossible

### Q6: Why separate ports for NS-SS and Client-SS communication?
**Answer**:
- **Concurrent Operations**: NS can query SS metadata while client streams data
- **Security**: Different authentication/authorization for NS vs Client
- **Load Balancing**: NS traffic (control plane) doesn't interfere with client traffic (data plane)
- **Debugging**: Can monitor ports separately with `tcpdump` or Wireshark

### Q7: How does LRU Cache improve performance?
**Answer**:
- **Hot Files**: 80/20 rule - 80% of accesses hit 20% of files
- **Cache Hit**: O(1) lookup (no Trie traversal needed)
- **Cache Miss**: O(k) Trie lookup, add to cache for future hits
- **Eviction**: Least recently used removed when cache full
- **Real Impact**: Reduces average lookup from O(k) to near O(1)

### Q8: Why journal + checkpoint instead of just journaling?
**Answer**:
- **Recovery Speed**: 
  - Journal only: O(N) replay of all operations from start
  - Checkpoint + Journal: O(M) load checkpoint + O(K) replay recent journal (M + K << N)
- **Space**: Checkpoint is snapshot, journal grows unbounded
- **Trade-off**: More disk writes (checkpoint creation) for faster recovery
- **Production Pattern**: Used by PostgreSQL (WAL + Checkpoints), MongoDB (Oplog + Snapshots)

### Q9: How do you ensure sentence write atomicity?
**Answer**:
1. Acquire sentence lock (prevents concurrent writers)
2. Read entire file into memory
3. Extract target sentence
4. Apply word updates to sentence buffer
5. Reconstruct entire file with updated sentence
6. Write to temporary file: `filename.tmp`
7. Atomic rename: `rename("filename.tmp", "filename")`
8. Release sentence lock
9. **POSIX guarantee**: `rename()` is atomic, old file only replaced if new file complete

### Q10: What are the limitations of your implementation?
**Answer**:
- **Single Name Server**: No horizontal scaling, single point of failure
- **In-Memory Metadata**: File count limited by RAM (though acceptable for MVP)
- **Round-Robin Load Balancing**: Doesn't account for SS load or disk space
- **Sentence Delimiter**: Every period creates new sentence (e.g., "e.g." splits)
- **Single-Level Undo**: No full version history like Git
- **No Authentication**: Username is self-declared (trust-based)

---

## 13. Testing & Debugging

### Tools Used:
- **GDB**: Debugging segfaults and race conditions
- **Valgrind**: Memory leak detection
- **strace**: System call tracing
- **netstat**: Check open ports and connections
- **tcpdump/Wireshark**: Inspect network packets (TCP handshakes, data flow)

### Common Bugs Fixed:
1. **Race condition in file creation**: Fixed with `file_list_mutex`
2. **Memory leak in thread arguments**: Fixed with `free(client_fd)` in threads
3. **Deadlock in cache operations**: Fixed with consistent lock ordering
4. **Buffer overflow in WRITE**: Fixed with proper bounds checking
5. **Checkpoint corruption**: Fixed with binary write mode (`"wb"`)

---

## 14. Conclusion

This implementation demonstrates:
- **Distributed Systems**: Multi-component architecture with TCP communication
- **Concurrency**: Pthread-based multithreading with proper synchronization
- **File Systems**: Efficient search (Trie + Cache), persistence (Journal + Checkpoint)
- **Systems Programming**: Socket programming, POSIX APIs, signal handling
- **Software Engineering**: Modular design, error handling, logging

**Total Lines of Code**: ~5000+ lines across nameserver, storageserver, client

**Key Takeaways for Viva**:
1. Understand **why** each design decision was made (trade-offs)
2. Know **where** each feature is implemented (line numbers)
3. Explain **how** synchronization prevents race conditions
4. Describe **alternatives** considered and rejected
5. Acknowledge **limitations** and potential improvements

---

**Document Created**: December 4, 2025  
**Purpose**: Viva Voce Preparation - Technical Deep Dive  
**Coverage**: Complete implementation with functions, data structures, mutexes, and design rationale
