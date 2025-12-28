#define _XOPEN_SOURCE 700
#include "common.h"
#include "socket_utils.h"
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/socket.h>

// Persistence paths
#define JOURNAL_FILE "data/ns_journal.log"
#define CHECKPOINT_FILE "data/ns_checkpoint.dat"
#define CHECKPOINT_INTERVAL 100  // Create checkpoint every N operations

// Storage Server structure
struct StorageServer {
    char ip[16];
    int ns_port;      // Port for NS to SS communication
    int client_port;  // Port for client to SS communication
    int ss_id;        // Unique identifier for this storage server
    int is_alive;     // 1 if alive, 0 if detected as down
    time_t last_heartbeat;  // Last successful heartbeat timestamp
};

// Client structure
struct Client {
    char username[100];
    int fd;
};

// Access request structure
#define MAX_ACCESS_REQUESTS 20

struct AccessRequest {
    char username[100];     // Who is requesting access
    time_t request_time;    // When the request was made
    int active;             // 1 if pending, 0 if processed/deleted
};

// File information structure
#define MAX_ACCESS_USERS 10
#define MAX_PATH_LENGTH 256
#define MAX_CHECKPOINTS 10
#define MAX_TAG_LENGTH 50
#define MAX_REPLICAS 10

struct FileInfo {
    char filename[100];        // Base filename
    char full_path[MAX_PATH_LENGTH];  // Full path including folders (e.g., "folder1/file.txt")
    int is_folder;             // 1 if this is a folder, 0 if file
    int ss_index;              // Primary SS index into ss_list (for files only)
    int ss_replicas[MAX_REPLICAS];  // Indices of replica storage servers
    int replica_count;         // Number of replicas for this file
    char owner[100];           // File/folder owner username
    char access_list[MAX_ACCESS_USERS][100];  // Users with access
    int access_permissions[MAX_ACCESS_USERS];  // 1=READ, 2=WRITE
    int access_count;          // Number of users in access list
    time_t created_time;       // File creation timestamp
    time_t modified_time;      // Last modification timestamp
    time_t accessed_time;      // Last access timestamp
    char last_accessed_by[100];  // Username who last accessed
    long file_size;            // File size in bytes
    struct AccessRequest requests[MAX_ACCESS_REQUESTS];  // Pending access requests
    int request_count;         // Number of pending requests
};

// Trie Node for efficient filename lookup
#define TRIE_CHILDREN 128  // ASCII characters

typedef struct TrieNode {
    struct TrieNode* children[TRIE_CHILDREN];
    int file_index;  // Index in file_list, -1 if not a file endpoint
    int is_end;      // 1 if this is end of a filename
} TrieNode;

// LRU Cache Node
#define LRU_CACHE_SIZE 20

typedef struct CacheNode {
    char filename[100];
    int file_index;
    struct CacheNode* prev;
    struct CacheNode* next;
} CacheNode;

typedef struct {
    CacheNode* head;
    CacheNode* tail;
    int size;
    pthread_mutex_t lock;
} LRUCache;

// Global storage server list
struct StorageServer ss_list[10];
int ss_count = 0;
pthread_mutex_t ss_list_mutex;

// Global client list
struct Client client_list[50];
int client_count = 0;
pthread_mutex_t client_list_mutex;

// Global file list
struct FileInfo file_list[100];
int file_count = 0;
pthread_mutex_t file_list_mutex;

// Global Trie and LRU Cache
TrieNode* trie_root = NULL;
pthread_mutex_t trie_mutex;
LRUCache lru_cache;

// Persistence state
int journal_operation_count = 0;
pthread_mutex_t journal_mutex;

// Function prototypes
void* handle_ss_registration(void* arg);
void* handle_client_connection(void* arg);
void journal_operation(const char* operation);
void create_checkpoint();
void recover_from_persistence();
TrieNode* create_trie_node(void);
void trie_insert(TrieNode* root, const char* filename, int file_index);
int trie_search(TrieNode* root, const char* filename);
void init_lru_cache(LRUCache* cache);
int lru_get(LRUCache* cache, const char* filename);
void lru_put(LRUCache* cache, const char* filename, int file_index);
void* client_listener_thread(void* arg);
void* heartbeat_monitor_thread(void* arg);
int get_alive_ss_index(int exclude_index);
void replicate_file_to_ss(int ss_index, const char* filename, const char* source_ss_ip, int source_ss_port);

// ========== Heartbeat & Fault Tolerance Implementation ==========

#define HEARTBEAT_INTERVAL 5  // Send heartbeat every 5 seconds
#define HEARTBEAT_TIMEOUT 15  // Mark as dead if no response for 15 seconds

/**
 * Heartbeat monitoring thread
 * Periodically pings all storage servers to check if they're alive
 */
void* heartbeat_monitor_thread(void* arg) {
    (void)arg;  // Unused
    
    while (1) {
        sleep(HEARTBEAT_INTERVAL);
        
        pthread_mutex_lock(&ss_list_mutex);
        time_t now = time(NULL);
        
        for (int i = 0; i < ss_count; i++) {
            if (!ss_list[i].is_alive) {
                continue;  // Skip already dead servers
            }
            
            // Try to connect and send PING
            int ss_fd = connect_to_server(ss_list[i].ip, ss_list[i].ns_port);
            if (ss_fd < 0) {
                // Connection failed
                if (difftime(now, ss_list[i].last_heartbeat) > HEARTBEAT_TIMEOUT) {
                    printf("⚠ Storage Server %d (%s:%d) marked as DOWN (heartbeat timeout)\n",
                           ss_list[i].ss_id, ss_list[i].ip, ss_list[i].ns_port);
                    ss_list[i].is_alive = 0;
                }
                continue;
            }
            
            // Send PING command
            const char* ping_msg = "PING\n";
            write(ss_fd, ping_msg, strlen(ping_msg));
            
            // Wait for PONG response (with timeout)
            struct timeval tv;
            tv.tv_sec = 2;
            tv.tv_usec = 0;
            setsockopt(ss_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
            
            char response[64];
            memset(response, 0, sizeof(response));
            ssize_t bytes = read(ss_fd, response, sizeof(response) - 1);
            close(ss_fd);
            
            if (bytes > 0 && strncmp(response, "PONG", 4) == 0) {
                ss_list[i].last_heartbeat = now;
                if (!ss_list[i].is_alive) {
                    printf("✓ Storage Server %d (%s:%d) came back ONLINE\n",
                           ss_list[i].ss_id, ss_list[i].ip, ss_list[i].ns_port);
                    ss_list[i].is_alive = 1;
                    // TODO: Trigger synchronization
                }
            } else {
                // No response
                if (difftime(now, ss_list[i].last_heartbeat) > HEARTBEAT_TIMEOUT) {
                    printf("⚠ Storage Server %d (%s:%d) marked as DOWN (no PONG)\n",
                           ss_list[i].ss_id, ss_list[i].ip, ss_list[i].ns_port);
                    ss_list[i].is_alive = 0;
                }
            }
        }
        
        pthread_mutex_unlock(&ss_list_mutex);
    }
    
    return NULL;
}

/**
 * Get an alive storage server index, excluding a specific index
 * Returns -1 if no alive server found
 */
int get_alive_ss_index(int exclude_index) {
    pthread_mutex_lock(&ss_list_mutex);
    
    for (int i = 0; i < ss_count; i++) {
        if (i != exclude_index && ss_list[i].is_alive) {
            pthread_mutex_unlock(&ss_list_mutex);
            return i;
        }
    }
    
    pthread_mutex_unlock(&ss_list_mutex);
    return -1;
}

/**
 * Replicate a file from one SS to another
 * This is used for async replication during CREATE/WRITE operations
 */
void replicate_file_to_ss(int dest_ss_index, const char* filename, const char* source_ss_ip, int source_ss_port) {
    pthread_mutex_lock(&ss_list_mutex);
    if (dest_ss_index < 0 || dest_ss_index >= ss_count || !ss_list[dest_ss_index].is_alive) {
        pthread_mutex_unlock(&ss_list_mutex);
        return;
    }
    
    char dest_ip[16];
    int dest_port;
    strncpy(dest_ip, ss_list[dest_ss_index].ip, sizeof(dest_ip) - 1);
    dest_ip[sizeof(dest_ip) - 1] = '\0';
    dest_port = ss_list[dest_ss_index].ns_port;
    pthread_mutex_unlock(&ss_list_mutex);
    
    // Connect to destination SS
    int dest_fd = connect_to_server(dest_ip, dest_port);
    if (dest_fd < 0) {
        fprintf(stderr, "Replication failed: cannot connect to SS %d\n", dest_ss_index);
        return;
    }
    
    // Send REPLICATE command: "REPLICATE <filename> <source_ip> <source_port>"
    char replicate_cmd[MAX_MSG_SIZE];
    snprintf(replicate_cmd, sizeof(replicate_cmd), "REPLICATE %s %s %d\n", 
             filename, source_ss_ip, source_ss_port);
    write(dest_fd, replicate_cmd, strlen(replicate_cmd));
    
    // Don't wait for response (async replication)
    close(dest_fd);
    
    printf("→ Replication initiated: '%s' to SS %d\n", filename, dest_ss_index);
}

// ========== Trie Implementation ==========

TrieNode* create_trie_node() {
    TrieNode* node = (TrieNode*)malloc(sizeof(TrieNode));
    if (!node) return NULL;
    
    for (int i = 0; i < 128; i++) {
        node->children[i] = NULL;
    }
    node->file_index = -1;
    node->is_end = 0;
    return node;
}

void trie_insert(TrieNode* root, const char* filename, int file_index) {
    if (!root || !filename) return;
    
    pthread_mutex_lock(&trie_mutex);
    
    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        unsigned char c = (unsigned char)filename[i];
        if (c >= 128) {
            pthread_mutex_unlock(&trie_mutex);
            return; // Invalid character
        }
        
        if (current->children[c] == NULL) {
            current->children[c] = create_trie_node();
            if (current->children[c] == NULL) {
                pthread_mutex_unlock(&trie_mutex);
                return;
            }
        }
        current = current->children[c];
    }
    
    current->is_end = 1;
    current->file_index = file_index;
    
    pthread_mutex_unlock(&trie_mutex);
}

int trie_search(TrieNode* root, const char* filename) {
    if (!root || !filename) return -1;
    
    pthread_mutex_lock(&trie_mutex);
    
    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        unsigned char c = (unsigned char)filename[i];
        if (c >= 128 || current->children[c] == NULL) {
            pthread_mutex_unlock(&trie_mutex);
            return -1;
        }
        current = current->children[c];
    }
    
    int result = (current->is_end) ? current->file_index : -1;
    pthread_mutex_unlock(&trie_mutex);
    return result;
}

// ========== LRU Cache Implementation ==========

void init_lru_cache(LRUCache* cache) {
    cache->head = NULL;
    cache->tail = NULL;
    cache->size = 0;
    pthread_mutex_init(&cache->lock, NULL);
}

// Helper: Move node to front (most recently used)
void move_to_front(LRUCache* cache, CacheNode* node) {
    if (cache->head == node) return; // Already at front
    
    // Remove from current position
    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    if (cache->tail == node) cache->tail = node->prev;
    
    // Insert at front
    node->prev = NULL;
    node->next = cache->head;
    if (cache->head) cache->head->prev = node;
    cache->head = node;
    if (!cache->tail) cache->tail = node;
}

// Helper: Remove node from cache
void remove_node(LRUCache* cache, CacheNode* node) {
    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    if (cache->head == node) cache->head = node->next;
    if (cache->tail == node) cache->tail = node->prev;
    cache->size--;
}

int lru_get(LRUCache* cache, const char* filename) {
    pthread_mutex_lock(&cache->lock);
    
    CacheNode* current = cache->head;
    while (current) {
        if (strcmp(current->filename, filename) == 0) {
            int file_index = current->file_index;
            move_to_front(cache, current);
            pthread_mutex_unlock(&cache->lock);
            return file_index;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&cache->lock);
    return -1; // Not found
}

void lru_put(LRUCache* cache, const char* filename, int file_index) {
    pthread_mutex_lock(&cache->lock);
    
    // Check if already exists
    CacheNode* current = cache->head;
    while (current) {
        if (strcmp(current->filename, filename) == 0) {
            current->file_index = file_index;
            move_to_front(cache, current);
            pthread_mutex_unlock(&cache->lock);
            return;
        }
        current = current->next;
    }
    
    // Create new node
    CacheNode* new_node = (CacheNode*)malloc(sizeof(CacheNode));
    if (!new_node) {
        pthread_mutex_unlock(&cache->lock);
        return;
    }
    strncpy(new_node->filename, filename, 99);
    new_node->filename[99] = '\0';
    new_node->file_index = file_index;
    new_node->prev = NULL;
    new_node->next = cache->head;
    
    if (cache->head) cache->head->prev = new_node;
    cache->head = new_node;
    if (!cache->tail) cache->tail = new_node;
    cache->size++;
    
    // Evict LRU if cache is full
    if (cache->size > LRU_CACHE_SIZE) {
        CacheNode* lru = cache->tail;
        remove_node(cache, lru);
        free(lru);
    }
    
    pthread_mutex_unlock(&cache->lock);
}

// ========== End of Trie/LRU Implementation ==========

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    printf("=== Name Server Starting ===\n");
    
    // Get and display local IP address
    char local_ip[64];
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
        struct sockaddr_in serv;
        memset(&serv, 0, sizeof(serv));
        serv.sin_family = AF_INET;
        serv.sin_addr.s_addr = inet_addr("8.8.8.8");
        serv.sin_port = htons(53);
        
        if (connect(sock, (const struct sockaddr*)&serv, sizeof(serv)) >= 0) {
            struct sockaddr_in name;
            socklen_t namelen = sizeof(name);
            if (getsockname(sock, (struct sockaddr*)&name, &namelen) >= 0) {
                inet_ntop(AF_INET, &name.sin_addr, local_ip, sizeof(local_ip));
                printf("Server IP Address: %s\n", local_ip);
                printf("(Share this IP with Storage Servers and Clients on other devices)\n");
            }
        }
        close(sock);
    }
    
    printf("Listening for Storage Server registrations on port %d\n", SS_REG_PORT);
    printf("Listening for Client connections on port %d\n", NS_PORT);
    
    // Initialize mutexes
    if (pthread_mutex_init(&ss_list_mutex, NULL) != 0) {
        perror("SS list mutex initialization failed");
        return 1;
    }
    
    if (pthread_mutex_init(&client_list_mutex, NULL) != 0) {
        perror("Client list mutex initialization failed");
        pthread_mutex_destroy(&ss_list_mutex);
        return 1;
    }
    
    if (pthread_mutex_init(&file_list_mutex, NULL) != 0) {
        perror("File list mutex initialization failed");
        pthread_mutex_destroy(&ss_list_mutex);
        pthread_mutex_destroy(&client_list_mutex);
        return 1;
    }
    
    // Initialize Trie for efficient file lookups
    trie_root = create_trie_node();
    if (!trie_root) {
        fprintf(stderr, "Failed to initialize Trie\n");
        pthread_mutex_destroy(&ss_list_mutex);
        pthread_mutex_destroy(&client_list_mutex);
        pthread_mutex_destroy(&file_list_mutex);
        return 1;
    }
    
    if (pthread_mutex_init(&trie_mutex, NULL) != 0) {
        perror("Trie mutex initialization failed");
        pthread_mutex_destroy(&ss_list_mutex);
        pthread_mutex_destroy(&client_list_mutex);
        pthread_mutex_destroy(&file_list_mutex);
        return 1;
    }
    
    // Initialize LRU Cache for frequently accessed files
    init_lru_cache(&lru_cache);
    printf("Trie and LRU Cache initialized for efficient file lookups\n");
    
    // Initialize journal mutex
    if (pthread_mutex_init(&journal_mutex, NULL) != 0) {
        perror("Journal mutex initialization failed");
        pthread_mutex_destroy(&ss_list_mutex);
        pthread_mutex_destroy(&client_list_mutex);
        pthread_mutex_destroy(&file_list_mutex);
        pthread_mutex_destroy(&trie_mutex);
        return 1;
    }
    
    // Create data directory if it doesn't exist
    mkdir("data", 0755);
    
    // Recover from persistent storage
    recover_from_persistence();
    
    // Create TCP server for Storage Server registration
    int server_fd = create_tcp_server(SS_REG_PORT);
    if (server_fd < 0) {
        fprintf(stderr, "Failed to create server on port %d\n", SS_REG_PORT);
        pthread_mutex_destroy(&ss_list_mutex);
        pthread_mutex_destroy(&client_list_mutex);
        return 1;
    }
    
    // Create a separate thread to handle client connections
    pthread_t client_thread;
    if (pthread_create(&client_thread, NULL, client_listener_thread, NULL) != 0) {
        perror("Failed to create client listener thread");
        close(server_fd);
        pthread_mutex_destroy(&ss_list_mutex);
        pthread_mutex_destroy(&client_list_mutex);
        return 1;
    }
    pthread_detach(client_thread);
    
    // Create heartbeat monitoring thread for fault tolerance
    pthread_t heartbeat_thread;
    if (pthread_create(&heartbeat_thread, NULL, heartbeat_monitor_thread, NULL) != 0) {
        perror("Failed to create heartbeat monitor thread");
        close(server_fd);
        pthread_mutex_destroy(&ss_list_mutex);
        pthread_mutex_destroy(&client_list_mutex);
        return 1;
    }
    pthread_detach(heartbeat_thread);
    printf("Heartbeat monitoring enabled for fault tolerance\n");
    
    printf("Name Server ready. Waiting for connections...\n\n");
    
    // Main loop - accept SS connections
    while (1) {
        int client_fd = accept_client(server_fd);
        if (client_fd < 0) {
            fprintf(stderr, "Failed to accept SS connection\n");
            continue;
        }
        
        // Spawn thread to handle SS registration
        pthread_t thread_id;
        int* client_fd_ptr = malloc(sizeof(int));
        if (client_fd_ptr == NULL) {
            perror("Failed to allocate memory for client_fd");
            close(client_fd);
            continue;
        }
        *client_fd_ptr = client_fd;
        
        if (pthread_create(&thread_id, NULL, handle_ss_registration, client_fd_ptr) != 0) {
            perror("Failed to create thread for SS registration");
            free(client_fd_ptr);
            close(client_fd);
            continue;
        }
        
        // Detach thread so it cleans up automatically
        pthread_detach(thread_id);
    }
    
    // Cleanup (unreachable in this version)
    close(server_fd);
    pthread_mutex_destroy(&ss_list_mutex);
    pthread_mutex_destroy(&client_list_mutex);
    
    return 0;
}

/**
 * Handle Storage Server registration
 * Thread function that processes SS registration requests
 */
void* handle_ss_registration(void* arg) {
    int client_fd = *(int*)arg;
    free(arg);
    
    char buffer[MAX_MSG_SIZE];
    memset(buffer, 0, sizeof(buffer));
    
    // Read registration message from Storage Server
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        perror("Failed to read registration message");
        close(client_fd);
        return NULL;
    }
    buffer[bytes_read] = '\0';
    
    // Parse registration message: "REGISTER <IP> <NS_PORT> <CLIENT_PORT> [FILES:file1,file2,...]"
    char command[32];
    char ip[16];
    int ns_port, client_port;
    char* files_section = NULL;
    
    // First parse basic registration
    int parsed = sscanf(buffer, "%s %s %d %d", command, ip, &ns_port, &client_port);
    
    if (parsed != 4 || strcmp(command, "REGISTER") != 0) {
        fprintf(stderr, "Invalid registration message: %s\n", buffer);
        const char* error_msg = "ACK_FAILURE: Invalid format\n";
        write(client_fd, error_msg, strlen(error_msg));
        close(client_fd);
        return NULL;
    }
    
    // Check for file list
    files_section = strstr(buffer, "FILES:");
    if (files_section != NULL) {
        files_section += 6;  // Skip "FILES:"
    }
    
    // Lock mutex and add to storage server list
    pthread_mutex_lock(&ss_list_mutex);
    
    if (ss_count >= 10) {
        pthread_mutex_unlock(&ss_list_mutex);
        fprintf(stderr, "Storage Server list full. Cannot register more.\n");
        const char* error_msg = "ACK_FAILURE: Server list full\n";
        write(client_fd, error_msg, strlen(error_msg));
        close(client_fd);
        return NULL;
    }
    
    // Add new storage server to list
    strncpy(ss_list[ss_count].ip, ip, sizeof(ss_list[ss_count].ip) - 1);
    ss_list[ss_count].ip[sizeof(ss_list[ss_count].ip) - 1] = '\0';
    ss_list[ss_count].ns_port = ns_port;
    ss_list[ss_count].client_port = client_port;
    ss_list[ss_count].ss_id = ss_count;  // Assign unique ID
    ss_list[ss_count].is_alive = 1;      // Mark as alive
    ss_list[ss_count].last_heartbeat = time(NULL);  // Initialize heartbeat
    
    int current_id = ss_count;
    ss_count++;
    
    pthread_mutex_unlock(&ss_list_mutex);
    
    // Log the registration with timestamp
    time_t now = time(NULL);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    printf("[%s] Storage Server registered:\n", timestamp);
    printf("  ID: %d\n", current_id);
    printf("  IP: %s\n", ip);
    printf("  NS Port: %d\n", ns_port);
    printf("  Client Port: %d\n", client_port);
    printf("  Total SS count: %d\n", ss_count);
    
    // Process file list if present
    if (files_section != NULL && strlen(files_section) > 0) {
        printf("  Files reported: %s\n", files_section);
        
        // Parse comma-separated file list
        char files_copy[MAX_MSG_SIZE];
        strncpy(files_copy, files_section, sizeof(files_copy) - 1);
        files_copy[sizeof(files_copy) - 1] = '\0';
        
        char* filename = strtok(files_copy, ",");
        int files_added = 0;
        
        pthread_mutex_lock(&file_list_mutex);
        while (filename != NULL && file_count < 100) {
            // Check if file already exists in metadata
            int exists = 0;
            for (int i = 0; i < file_count; i++) {
                if (strcmp(file_list[i].filename, filename) == 0) {
                    exists = 1;
                    break;
                }
            }
            
            // Add new file to metadata
            if (!exists) {
                strncpy(file_list[file_count].filename, filename, 
                        sizeof(file_list[file_count].filename) - 1);
                file_list[file_count].filename[sizeof(file_list[file_count].filename) - 1] = '\0';
                file_list[file_count].ss_index = current_id;
                strcpy(file_list[file_count].owner, "system");  // Default owner
                file_list[file_count].access_count = 0;
                
                // Insert into Trie
                trie_insert(trie_root, filename, file_count);
                
                file_count++;
                files_added++;
            }
            
            filename = strtok(NULL, ",");
        }
        pthread_mutex_unlock(&file_list_mutex);
        
        printf("  Files added to metadata: %d\n", files_added);
    }
    printf("\n");
    
    // ========== SS Recovery Sync ==========
    // If this is a reconnection (ss_count > 1), sync files from other SS instances
    if (ss_count > 1) {
        printf("→ Initiating recovery sync for SS %d...\n", current_id);
        
        pthread_mutex_lock(&file_list_mutex);
        int files_to_sync = 0;
        
        // Find files that this SS doesn't have but should (files not in its replica list)
        for (int f = 0; f < file_count; f++) {
            if (file_list[f].is_folder) continue;  // Skip folders
            
            // Check if this SS has this file as a replica
            int has_file = 0;
            for (int r = 0; r < file_list[f].replica_count; r++) {
                if (file_list[f].ss_replicas[r] == current_id) {
                    has_file = 1;
                    break;
                }
            }
            
            if (!has_file && file_list[f].replica_count > 0) {
                // Find an alive SS that has this file
                int source_ss = -1;
                char source_ip[16];
                int source_port;
                
                pthread_mutex_lock(&ss_list_mutex);
                for (int r = 0; r < file_list[f].replica_count; r++) {
                    int ss_idx = file_list[f].ss_replicas[r];
                    if (ss_idx >= 0 && ss_idx < ss_count && ss_list[ss_idx].is_alive) {
                        source_ss = ss_idx;
                        strncpy(source_ip, ss_list[source_ss].ip, sizeof(source_ip) - 1);
                        source_ip[sizeof(source_ip) - 1] = '\0';
                        source_port = ss_list[source_ss].ns_port;
                        break;
                    }
                }
                pthread_mutex_unlock(&ss_list_mutex);
                
                if (source_ss >= 0) {
                    // Trigger replication from source SS to this new SS
                    replicate_file_to_ss(current_id, file_list[f].filename, source_ip, source_port);
                    
                    // Add this SS to the replica list
                    if (file_list[f].replica_count < MAX_REPLICAS) {
                        file_list[f].ss_replicas[file_list[f].replica_count] = current_id;
                        file_list[f].replica_count++;
                    }
                    
                    files_to_sync++;
                }
            }
        }
        
        pthread_mutex_unlock(&file_list_mutex);
        
        if (files_to_sync > 0) {
            printf("✓ Recovery sync initiated: %d files to replicate to SS %d\n", files_to_sync, current_id);
        } else {
            printf("✓ Recovery sync complete: SS %d is up-to-date\n", current_id);
        }
    }
    
    // Send acknowledgment back to Storage Server
    const char* ack_msg = "ACK_SUCCESS\n";
    ssize_t bytes_written = write(client_fd, ack_msg, strlen(ack_msg));
    if (bytes_written < 0) {
        perror("Failed to send ACK to Storage Server");
    }
    
    // Close connection
    close(client_fd);
    
    return NULL;
}

/**
 * Thread function to listen for client connections
 */
// ========== Persistence Implementation ==========

void journal_operation(const char* operation) {
    pthread_mutex_lock(&journal_mutex);
    
    FILE* journal = fopen(JOURNAL_FILE, "a");
    if (journal == NULL) {
        fprintf(stderr, "WARNING: Failed to open journal file\n");
        pthread_mutex_unlock(&journal_mutex);
        return;
    }
    
    // Write timestamp and operation
    time_t now = time(NULL);
    fprintf(journal, "[%ld] %s\n", now, operation);
    fflush(journal);  // Force write to disk
    fclose(journal);
    
    journal_operation_count++;
    
    // Check if we need a checkpoint
    if (journal_operation_count >= CHECKPOINT_INTERVAL) {
        create_checkpoint();
        journal_operation_count = 0;
    }
    
    pthread_mutex_unlock(&journal_mutex);
}

void create_checkpoint() {
    printf("Creating checkpoint...\n");
    
    FILE* checkpoint = fopen(CHECKPOINT_FILE, "w");
    if (checkpoint == NULL) {
        fprintf(stderr, "ERROR: Failed to create checkpoint file\n");
        return;
    }
    
    // Write magic header
    fprintf(checkpoint, "NAMESERVER_CHECKPOINT_V1\n");
    
    // Write storage server count and list
    pthread_mutex_lock(&ss_list_mutex);
    fprintf(checkpoint, "SS_COUNT %d\n", ss_count);
    for (int i = 0; i < ss_count; i++) {
        fprintf(checkpoint, "SS %s %d %d\n", 
                ss_list[i].ip, ss_list[i].ns_port, ss_list[i].client_port);
    }
    pthread_mutex_unlock(&ss_list_mutex);
    
    // Write file count and metadata
    pthread_mutex_lock(&file_list_mutex);
    fprintf(checkpoint, "FILE_COUNT %d\n", file_count);
    for (int i = 0; i < file_count; i++) {
        fprintf(checkpoint, "FILE %s %d %s\n",
                file_list[i].filename, file_list[i].ss_index, file_list[i].owner);
        
        // Write access list
        fprintf(checkpoint, "ACCESS_COUNT %d\n", file_list[i].access_count);
        for (int j = 0; j < file_list[i].access_count; j++) {
            fprintf(checkpoint, "ACCESS %s %d\n",
                    file_list[i].access_list[j], file_list[i].access_permissions[j]);
        }
    }
    pthread_mutex_unlock(&file_list_mutex);
    
    fclose(checkpoint);
    
    // Truncate journal after successful checkpoint
    FILE* journal = fopen(JOURNAL_FILE, "w");
    if (journal) {
        fclose(journal);
    }
    
    printf("Checkpoint created successfully\n");
}

void recover_from_persistence() {
    printf("Recovering metadata from persistence...\n");
    
    // Try to load checkpoint first
    FILE* checkpoint = fopen(CHECKPOINT_FILE, "r");
    if (checkpoint != NULL) {
        char line[512];
        
        // Verify magic header
        if (fgets(line, sizeof(line), checkpoint) == NULL ||
            strncmp(line, "NAMESERVER_CHECKPOINT_V1", 24) != 0) {
            fprintf(stderr, "WARNING: Invalid checkpoint file\n");
            fclose(checkpoint);
            return;
        }
        
        // Read storage servers
        int ss_cnt = 0;
        if (fscanf(checkpoint, "SS_COUNT %d\n", &ss_cnt) == 1) {
            for (int i = 0; i < ss_cnt && i < 10; i++) {
                fscanf(checkpoint, "SS %s %d %d\n",
                       ss_list[i].ip, &ss_list[i].ns_port, &ss_list[i].client_port);
            }
            ss_count = ss_cnt;
            printf("Recovered %d storage servers\n", ss_count);
        }
        
        // Read files
        int f_cnt = 0;
        if (fscanf(checkpoint, "FILE_COUNT %d\n", &f_cnt) == 1) {
            for (int i = 0; i < f_cnt && i < 100; i++) {
                fscanf(checkpoint, "FILE %s %d %s\n",
                       file_list[i].filename, &file_list[i].ss_index, file_list[i].owner);
                
                // Read access list
                int access_cnt = 0;
                fscanf(checkpoint, "ACCESS_COUNT %d\n", &access_cnt);
                file_list[i].access_count = access_cnt;
                
                for (int j = 0; j < access_cnt && j < MAX_ACCESS_USERS; j++) {
                    fscanf(checkpoint, "ACCESS %s %d\n",
                           file_list[i].access_list[j], &file_list[i].access_permissions[j]);
                }
                
                // Rebuild Trie
                trie_insert(trie_root, file_list[i].filename, i);
            }
            file_count = f_cnt;
            printf("Recovered %d files\n", file_count);
        }
        
        fclose(checkpoint);
    } else {
        printf("No checkpoint file found, starting fresh\n");
    }
    
    // TODO: Replay journal operations if needed
    // For now, we'll rely on checkpointing
}

void* client_listener_thread(void* arg) {
    (void)arg;
    
    // Create TCP server for client connections
    int client_server_fd = create_tcp_server(NS_PORT);
    if (client_server_fd < 0) {
        fprintf(stderr, "Failed to create client server on port %d\n", NS_PORT);
        return NULL;
    }
    
    printf("Client listener thread started on port %d\n", NS_PORT);
    
    // Accept client connections
    while (1) {
        int client_fd = accept_client(client_server_fd);
        if (client_fd < 0) {
            fprintf(stderr, "Failed to accept client connection\n");
            continue;
        }
        
        // Spawn thread to handle client
        pthread_t thread_id;
        int* client_fd_ptr = malloc(sizeof(int));
        if (client_fd_ptr == NULL) {
            perror("Failed to allocate memory for client_fd");
            close(client_fd);
            continue;
        }
        *client_fd_ptr = client_fd;
        
        if (pthread_create(&thread_id, NULL, handle_client_connection, client_fd_ptr) != 0) {
            perror("Failed to create thread for client connection");
            free(client_fd_ptr);
            close(client_fd);
            continue;
        }
        
        pthread_detach(thread_id);
    }
    
    close(client_server_fd);
    return NULL;
}

/**
 * Get file statistics (word count, char count, timestamps) from storage server
 * Returns 1 on success, 0 on failure
 */
int get_file_stats(const char* filename, int ss_index, int* word_count, int* char_count,
                   long* file_size, time_t* created, time_t* modified, time_t* accessed) {
    if (ss_index < 0 || ss_index >= ss_count) {
        return 0;
    }
    
    // Connect to storage server
    int ss_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_fd < 0) {
        perror("Socket creation failed");
        return 0;
    }
    
    struct sockaddr_in ss_addr;
    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_list[ss_index].ns_port);
    
    if (inet_pton(AF_INET, ss_list[ss_index].ip, &ss_addr.sin_addr) <= 0) {
        close(ss_fd);
        return 0;
    }
    
    if (connect(ss_fd, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        close(ss_fd);
        return 0;
    }
    
    // Send GET_STATS command
    char command[256];
    snprintf(command, sizeof(command), "GET_STATS %s\n", filename);
    write(ss_fd, command, strlen(command));
    
    // Read response
    char response[512];
    memset(response, 0, sizeof(response));
    ssize_t n = read(ss_fd, response, sizeof(response) - 1);
    close(ss_fd);
    
    if (n <= 0) {
        return 0;
    }
    
    // Parse response: "STATS <word_count> <char_count> <file_size> <created> <modified> <accessed>"
    int wc, cc;
    long fs, ct, mt, at;
    if (sscanf(response, "STATS %d %d %ld %ld %ld %ld", &wc, &cc, &fs, &ct, &mt, &at) == 6) {
        *word_count = wc;
        *char_count = cc;
        *file_size = fs;
        *created = (time_t)ct;
        *modified = (time_t)mt;
        *accessed = (time_t)at;
        return 1;
    }
    
    return 0;
}

/**
 * Check if user has access to a file
 * Returns 1 if user is owner or in access list, 0 otherwise
 */
int has_file_access(struct FileInfo* file, const char* username) {
    // Owner always has access
    if (strcmp(file->owner, username) == 0) {
        return 1;
    }
    
    // Check access list
    for (int i = 0; i < file->access_count; i++) {
        if (strcmp(file->access_list[i], username) == 0) {
            return 1;
        }
    }
    
    return 0;
}

/**
 * Check if user has write access to a file
 * Returns 1 if user is owner or has write permission, 0 otherwise
 */
int has_write_access(struct FileInfo* file, const char* username) {
    // Owner always has write access
    if (strcmp(file->owner, username) == 0) {
        return 1;
    }
    
    // Check access list for write permission (ACCESS_WRITE = 2)
    for (int i = 0; i < file->access_count; i++) {
        if (strcmp(file->access_list[i], username) == 0 && file->access_permissions[i] == 2) {
            return 1;
        }
    }
    
    return 0;
}

/**
 * Handle client connection and commands
 */
void* handle_client_connection(void* arg) {
    int client_fd = *(int*)arg;
    free(arg);
    
    char buffer[MAX_MSG_SIZE];
    char username[100] = {0};
    int client_index = -1;
    time_t now;
    char timestamp[64];
    
    // Read registration message from client
    memset(buffer, 0, sizeof(buffer));
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        perror("Failed to read client registration");
        close(client_fd);
        return NULL;
    }
    buffer[bytes_read] = '\0';
    
    // Parse registration: "REGISTER_CLIENT <username>"
    char command[32];
    int parsed = sscanf(buffer, "%s %s", command, username);
    
    if (parsed != 2 || strcmp(command, "REGISTER_CLIENT") != 0) {
        fprintf(stderr, "Invalid client registration: %s\n", buffer);
        const char* error_msg = "ACK_FAILURE: Invalid registration format\n";
        write(client_fd, error_msg, strlen(error_msg));
        close(client_fd);
        return NULL;
    }
    
    // Add client to list
    pthread_mutex_lock(&client_list_mutex);
    
    if (client_count >= 50) {
        pthread_mutex_unlock(&client_list_mutex);
        fprintf(stderr, "Client list full\n");
        const char* error_msg = "ACK_FAILURE: Client list full\n";
        write(client_fd, error_msg, strlen(error_msg));
        close(client_fd);
        return NULL;
    }
    
    // Check if username already exists
    int username_exists = 0;
    int existing_client_index = -1;
    for (int i = 0; i < client_count; i++) {
        if (strcmp(client_list[i].username, username) == 0) {
            username_exists = 1;
            existing_client_index = i;
            break;
        }
    }
    
    if (username_exists) {
        // User already exists - allow login by reusing the username
        // Update the socket fd for this user (in case of reconnection)
        client_list[existing_client_index].fd = client_fd;
        client_index = existing_client_index;
        pthread_mutex_unlock(&client_list_mutex);
        
        now = time(NULL);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
        printf("[%s] User '%s' logged in (existing user, ID: %d)\n", timestamp, username, client_index);
    } else {
        // New user - add to the client list
        strncpy(client_list[client_count].username, username, sizeof(client_list[client_count].username) - 1);
        client_list[client_count].username[sizeof(client_list[client_count].username) - 1] = '\0';
        client_list[client_count].fd = client_fd;
        client_index = client_count;
        client_count++;
        
        pthread_mutex_unlock(&client_list_mutex);
        
        // Log registration for new user
        now = time(NULL);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
        printf("[%s] New client registered: %s (ID: %d, Total clients: %d)\n", 
               timestamp, username, client_index, client_count);
    }
    
    // Send acknowledgment
    const char* ack_msg = "ACK_SUCCESS\n";
    write(client_fd, ack_msg, strlen(ack_msg));
    
    // Command loop - handle client commands
    while (1) {
        memset(buffer, 0, sizeof(buffer));
        bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
        
        if (bytes_read <= 0) {
            // Client disconnected
            now = time(NULL);
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
            printf("[%s] Client disconnected: %s\n", timestamp, username);
            break;
        }
        
        buffer[bytes_read] = '\0';
        
        // Remove trailing newline
        char* newline = strchr(buffer, '\n');
        if (newline) *newline = '\0';
        
        // Update timestamp for each command
        now = time(NULL);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
        printf("[%s] Command from %s: %s\n", timestamp, username, buffer);
        
        // Handle LIST command
        if (strcmp(buffer, "LIST") == 0) {
            char response[MAX_MSG_SIZE];
            memset(response, 0, sizeof(response));
            
            pthread_mutex_lock(&client_list_mutex);
            
            // Build list of usernames
            int offset = 0;
            for (int i = 0; i < client_count && offset < (int)sizeof(response) - 100; i++) {
                offset += snprintf(response + offset, sizeof(response) - offset, 
                                 "--> %s\n", client_list[i].username);
            }
            
            pthread_mutex_unlock(&client_list_mutex);
            
            // Send response
            if (offset > 0) {
                write(client_fd, response, strlen(response));
            } else {
                const char* empty_msg = "No users registered\n";
                write(client_fd, empty_msg, strlen(empty_msg));
            }
        }
        // Handle CREATE command
        else if (strncmp(buffer, "CREATE ", 7) == 0) {
            char filename[100];
            if (sscanf(buffer, "CREATE %s", filename) != 1) {
                const char* error_msg = "ERROR: Invalid CREATE format. Use: CREATE <filename>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check if file already exists (using Trie for O(k) lookup)
            int file_idx = trie_search(trie_root, filename);
            
            if (file_idx >= 0) {
                const char* error_msg = "ERROR: File already exists\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check if we have any storage servers
            pthread_mutex_lock(&ss_list_mutex);
            if (ss_count == 0) {
                pthread_mutex_unlock(&ss_list_mutex);
                const char* error_msg = "ERROR: No storage servers available\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Pick a storage server (round-robin) - only alive servers
            int ss_index = -1;
            for (int i = 0; i < ss_count; i++) {
                int candidate = (file_count + i) % ss_count;
                if (ss_list[candidate].is_alive) {
                    ss_index = candidate;
                    break;
                }
            }
            
            if (ss_index == -1) {
                pthread_mutex_unlock(&ss_list_mutex);
                const char* error_msg = "ERROR: No alive storage servers available\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            char ss_ip[16];
            int ss_port;
            strncpy(ss_ip, ss_list[ss_index].ip, sizeof(ss_ip) - 1);
            ss_ip[sizeof(ss_ip) - 1] = '\0';
            ss_port = ss_list[ss_index].ns_port;  // Use ns_port for NS commands
            pthread_mutex_unlock(&ss_list_mutex);
            
            printf("Creating file '%s' on Storage Server %d (%s:%d)\n", 
                   filename, ss_index, ss_ip, ss_port);
            
            // Connect to Storage Server
            int ss_fd = connect_to_server(ss_ip, ss_port);
            if (ss_fd < 0) {
                fprintf(stderr, "Failed to connect to Storage Server\n");
                const char* error_msg = "ERROR: Cannot connect to Storage Server\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Send CREATE_FILE command to SS
            char ss_command[MAX_MSG_SIZE];
            snprintf(ss_command, sizeof(ss_command), "CREATE_FILE %s", filename);
            write(ss_fd, ss_command, strlen(ss_command));
            
            // Wait for response from SS
            char ss_response[MAX_MSG_SIZE];
            memset(ss_response, 0, sizeof(ss_response));
            ssize_t ss_bytes = read(ss_fd, ss_response, sizeof(ss_response) - 1);
            close(ss_fd);
            
            if (ss_bytes <= 0 || strncmp(ss_response, "ACK_SUCCESS", 11) != 0) {
                fprintf(stderr, "Storage Server failed to create file\n");
                const char* error_msg = "ERROR: Storage Server failed to create file\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Add file to file list
            pthread_mutex_lock(&file_list_mutex);
            strncpy(file_list[file_count].filename, filename, sizeof(file_list[file_count].filename) - 1);
            file_list[file_count].filename[sizeof(file_list[file_count].filename) - 1] = '\0';
            strncpy(file_list[file_count].full_path, filename, sizeof(file_list[file_count].full_path) - 1);
            file_list[file_count].full_path[sizeof(file_list[file_count].full_path) - 1] = '\0';
            file_list[file_count].is_folder = 0;  // This is a file
            file_list[file_count].ss_index = ss_index;
            
            // Set owner (get from client_list using client_fd)
            int owner_found = 0;
            pthread_mutex_lock(&client_list_mutex);
            for (int i = 0; i < client_count; i++) {
                if (client_list[i].fd == client_fd) {
                    strncpy(file_list[file_count].owner, client_list[i].username, 
                            sizeof(file_list[file_count].owner) - 1);
                    file_list[file_count].owner[sizeof(file_list[file_count].owner) - 1] = '\0';
                    owner_found = 1;
                    printf("DEBUG: Set owner to '%s' for file '%s'\n", client_list[i].username, filename);
                    break;
                }
            }
            pthread_mutex_unlock(&client_list_mutex);
            
            // Fallback: if owner not found in client_list, use the username from this thread
            if (!owner_found) {
                strncpy(file_list[file_count].owner, username, 
                        sizeof(file_list[file_count].owner) - 1);
                file_list[file_count].owner[sizeof(file_list[file_count].owner) - 1] = '\0';
                printf("DEBUG: Set owner (fallback) to '%s' for file '%s'\n", username, filename);
            }
            
            // Initialize empty access list
            file_list[file_count].access_count = 0;
            
            // Initialize replica tracking
            file_list[file_count].ss_replicas[0] = ss_index;  // Primary replica
            file_list[file_count].replica_count = 1;
            
            // Initialize access request queue
            file_list[file_count].request_count = 0;
            for (int i = 0; i < MAX_ACCESS_REQUESTS; i++) {
                file_list[file_count].requests[i].active = 0;
            }
            
            // Initialize timestamps
            time_t now = time(NULL);
            file_list[file_count].created_time = now;
            file_list[file_count].modified_time = now;
            file_list[file_count].accessed_time = now;
            strncpy(file_list[file_count].last_accessed_by, username,
                   sizeof(file_list[file_count].last_accessed_by) - 1);
            file_list[file_count].file_size = 0;
            
            // Insert into Trie for efficient future lookups
            trie_insert(trie_root, filename, file_count);
            
            int current_file_index = file_count;
            file_count++;
            pthread_mutex_unlock(&file_list_mutex);
            
            // Asynchronously replicate to other storage servers if available
            pthread_mutex_lock(&ss_list_mutex);
            for (int i = 0; i < ss_count && file_list[current_file_index].replica_count < MAX_REPLICAS; i++) {
                if (i != ss_index && ss_list[i].is_alive) {
                    // Add to replica list
                    pthread_mutex_lock(&file_list_mutex);
                    file_list[current_file_index].ss_replicas[file_list[current_file_index].replica_count] = i;
                    file_list[current_file_index].replica_count++;
                    pthread_mutex_unlock(&file_list_mutex);
                    
                    // Initiate replication (async)
                    replicate_file_to_ss(i, filename, ss_ip, ss_port);
                }
            }
            pthread_mutex_unlock(&ss_list_mutex);
            
            // Journal the operation for persistence
            char journal_entry[256];
            snprintf(journal_entry, sizeof(journal_entry), "CREATE %s %d %s", 
                     filename, ss_index, file_list[current_file_index].owner);
            journal_operation(journal_entry);
            
            printf("✓ File '%s' created successfully (Primary SS: %d, Replicas: %d, Total files: %d)\n", 
                   filename, ss_index, file_list[current_file_index].replica_count, file_count);
            
            // Send success to client
            const char* ack_msg = "ACK_SUCCESS\n";
            write(client_fd, ack_msg, strlen(ack_msg));
        }
        // Handle CREATEFOLDER command
        else if (strncmp(buffer, "CREATEFOLDER ", 13) == 0) {
            char foldername[256];
            if (sscanf(buffer, "CREATEFOLDER %s", foldername) != 1) {
                const char* error_msg = "ERROR: Invalid CREATEFOLDER format. Use: CREATEFOLDER <foldername>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check if folder already exists
            int folder_idx = trie_search(trie_root, foldername);
            if (folder_idx >= 0) {
                const char* error_msg = "ERROR: Folder already exists\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Add folder to file list
            pthread_mutex_lock(&file_list_mutex);
            strncpy(file_list[file_count].filename, foldername, sizeof(file_list[file_count].filename) - 1);
            file_list[file_count].filename[sizeof(file_list[file_count].filename) - 1] = '\0';
            strncpy(file_list[file_count].full_path, foldername, sizeof(file_list[file_count].full_path) - 1);
            file_list[file_count].full_path[sizeof(file_list[file_count].full_path) - 1] = '\0';
            file_list[file_count].is_folder = 1;
            file_list[file_count].ss_index = -1;  // Folders don't have storage servers
            
            // Set owner
            strncpy(file_list[file_count].owner, username, sizeof(file_list[file_count].owner) - 1);
            file_list[file_count].owner[sizeof(file_list[file_count].owner) - 1] = '\0';
            
            // Initialize empty access list
            file_list[file_count].access_count = 0;
            
            // Initialize timestamps
            time_t now = time(NULL);
            file_list[file_count].created_time = now;
            file_list[file_count].modified_time = now;
            file_list[file_count].accessed_time = now;
            strncpy(file_list[file_count].last_accessed_by, username,
                   sizeof(file_list[file_count].last_accessed_by) - 1);
            file_list[file_count].file_size = 0;
            
            // Insert into Trie
            trie_insert(trie_root, foldername, file_count);
            
            file_count++;
            pthread_mutex_unlock(&file_list_mutex);
            
            // Journal the operation
            char journal_entry[300];
            snprintf(journal_entry, sizeof(journal_entry), "CREATEFOLDER %s %s", foldername, username);
            journal_operation(journal_entry);
            
            printf("✓ Folder '%s' created successfully by %s\n", foldername, username);
            
            const char* ack_msg = "ACK_SUCCESS: Folder created\n";
            write(client_fd, ack_msg, strlen(ack_msg));
        }
        // Handle READ command
        else if (strncmp(buffer, "READ ", 5) == 0) {
            char filename[100];
            if (sscanf(buffer, "READ %s", filename) != 1) {
                const char* error_msg = "ERROR: Invalid READ format. Use: READ <filename>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Look up file using LRU cache + Trie (O(1) + O(k) instead of O(n))
            int file_idx = lru_get(&lru_cache, filename);
            if (file_idx < 0) {
                file_idx = trie_search(trie_root, filename);
                if (file_idx >= 0) {
                    lru_put(&lru_cache, filename, file_idx);  // Cache for future
                }
            }
            
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check access control
            pthread_mutex_lock(&file_list_mutex);
            if (!has_file_access(&file_list[file_idx], username)) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Access denied\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            int ss_index = file_list[file_idx].ss_index;
            int replica_indices[MAX_REPLICAS];
            int replica_count = file_list[file_idx].replica_count;
            for (int i = 0; i < replica_count; i++) {
                replica_indices[i] = file_list[file_idx].ss_replicas[i];
            }
            pthread_mutex_unlock(&file_list_mutex);
            
            // Try primary SS first, then replicas if primary is down
            int selected_ss = -1;
            char ss_ip[16];
            int ss_client_port;
            
            pthread_mutex_lock(&ss_list_mutex);
            
            // Try all replicas (including primary) until we find an alive one
            for (int i = 0; i < replica_count; i++) {
                int current_ss = replica_indices[i];
                if (current_ss >= 0 && current_ss < ss_count && ss_list[current_ss].is_alive) {
                    selected_ss = current_ss;
                    strncpy(ss_ip, ss_list[current_ss].ip, sizeof(ss_ip) - 1);
                    ss_ip[sizeof(ss_ip) - 1] = '\0';
                    ss_client_port = ss_list[current_ss].client_port;
                    break;
                }
            }
            
            pthread_mutex_unlock(&ss_list_mutex);
            
            if (selected_ss < 0) {
                const char* error_msg = "ERROR: No available storage servers for this file\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            if (selected_ss != ss_index) {
                printf("⚠ Primary SS %d is down, using replica SS %d for file '%s'\n",
                       ss_index, selected_ss, filename);
            }
            
            printf("Redirecting client to Storage Server %d (%s:%d) for file '%s'\n",
                   selected_ss, ss_ip, ss_client_port, filename);
            
            // Send REDIRECT message to client
            char redirect_msg[MAX_MSG_SIZE];
            snprintf(redirect_msg, sizeof(redirect_msg), "REDIRECT %s %d\n", ss_ip, ss_client_port);
            write(client_fd, redirect_msg, strlen(redirect_msg));
        }
        // Handle WRITE command
        else if (strncmp(buffer, "WRITE ", 6) == 0) {
            char filename[100];
            int sentence_num;
            if (sscanf(buffer, "WRITE %s %d", filename, &sentence_num) != 2) {
                const char* error_msg = "ERROR: Invalid WRITE format. Use: WRITE <filename> <sentence_num>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Look up file using LRU cache + Trie
            int file_idx = lru_get(&lru_cache, filename);
            if (file_idx < 0) {
                file_idx = trie_search(trie_root, filename);
                if (file_idx >= 0) {
                    lru_put(&lru_cache, filename, file_idx);
                }
            }
            
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check write access control
            pthread_mutex_lock(&file_list_mutex);
            printf("DEBUG WRITE: User '%s' trying to write to '%s' owned by '%s'\n", 
                   username, filename, file_list[file_idx].owner);
            if (!has_write_access(&file_list[file_idx], username)) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Write access denied\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            int ss_index = file_list[file_idx].ss_index;
            pthread_mutex_unlock(&file_list_mutex);
            
            // Get Storage Server information
            pthread_mutex_lock(&ss_list_mutex);
            if (ss_index < 0 || ss_index >= ss_count) {
                pthread_mutex_unlock(&ss_list_mutex);
                const char* error_msg = "ERROR: Invalid storage server index\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            char ss_ip[16];
            int ss_client_port;
            strncpy(ss_ip, ss_list[ss_index].ip, sizeof(ss_ip) - 1);
            ss_ip[sizeof(ss_ip) - 1] = '\0';
            ss_client_port = ss_list[ss_index].client_port;  // Port for client connections
            pthread_mutex_unlock(&ss_list_mutex);
            
            printf("Redirecting client to Storage Server %d (%s:%d) for WRITE to '%s' sentence %d\n",
                   ss_index, ss_ip, ss_client_port, filename, sentence_num);
            
            // Send REDIRECT message to client
            char redirect_msg[MAX_MSG_SIZE];
            snprintf(redirect_msg, sizeof(redirect_msg), "REDIRECT %s %d\n", ss_ip, ss_client_port);
            write(client_fd, redirect_msg, strlen(redirect_msg));
        }
        // Handle UNDO command
        else if (strncmp(buffer, "UNDO ", 5) == 0) {
            char filename[100];
            if (sscanf(buffer, "UNDO %s", filename) != 1) {
                const char* error_msg = "ERROR: Invalid UNDO format. Use: UNDO <filename>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Look up file using LRU cache + Trie
            int file_idx = lru_get(&lru_cache, filename);
            if (file_idx < 0) {
                file_idx = trie_search(trie_root, filename);
                if (file_idx >= 0) {
                    lru_put(&lru_cache, filename, file_idx);
                }
            }
            
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Get Storage Server information
            pthread_mutex_lock(&file_list_mutex);
            int ss_index = file_list[file_idx].ss_index;
            pthread_mutex_unlock(&file_list_mutex);
            
            pthread_mutex_lock(&ss_list_mutex);
            if (ss_index < 0 || ss_index >= ss_count) {
                pthread_mutex_unlock(&ss_list_mutex);
                const char* error_msg = "ERROR: Invalid storage server index\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            char ss_ip[16];
            int ss_client_port;
            strncpy(ss_ip, ss_list[ss_index].ip, sizeof(ss_ip) - 1);
            ss_ip[sizeof(ss_ip) - 1] = '\0';
            ss_client_port = ss_list[ss_index].client_port;
            pthread_mutex_unlock(&ss_list_mutex);
            
            printf("Redirecting client to Storage Server %d (%s:%d) for UNDO on '%s'\n",
                   ss_index, ss_ip, ss_client_port, filename);
            
            // Send REDIRECT message to client
            char redirect_msg[MAX_MSG_SIZE];
            snprintf(redirect_msg, sizeof(redirect_msg), "REDIRECT %s %d\n", ss_ip, ss_client_port);
            write(client_fd, redirect_msg, strlen(redirect_msg));
        }
        // Handle CHECKPOINT command
        else if (strncmp(buffer, "CHECKPOINT ", 11) == 0) {
            char filename[100], tag[MAX_TAG_LENGTH];
            if (sscanf(buffer, "CHECKPOINT %s %s", filename, tag) != 2) {
                const char* error_msg = "ERROR: Invalid CHECKPOINT format. Use: CHECKPOINT <filename> <tag>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Find file
            int file_idx = lru_get(&lru_cache, filename);
            if (file_idx < 0) {
                file_idx = trie_search(trie_root, filename);
                if (file_idx >= 0) {
                    lru_put(&lru_cache, filename, file_idx);
                }
            }
            
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check write access
            pthread_mutex_lock(&file_list_mutex);
            if (!has_write_access(&file_list[file_idx], username)) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Write access required to create checkpoints\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            int ss_index = file_list[file_idx].ss_index;
            pthread_mutex_unlock(&file_list_mutex);
            
            // Connect to storage server
            pthread_mutex_lock(&ss_list_mutex);
            if (ss_index < 0 || ss_index >= ss_count) {
                pthread_mutex_unlock(&ss_list_mutex);
                const char* error_msg = "ERROR: Invalid storage server\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            char ss_ip[16];
            int ss_port;
            strncpy(ss_ip, ss_list[ss_index].ip, sizeof(ss_ip) - 1);
            ss_port = ss_list[ss_index].ns_port;
            pthread_mutex_unlock(&ss_list_mutex);
            
            int ss_fd = connect_to_server(ss_ip, ss_port);
            if (ss_fd < 0) {
                const char* error_msg = "ERROR: Cannot connect to storage server\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Send CREATE_CHECKPOINT command
            char ss_command[MAX_MSG_SIZE];
            snprintf(ss_command, sizeof(ss_command), "CREATE_CHECKPOINT %s %s %s", filename, tag, username);
            write(ss_fd, ss_command, strlen(ss_command));
            
            // Get response
            char ss_response[MAX_MSG_SIZE];
            memset(ss_response, 0, sizeof(ss_response));
            read(ss_fd, ss_response, sizeof(ss_response) - 1);
            close(ss_fd);
            
            if (strncmp(ss_response, "ACK_SUCCESS", 11) == 0) {
                printf("✓ Checkpoint created: %s (tag: %s) by %s\n", filename, tag, username);
                const char* ack_msg = "ACK_SUCCESS: Checkpoint created\n";
                write(client_fd, ack_msg, strlen(ack_msg));
            } else {
                write(client_fd, ss_response, strlen(ss_response));
            }
        }
        // Handle LISTCHECKPOINTS command
        else if (strncmp(buffer, "LISTCHECKPOINTS ", 16) == 0) {
            char filename[100];
            if (sscanf(buffer, "LISTCHECKPOINTS %s", filename) != 1) {
                const char* error_msg = "ERROR: Invalid LISTCHECKPOINTS format. Use: LISTCHECKPOINTS <filename>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Find file
            int file_idx = trie_search(trie_root, filename);
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            pthread_mutex_lock(&file_list_mutex);
            int ss_index = file_list[file_idx].ss_index;
            pthread_mutex_unlock(&file_list_mutex);
            
            // Connect to storage server
            pthread_mutex_lock(&ss_list_mutex);
            char ss_ip[16];
            int ss_port;
            strncpy(ss_ip, ss_list[ss_index].ip, sizeof(ss_ip) - 1);
            ss_port = ss_list[ss_index].ns_port;
            pthread_mutex_unlock(&ss_list_mutex);
            
            int ss_fd = connect_to_server(ss_ip, ss_port);
            if (ss_fd < 0) {
                const char* error_msg = "ERROR: Cannot connect to storage server\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Send LIST_CHECKPOINTS command
            char ss_command[MAX_MSG_SIZE];
            snprintf(ss_command, sizeof(ss_command), "LIST_CHECKPOINTS %s", filename);
            write(ss_fd, ss_command, strlen(ss_command));
            
            // Get response
            char ss_response[MAX_MSG_SIZE];
            memset(ss_response, 0, sizeof(ss_response));
            ssize_t bytes = read(ss_fd, ss_response, sizeof(ss_response) - 1);
            close(ss_fd);
            
            if (bytes > 0) {
                if (strncmp(ss_response, "NO_CHECKPOINTS", 14) == 0) {
                    const char* msg = "No checkpoints found for this file.\n";
                    write(client_fd, msg, strlen(msg));
                } else {
                    char formatted_response[MAX_MSG_SIZE];
                    int offset = 0;
                    offset += snprintf(formatted_response + offset, sizeof(formatted_response) - offset,
                                     "Checkpoints for '%s':\n", filename);
                    offset += snprintf(formatted_response + offset, sizeof(formatted_response) - offset,
                                     "-------------------------------------------\n");
                    offset += snprintf(formatted_response + offset, sizeof(formatted_response) - offset,
                                     "Tag             | Created At         | Created By\n");
                    offset += snprintf(formatted_response + offset, sizeof(formatted_response) - offset,
                                     "----------------|--------------------|-----------\n");
                    
                    // Parse and format the response
                    char* line = strtok(ss_response, "\n");
                    while (line != NULL) {
                        char tag[MAX_TAG_LENGTH], timestamp[64], creator[100];
                        if (sscanf(line, "%[^|]|%[^|]|%s", tag, timestamp, creator) == 3) {
                            offset += snprintf(formatted_response + offset, sizeof(formatted_response) - offset,
                                             "%-15s | %-18s | %s\n", tag, timestamp, creator);
                        }
                        line = strtok(NULL, "\n");
                    }
                    
                    write(client_fd, formatted_response, strlen(formatted_response));
                }
            } else {
                const char* error_msg = "ERROR: Failed to get checkpoint list\n";
                write(client_fd, error_msg, strlen(error_msg));
            }
        }
        // Handle VIEWCHECKPOINT command
        else if (strncmp(buffer, "VIEWCHECKPOINT ", 15) == 0) {
            char filename[100], tag[MAX_TAG_LENGTH];
            if (sscanf(buffer, "VIEWCHECKPOINT %s %s", filename, tag) != 2) {
                const char* error_msg = "ERROR: Invalid VIEWCHECKPOINT format. Use: VIEWCHECKPOINT <filename> <tag>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Find file
            int file_idx = trie_search(trie_root, filename);
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check read access
            pthread_mutex_lock(&file_list_mutex);
            if (!has_file_access(&file_list[file_idx], username)) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Access denied\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            int ss_index = file_list[file_idx].ss_index;
            pthread_mutex_unlock(&file_list_mutex);
            
            // Get storage server info
            pthread_mutex_lock(&ss_list_mutex);
            char ss_ip[16];
            int ss_client_port;
            strncpy(ss_ip, ss_list[ss_index].ip, sizeof(ss_ip) - 1);
            ss_client_port = ss_list[ss_index].client_port;
            pthread_mutex_unlock(&ss_list_mutex);
            
            // Redirect client to storage server
            char redirect_msg[MAX_MSG_SIZE];
            snprintf(redirect_msg, sizeof(redirect_msg), "REDIRECT_VIEWCHECKPOINT %s %d %s", 
                    ss_ip, ss_client_port, tag);
            write(client_fd, redirect_msg, strlen(redirect_msg));
        }
        // Handle REVERT command
        else if (strncmp(buffer, "REVERT ", 7) == 0) {
            char filename[100], tag[MAX_TAG_LENGTH];
            if (sscanf(buffer, "REVERT %s %s", filename, tag) != 2) {
                const char* error_msg = "ERROR: Invalid REVERT format. Use: REVERT <filename> <tag>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Find file
            int file_idx = trie_search(trie_root, filename);
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check write access
            pthread_mutex_lock(&file_list_mutex);
            if (!has_write_access(&file_list[file_idx], username)) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Write access required to revert\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            int ss_index = file_list[file_idx].ss_index;
            pthread_mutex_unlock(&file_list_mutex);
            
            // Get storage server info
            pthread_mutex_lock(&ss_list_mutex);
            char ss_ip[16];
            int ss_client_port;
            strncpy(ss_ip, ss_list[ss_index].ip, sizeof(ss_ip) - 1);
            ss_client_port = ss_list[ss_index].client_port;
            pthread_mutex_unlock(&ss_list_mutex);
            
            // Redirect client to storage server
            char redirect_msg[MAX_MSG_SIZE];
            snprintf(redirect_msg, sizeof(redirect_msg), "REDIRECT_REVERT %s %d %s", 
                    ss_ip, ss_client_port, tag);
            write(client_fd, redirect_msg, strlen(redirect_msg));
        }
        // Handle INFO command
        else if (strncmp(buffer, "INFO ", 5) == 0) {
            char filename[100];
            if (sscanf(buffer, "INFO %s", filename) != 1) {
                const char* error_msg = "ERROR: Invalid INFO format. Use: INFO <filename>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Look up file using LRU cache + Trie
            int file_idx = lru_get(&lru_cache, filename);
            if (file_idx < 0) {
                file_idx = trie_search(trie_root, filename);
                if (file_idx >= 0) {
                    lru_put(&lru_cache, filename, file_idx);
                }
            }
            
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Build info response matching spec Example 6
            pthread_mutex_lock(&file_list_mutex);
            
            // Get fresh file statistics from storage server
            int word_count = 0, char_count = 0;
            long file_size = 0;
            time_t created, modified, accessed;
            
            if (get_file_stats(file_list[file_idx].filename, file_list[file_idx].ss_index,
                              &word_count, &char_count, &file_size,
                              &created, &modified, &accessed)) {
                file_list[file_idx].file_size = file_size;
                file_list[file_idx].accessed_time = accessed;
                file_list[file_idx].modified_time = modified;
                
                // Use the file system's creation time if:
                // 1. We don't have a creation time yet, OR
                // 2. The file system time is older (file existed before registration)
                if (file_list[file_idx].created_time == 0 || created < file_list[file_idx].created_time) {
                    file_list[file_idx].created_time = created;
                }
                
                // Update last accessed by
                strncpy(file_list[file_idx].last_accessed_by, username, 
                       sizeof(file_list[file_idx].last_accessed_by) - 1);
            }
            
            char info_response[MAX_MSG_SIZE];
            int off = 0;
            
            // Basic file info
            off += snprintf(info_response + off, sizeof(info_response) - off, 
                           "--> File: %s\n", file_list[file_idx].filename);
            off += snprintf(info_response + off, sizeof(info_response) - off, 
                           "--> Owner: %s\n", file_list[file_idx].owner);
            
            // Format timestamps
            char created_str[64], modified_str[64], accessed_str[64];
            struct tm* tm_info;
            
            tm_info = localtime(&file_list[file_idx].created_time);
            strftime(created_str, sizeof(created_str), "%Y-%m-%d %H:%M", tm_info);
            
            tm_info = localtime(&file_list[file_idx].modified_time);
            strftime(modified_str, sizeof(modified_str), "%Y-%m-%d %H:%M", tm_info);
            
            tm_info = localtime(&file_list[file_idx].accessed_time);
            strftime(accessed_str, sizeof(accessed_str), "%Y-%m-%d %H:%M", tm_info);
            
            off += snprintf(info_response + off, sizeof(info_response) - off, 
                           "--> Created: %s\n", created_str);
            off += snprintf(info_response + off, sizeof(info_response) - off, 
                           "--> Last Modified: %s\n", modified_str);
            off += snprintf(info_response + off, sizeof(info_response) - off, 
                           "--> Size: %ld bytes\n", file_list[file_idx].file_size);
            
            // Access list with permissions
            off += snprintf(info_response + off, sizeof(info_response) - off, 
                           "--> Access: %s (RW)", file_list[file_idx].owner);
            
            for (int i = 0; i < file_list[file_idx].access_count; i++) {
                const char* perm = (file_list[file_idx].access_permissions[i] == 2) ? "RW" : "R";
                off += snprintf(info_response + off, sizeof(info_response) - off,
                               ", %s (%s)", file_list[file_idx].access_list[i], perm);
            }
            off += snprintf(info_response + off, sizeof(info_response) - off, "\n");
            
            const char* last_user = (strlen(file_list[file_idx].last_accessed_by) > 0) ? 
                                    file_list[file_idx].last_accessed_by : file_list[file_idx].owner;
            off += snprintf(info_response + off, sizeof(info_response) - off, 
                           "--> Last Accessed: %s by %s\n", accessed_str, last_user);
            
            pthread_mutex_unlock(&file_list_mutex);
            
            write(client_fd, info_response, strlen(info_response));
        }
        // Handle ADDACCESS command
        else if (strncmp(buffer, "ADDACCESS ", 10) == 0) {
            char flag[10], filename[100], target_user[100];
            int access_type = 2;  // Default to WRITE access (which includes READ)
            
            // Parse command - try with flag first
            int parsed = sscanf(buffer, "ADDACCESS %s %s %s", flag, filename, target_user);
            
            if (parsed == 3) {
                // Flag provided
                if (strcmp(flag, "-R") == 0 || strcmp(flag, "-r") == 0) {
                    access_type = 1;  // READ only
                } else if (strcmp(flag, "-W") == 0 || strcmp(flag, "-w") == 0) {
                    access_type = 2;  // WRITE (includes READ)
                } else {
                    const char* error_msg = "ERROR: Invalid flag. Use -R for read or -W for write\n";
                    write(client_fd, error_msg, strlen(error_msg));
                    continue;
                }
            } else if (parsed == 2) {
                // No flag - old format: ADDACCESS <filename> <username>
                strcpy(target_user, filename);
                strcpy(filename, flag);
                access_type = 2;  // Default to write
            } else {
                const char* error_msg = "ERROR: Invalid ADDACCESS format. Use: ADDACCESS -R|-W <filename> <username>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Look up file using LRU cache + Trie
            int file_idx = lru_get(&lru_cache, filename);
            if (file_idx < 0) {
                file_idx = trie_search(trie_root, filename);
                if (file_idx >= 0) {
                    lru_put(&lru_cache, filename, file_idx);
                }
            }
            
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check if requester is the owner
            pthread_mutex_lock(&file_list_mutex);
            if (strcmp(file_list[file_idx].owner, username) != 0) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Only the owner can modify access\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check if user already has access
            int existing_index = -1;
            for (int i = 0; i < file_list[file_idx].access_count; i++) {
                if (strcmp(file_list[file_idx].access_list[i], target_user) == 0) {
                    existing_index = i;
                    break;
                }
            }
            
            if (existing_index >= 0) {
                // User already has access - check if we're upgrading or downgrading
                int current_perm = file_list[file_idx].access_permissions[existing_index];
                
                if (current_perm == access_type) {
                    pthread_mutex_unlock(&file_list_mutex);
                    const char* error_msg = "ERROR: User already has this access level\n";
                    write(client_fd, error_msg, strlen(error_msg));
                    continue;
                }
                
                // Update the permission (upgrade READ->WRITE or downgrade WRITE->READ)
                file_list[file_idx].access_permissions[existing_index] = access_type;
                pthread_mutex_unlock(&file_list_mutex);
                
                // Journal the operation
                char journal_entry[256];
                snprintf(journal_entry, sizeof(journal_entry), "MODACCESS %s %s %d",
                         filename, target_user, access_type);
                journal_operation(journal_entry);
                
                const char* action = (access_type > current_perm) ? "upgraded" : "downgraded";
                printf("✓ Access %s: %s now has %s access to %s\n", 
                       action, target_user,
                       access_type == 1 ? "READ" : "WRITE",
                       filename);
                const char* ack_msg = "ACK_SUCCESS\n";
                write(client_fd, ack_msg, strlen(ack_msg));
                continue;
            }
            
            // Add user to access list (new user)
            if (file_list[file_idx].access_count >= MAX_ACCESS_USERS) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Access list is full\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            strncpy(file_list[file_idx].access_list[file_list[file_idx].access_count],
                    target_user, 100);
            file_list[file_idx].access_list[file_list[file_idx].access_count][99] = '\0';
            file_list[file_idx].access_permissions[file_list[file_idx].access_count] = access_type;
            file_list[file_idx].access_count++;
            
            pthread_mutex_unlock(&file_list_mutex);
            
            // Journal the operation for persistence
            char journal_entry[256];
            snprintf(journal_entry, sizeof(journal_entry), "ADDACCESS %s %s %d",
                     filename, target_user, access_type);
            journal_operation(journal_entry);
            
            printf("✓ Added %s access: %s can now %s %s\n", 
                   access_type == 1 ? "READ" : "WRITE",
                   target_user, 
                   access_type == 1 ? "read" : "read/write",
                   filename);
            const char* ack_msg = "ACK_SUCCESS\n";
            write(client_fd, ack_msg, strlen(ack_msg));
        }
        // Handle REMACCESS command
        else if (strncmp(buffer, "REMACCESS ", 10) == 0) {
            char filename[100], target_user[100];
            if (sscanf(buffer, "REMACCESS %s %s", filename, target_user) != 2) {
                const char* error_msg = "ERROR: Invalid REMACCESS format. Use: REMACCESS <filename> <username>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Look up file using LRU cache + Trie
            int file_idx = lru_get(&lru_cache, filename);
            if (file_idx < 0) {
                file_idx = trie_search(trie_root, filename);
                if (file_idx >= 0) {
                    lru_put(&lru_cache, filename, file_idx);
                }
            }
            
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check if requester is the owner
            pthread_mutex_lock(&file_list_mutex);
            if (strcmp(file_list[file_idx].owner, username) != 0) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Only the owner can modify access\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Find and remove user from access list
            int found_user = 0;
            for (int i = 0; i < file_list[file_idx].access_count; i++) {
                if (strcmp(file_list[file_idx].access_list[i], target_user) == 0) {
                    found_user = 1;
                    // Shift remaining users down
                    for (int j = i; j < file_list[file_idx].access_count - 1; j++) {
                        strncpy(file_list[file_idx].access_list[j],
                                file_list[file_idx].access_list[j + 1], 100);
                    }
                    file_list[file_idx].access_count--;
                    break;
                }
            }
            
            pthread_mutex_unlock(&file_list_mutex);
            
            if (found_user) {
                // Journal the operation for persistence
                char journal_entry[256];
                snprintf(journal_entry, sizeof(journal_entry), "REMACCESS %s %s",
                         filename, target_user);
                journal_operation(journal_entry);
                
                printf("✓ Removed access: %s can no longer access %s\n", target_user, filename);
                const char* ack_msg = "ACK_SUCCESS\n";
                write(client_fd, ack_msg, strlen(ack_msg));
            } else {
                const char* error_msg = "ERROR: User not in access list\n";
                write(client_fd, error_msg, strlen(error_msg));
            }
        }
        // Handle REQACCESS command - Request access to a file
        else if (strncmp(buffer, "REQACCESS ", 10) == 0) {
            char filename[256];
            if (sscanf(buffer, "REQACCESS %s", filename) != 1) {
                const char* error_msg = "ERROR: Invalid REQACCESS format. Use: REQACCESS <filename>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Look up file
            int file_idx = lru_get(&lru_cache, filename);
            if (file_idx < 0) {
                file_idx = trie_search(trie_root, filename);
                if (file_idx >= 0) {
                    lru_put(&lru_cache, filename, file_idx);
                }
            }
            
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            pthread_mutex_lock(&file_list_mutex);
            
            // Check if user already has access
            if (has_file_access(&file_list[file_idx], username)) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* msg = "INFO: You already have access to this file\n";
                write(client_fd, msg, strlen(msg));
                continue;
            }
            
            // Check if request already exists
            int request_exists = 0;
            for (int i = 0; i < file_list[file_idx].request_count; i++) {
                if (file_list[file_idx].requests[i].active && 
                    strcmp(file_list[file_idx].requests[i].username, username) == 0) {
                    request_exists = 1;
                    break;
                }
            }
            
            if (request_exists) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* msg = "INFO: You already have a pending request for this file\n";
                write(client_fd, msg, strlen(msg));
                continue;
            }
            
            // Add request to queue
            if (file_list[file_idx].request_count >= MAX_ACCESS_REQUESTS) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Access request queue is full\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Find first inactive slot
            int slot = -1;
            for (int i = 0; i < MAX_ACCESS_REQUESTS; i++) {
                if (!file_list[file_idx].requests[i].active) {
                    slot = i;
                    break;
                }
            }
            
            if (slot >= 0) {
                strncpy(file_list[file_idx].requests[slot].username, username, 99);
                file_list[file_idx].requests[slot].username[99] = '\0';
                file_list[file_idx].requests[slot].request_time = time(NULL);
                file_list[file_idx].requests[slot].active = 1;
                file_list[file_idx].request_count++;
            }
            
            pthread_mutex_unlock(&file_list_mutex);
            
            printf("✓ Access request: %s → %s (owner: %s)\n", 
                   username, filename, file_list[file_idx].owner);
            const char* ack_msg = "ACK_SUCCESS: Access request sent to file owner\n";
            write(client_fd, ack_msg, strlen(ack_msg));
        }
        // Handle SHOW_REQUESTS command - Show pending access requests for owned files
        else if (strncmp(buffer, "SHOW_REQUESTS", 13) == 0) {
            char response[MAX_MSG_SIZE * 2];
            int offset = 0;
            int total_requests = 0;
            
            offset += snprintf(response + offset, sizeof(response) - offset,
                              "=== Pending Access Requests ===\n");
            
            pthread_mutex_lock(&file_list_mutex);
            for (int f = 0; f < file_count; f++) {
                // Only show requests for files owned by this user
                if (strcmp(file_list[f].owner, username) != 0) {
                    continue;
                }
                
                for (int i = 0; i < MAX_ACCESS_REQUESTS; i++) {
                    if (file_list[f].requests[i].active) {
                        total_requests++;
                        char time_str[64];
                        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S",
                                localtime(&file_list[f].requests[i].request_time));
                        
                        offset += snprintf(response + offset, sizeof(response) - offset,
                                          "%d. File: %s | User: %s | Time: %s\n",
                                          total_requests,
                                          file_list[f].full_path,
                                          file_list[f].requests[i].username,
                                          time_str);
                    }
                }
            }
            pthread_mutex_unlock(&file_list_mutex);
            
            if (total_requests == 0) {
                offset += snprintf(response + offset, sizeof(response) - offset,
                                  "No pending access requests.\n");
            }
            
            offset += snprintf(response + offset, sizeof(response) - offset, "\n");
            write(client_fd, response, offset);
        }
        // Handle APPROVE_REQ command - Approve an access request
        else if (strncmp(buffer, "APPROVE_REQ ", 12) == 0) {
            char filename[256], requester[100];
            if (sscanf(buffer, "APPROVE_REQ %s %s", filename, requester) != 2) {
                const char* error_msg = "ERROR: Invalid APPROVE_REQ format. Use: APPROVE_REQ <filename> <username>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Look up file
            int file_idx = lru_get(&lru_cache, filename);
            if (file_idx < 0) {
                file_idx = trie_search(trie_root, filename);
                if (file_idx >= 0) {
                    lru_put(&lru_cache, filename, file_idx);
                }
            }
            
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            pthread_mutex_lock(&file_list_mutex);
            
            // Check if requester is the owner
            if (strcmp(file_list[file_idx].owner, username) != 0) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Only the owner can approve access requests\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Find and remove the request
            int request_found = 0;
            for (int i = 0; i < MAX_ACCESS_REQUESTS; i++) {
                if (file_list[file_idx].requests[i].active && 
                    strcmp(file_list[file_idx].requests[i].username, requester) == 0) {
                    // Mark request as inactive
                    file_list[file_idx].requests[i].active = 0;
                    file_list[file_idx].request_count--;
                    request_found = 1;
                    break;
                }
            }
            
            if (!request_found) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: No pending request from that user for this file\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Grant READ access to the requester
            if (file_list[file_idx].access_count >= MAX_ACCESS_USERS) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Access list is full\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            strncpy(file_list[file_idx].access_list[file_list[file_idx].access_count],
                    requester, 99);
            file_list[file_idx].access_list[file_list[file_idx].access_count][99] = '\0';
            file_list[file_idx].access_permissions[file_list[file_idx].access_count] = ACCESS_READ;
            file_list[file_idx].access_count++;
            
            pthread_mutex_unlock(&file_list_mutex);
            
            printf("✓ Approved: %s granted READ access to %s\n", requester, filename);
            const char* ack_msg = "ACK_SUCCESS: Access granted\n";
            write(client_fd, ack_msg, strlen(ack_msg));
        }
        // Handle EXEC command
        else if (strncmp(buffer, "EXEC ", 5) == 0) {
            char filename[100];
            if (sscanf(buffer, "EXEC %s", filename) != 1) {
                const char* error_msg = "ERROR: Invalid EXEC format. Use: EXEC <filename>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Look up file using LRU cache + Trie
            int file_idx = lru_get(&lru_cache, filename);
            if (file_idx < 0) {
                file_idx = trie_search(trie_root, filename);
                if (file_idx >= 0) {
                    lru_put(&lru_cache, filename, file_idx);
                }
            }
            
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check access control
            pthread_mutex_lock(&file_list_mutex);
            if (!has_file_access(&file_list[file_idx], username)) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Access denied\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            int ss_index = file_list[file_idx].ss_index;
            pthread_mutex_unlock(&file_list_mutex);
            
            // Get Storage Server information
            pthread_mutex_lock(&ss_list_mutex);
            if (ss_index < 0 || ss_index >= ss_count) {
                pthread_mutex_unlock(&ss_list_mutex);
                const char* error_msg = "ERROR: Invalid storage server index\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            char ss_ip[16];
            int ss_client_port;
            strncpy(ss_ip, ss_list[ss_index].ip, sizeof(ss_ip) - 1);
            ss_ip[sizeof(ss_ip) - 1] = '\0';
            ss_client_port = ss_list[ss_index].client_port;
            pthread_mutex_unlock(&ss_list_mutex);
            
            printf("Fetching file '%s' from Storage Server %d for execution...\n", filename, ss_index);
            
            // Connect to Storage Server as a client
            int ss_fd = connect_to_server(ss_ip, ss_client_port);
            if (ss_fd < 0) {
                fprintf(stderr, "Failed to connect to Storage Server\n");
                const char* error_msg = "ERROR: Cannot connect to Storage Server\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Request file content from SS
            char ss_command[MAX_MSG_SIZE];
            snprintf(ss_command, sizeof(ss_command), "READ_FILE %s\n", filename);
            write(ss_fd, ss_command, strlen(ss_command));
            
            // Read file content from SS
            char file_content[MAX_MSG_SIZE];
            memset(file_content, 0, sizeof(file_content));
            ssize_t content_bytes = read(ss_fd, file_content, sizeof(file_content) - 1);
            close(ss_fd);
            
            if (content_bytes <= 0) {
                const char* error_msg = "ERROR: Failed to fetch file from Storage Server\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            file_content[content_bytes] = '\0';
            
            // Check for errors from SS
            if (strncmp(file_content, "ERROR:", 6) == 0) {
                write(client_fd, file_content, strlen(file_content));
                continue;
            }
            
            // Save to temporary file
            char temp_filename[] = "/tmp/langos_exec_XXXXXX";
            int temp_fd = mkstemp(temp_filename);
            if (temp_fd < 0) {
                perror("Failed to create temporary file");
                const char* error_msg = "ERROR: Failed to create temporary file\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Write script content to temp file
            write(temp_fd, file_content, content_bytes);
            close(temp_fd);
            
            // Make it executable
            chmod(temp_filename, 0700);
            
            printf("Executing script: %s\n", temp_filename);
            
            // Execute using popen
            char exec_command[512];
            snprintf(exec_command, sizeof(exec_command), "sh %s 2>&1", temp_filename);
            FILE* pipe = popen(exec_command, "r");
            
            if (pipe == NULL) {
                perror("Failed to execute script");
                const char* error_msg = "ERROR: Failed to execute script\n";
                write(client_fd, error_msg, strlen(error_msg));
                remove(temp_filename);
                continue;
            }
            
            // Read output from execution
            char exec_output[MAX_MSG_SIZE * 2];
            memset(exec_output, 0, sizeof(exec_output));
            size_t output_len = 0;
            
            char line[256];
            while (fgets(line, sizeof(line), pipe) != NULL && output_len < sizeof(exec_output) - 256) {
                size_t line_len = strlen(line);
                memcpy(exec_output + output_len, line, line_len);
                output_len += line_len;
            }
            
            int exit_status = pclose(pipe);
            
            // Clean up temporary file
            remove(temp_filename);
            
            // Send execution output back to client
            char response[MAX_MSG_SIZE * 2];
            snprintf(response, sizeof(response), 
                     "=== Execution Output ===\n%s\n=== Exit Status: %d ===\n",
                     output_len > 0 ? exec_output : "(no output)", 
                     WEXITSTATUS(exit_status));
            
            write(client_fd, response, strlen(response));
            
            printf("✓ Executed '%s' for user '%s' (exit: %d)\n", 
                   filename, username, WEXITSTATUS(exit_status));
        }
        // Handle VIEWFOLDER command
        else if (strncmp(buffer, "VIEWFOLDER ", 11) == 0) {
            char foldername[256];
            if (sscanf(buffer, "VIEWFOLDER %s", foldername) != 1) {
                const char* error_msg = "ERROR: Invalid VIEWFOLDER format. Use: VIEWFOLDER <foldername>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check if folder exists
            int folder_idx = trie_search(trie_root, foldername);
            if (folder_idx < 0 || !file_list[folder_idx].is_folder) {
                const char* error_msg = "ERROR: Folder not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            char folder_response[MAX_MSG_SIZE * 2];
            memset(folder_response, 0, sizeof(folder_response));
            int offset = 0;
            
            offset += snprintf(folder_response + offset, sizeof(folder_response) - offset,
                             "Contents of folder '%s':\n", foldername);
            offset += snprintf(folder_response + offset, sizeof(folder_response) - offset,
                             "-------------------------------------------\n");
            
            int count = 0;
            pthread_mutex_lock(&file_list_mutex);
            
            // Build folder path prefix
            char folder_prefix[270];
            snprintf(folder_prefix, sizeof(folder_prefix), "%s/", foldername);
            
            for (int i = 0; i < file_count && offset < (int)sizeof(folder_response) - 100; i++) {
                // Check if file/folder is inside this folder
                if (strncmp(file_list[i].full_path, folder_prefix, strlen(folder_prefix)) == 0) {
                    // Check if it's a direct child (not nested deeper)
                    const char* remaining = file_list[i].full_path + strlen(folder_prefix);
                    if (strchr(remaining, '/') == NULL) {  // No more slashes = direct child
                        count++;
                        if (file_list[i].is_folder) {
                            offset += snprintf(folder_response + offset, sizeof(folder_response) - offset,
                                             "[DIR]  %s/\n", file_list[i].filename);
                        } else {
                            offset += snprintf(folder_response + offset, sizeof(folder_response) - offset,
                                             "[FILE] %s (owner: %s)\n", 
                                             file_list[i].filename, file_list[i].owner);
                        }
                    }
                }
            }
            pthread_mutex_unlock(&file_list_mutex);
            
            if (count == 0) {
                offset += snprintf(folder_response + offset, sizeof(folder_response) - offset,
                                 "(empty folder)\n");
            }
            
            offset += snprintf(folder_response + offset, sizeof(folder_response) - offset,
                             "-------------------------------------------\n");
            offset += snprintf(folder_response + offset, sizeof(folder_response) - offset,
                             "Total: %d items\n", count);
            
            write(client_fd, folder_response, strlen(folder_response));
        }
        // Handle VIEW command with flags
        else if (strncmp(buffer, "VIEW", 4) == 0) {
            char flags[10] = "";
            int show_all = 0;
            int show_details = 0;
            
            // Parse flags
            if (sscanf(buffer, "VIEW %s", flags) == 1) {
                if (strstr(flags, "a") != NULL || strstr(flags, "A") != NULL) {
                    show_all = 1;
                }
                if (strstr(flags, "l") != NULL || strstr(flags, "L") != NULL) {
                    show_details = 1;
                }
            }
            
            char view_response[MAX_MSG_SIZE * 2];
            memset(view_response, 0, sizeof(view_response));
            int offset = 0;
            
            pthread_mutex_lock(&file_list_mutex);
            
            if (show_details) {
                // Table header matching spec format
                offset += snprintf(view_response + offset, sizeof(view_response) - offset,
                                 "---------------------------------------------------------\n");
                offset += snprintf(view_response + offset, sizeof(view_response) - offset,
                                 "|  Filename  | Words | Chars | Last Access Time | Owner |\n");
                offset += snprintf(view_response + offset, sizeof(view_response) - offset,
                                 "|------------|-------|-------|------------------|-------|\n");
            }
            
            for (int i = 0; i < file_count && offset < (int)sizeof(view_response) - 200; i++) {
                // Check access
                int has_access = show_all || has_file_access(&file_list[i], username);
                
                if (!has_access) continue;
                
                if (show_details) {
                    // For folders, show as directory
                    if (file_list[i].is_folder) {
                        struct tm* tm_info = localtime(&file_list[i].created_time);
                        char timestamp[20];
                        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M", tm_info);
                        
                        offset += snprintf(view_response + offset, sizeof(view_response) - offset,
                                         "| %-10s | %-5s | %-5s | %-16s | %-5s |\n",
                                         file_list[i].filename,
                                         "<DIR>", "<DIR>",
                                         timestamp,
                                         file_list[i].owner);
                        continue;
                    }
                    
                    // Get file statistics from storage server
                    int word_count = 0;
                    int char_count = 0;
                    long file_size = 0;
                    time_t created, modified, accessed;
                    char word_str[10] = "N/A";
                    char char_str[10] = "N/A";
                    
                    if (get_file_stats(file_list[i].filename, file_list[i].ss_index, 
                                      &word_count, &char_count, &file_size, 
                                      &created, &modified, &accessed)) {
                        snprintf(word_str, sizeof(word_str), "%d", word_count);
                        snprintf(char_str, sizeof(char_str), "%d", char_count);
                        
                        // Update file metadata
                        file_list[i].file_size = file_size;
                        file_list[i].accessed_time = accessed;
                        file_list[i].modified_time = modified;
                        if (file_list[i].created_time == 0) {
                            file_list[i].created_time = created;
                        }
                    }
                    
                    // Format access time for display
                    struct tm* tm_info = localtime(&file_list[i].accessed_time);
                    char timestamp[20];
                    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M", tm_info);
                    
                    // Format: |  Filename  | Words | Chars | Last Access Time | Owner |
                    offset += snprintf(view_response + offset, sizeof(view_response) - offset,
                                     "| %-10s | %-5s | %-5s | %-16s | %-5s |\n",
                                     file_list[i].filename,
                                     word_str, char_str,
                                     timestamp,
                                     file_list[i].owner);
                } else {
                    // Simple format: --> filename (or foldername/)
                    if (file_list[i].is_folder) {
                        offset += snprintf(view_response + offset, sizeof(view_response) - offset,
                                         "--> %s/ (folder)\n", file_list[i].filename);
                    } else {
                        offset += snprintf(view_response + offset, sizeof(view_response) - offset,
                                         "--> %s\n", file_list[i].filename);
                    }
                }
            }
            
            if (show_details && offset > 100) {
                offset += snprintf(view_response + offset, sizeof(view_response) - offset,
                                 "---------------------------------------------------------\n");
            }
            
            pthread_mutex_unlock(&file_list_mutex);
            
            if (offset == 0 || (show_details && offset <= 100)) {
                const char* empty_msg = "No files available\n";
                write(client_fd, empty_msg, strlen(empty_msg));
            } else {
                write(client_fd, view_response, strlen(view_response));
            }
        }
        // Handle MOVE command
        else if (strncmp(buffer, "MOVE ", 5) == 0) {
            char filename[100], foldername[256];
            if (sscanf(buffer, "MOVE %s %s", filename, foldername) != 2) {
                const char* error_msg = "ERROR: Invalid MOVE format. Use: MOVE <filename> <foldername>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Find the file
            int file_idx = lru_get(&lru_cache, filename);
            if (file_idx < 0) {
                file_idx = trie_search(trie_root, filename);
            }
            
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check if user is owner
            pthread_mutex_lock(&file_list_mutex);
            if (strcmp(file_list[file_idx].owner, username) != 0) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Only the owner can move files\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check if target is a folder
            int folder_idx = trie_search(trie_root, foldername);
            if (folder_idx < 0 || !file_list[folder_idx].is_folder) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Target folder not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Build new path
            char new_path[MAX_PATH_LENGTH];
            snprintf(new_path, sizeof(new_path), "%s/%s", foldername, file_list[file_idx].filename);
            
            // Update the file's full path
            strncpy(file_list[file_idx].full_path, new_path, sizeof(file_list[file_idx].full_path) - 1);
            file_list[file_idx].full_path[sizeof(file_list[file_idx].full_path) - 1] = '\0';
            
            pthread_mutex_unlock(&file_list_mutex);
            
            // Journal the operation
            char journal_entry[600];
            snprintf(journal_entry, sizeof(journal_entry), "MOVE %s %s %s", filename, foldername, username);
            journal_operation(journal_entry);
            
            printf("✓ Moved file '%s' to folder '%s'\n", filename, foldername);
            
            const char* ack_msg = "ACK_SUCCESS: File moved\n";
            write(client_fd, ack_msg, strlen(ack_msg));
        }
        // Handle DELETE command
        else if (strncmp(buffer, "DELETE ", 7) == 0) {
            char filename[100];
            if (sscanf(buffer, "DELETE %s", filename) != 1) {
                const char* error_msg = "ERROR: Invalid DELETE format. Use: DELETE <filename>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Look up file
            int file_idx = lru_get(&lru_cache, filename);
            if (file_idx < 0) {
                file_idx = trie_search(trie_root, filename);
            }
            
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check if requester is the owner
            pthread_mutex_lock(&file_list_mutex);
            if (strcmp(file_list[file_idx].owner, username) != 0) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Only the owner can delete the file\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            int ss_index = file_list[file_idx].ss_index;
            pthread_mutex_unlock(&file_list_mutex);
            
            // Get Storage Server information
            pthread_mutex_lock(&ss_list_mutex);
            if (ss_index < 0 || ss_index >= ss_count) {
                pthread_mutex_unlock(&ss_list_mutex);
                const char* error_msg = "ERROR: Invalid storage server index\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            char ss_ip[16];
            int ss_port;
            strncpy(ss_ip, ss_list[ss_index].ip, sizeof(ss_ip) - 1);
            ss_ip[sizeof(ss_ip) - 1] = '\0';
            ss_port = ss_list[ss_index].ns_port;
            pthread_mutex_unlock(&ss_list_mutex);
            
            printf("Deleting file '%s' from Storage Server %d...\n", filename, ss_index);
            
            // Connect to Storage Server
            int ss_fd = connect_to_server(ss_ip, ss_port);
            if (ss_fd < 0) {
                fprintf(stderr, "Failed to connect to Storage Server\n");
                const char* error_msg = "ERROR: Cannot connect to Storage Server\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Send DELETE_FILE command to SS
            char ss_command[MAX_MSG_SIZE];
            snprintf(ss_command, sizeof(ss_command), "DELETE_FILE %s\n", filename);
            write(ss_fd, ss_command, strlen(ss_command));
            
            // Wait for response from SS
            char ss_response[MAX_MSG_SIZE];
            memset(ss_response, 0, sizeof(ss_response));
            ssize_t ss_bytes = read(ss_fd, ss_response, sizeof(ss_response) - 1);
            close(ss_fd);
            
            if (ss_bytes <= 0 || strncmp(ss_response, "ACK_SUCCESS", 11) != 0) {
                fprintf(stderr, "Storage Server failed to delete file\n");
                const char* error_msg = "ERROR: Storage Server failed to delete file\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Remove file from file list
            pthread_mutex_lock(&file_list_mutex);
            for (int i = file_idx; i < file_count - 1; i++) {
                file_list[i] = file_list[i + 1];
            }
            file_count--;
            pthread_mutex_unlock(&file_list_mutex);
            
            // Journal the operation for persistence
            char journal_entry[256];
            snprintf(journal_entry, sizeof(journal_entry), "DELETE %s", filename);
            journal_operation(journal_entry);
            
            printf("✓ File '%s' deleted successfully\n", filename);
            const char* ack_msg = "ACK_SUCCESS: File deleted\n";
            write(client_fd, ack_msg, strlen(ack_msg));
        }
        // Handle STREAM command
        else if (strncmp(buffer, "STREAM ", 7) == 0) {
            char filename[100];
            if (sscanf(buffer, "STREAM %s", filename) != 1) {
                const char* error_msg = "ERROR: Invalid STREAM format. Use: STREAM <filename>\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Look up file using LRU cache + Trie
            int file_idx = lru_get(&lru_cache, filename);
            if (file_idx < 0) {
                file_idx = trie_search(trie_root, filename);
                if (file_idx >= 0) {
                    lru_put(&lru_cache, filename, file_idx);
                }
            }
            
            if (file_idx < 0) {
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Check access control
            pthread_mutex_lock(&file_list_mutex);
            if (!has_file_access(&file_list[file_idx], username)) {
                pthread_mutex_unlock(&file_list_mutex);
                const char* error_msg = "ERROR: Access denied\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            int ss_index = file_list[file_idx].ss_index;
            pthread_mutex_unlock(&file_list_mutex);
            
            // Get Storage Server information
            pthread_mutex_lock(&ss_list_mutex);
            if (ss_index < 0 || ss_index >= ss_count) {
                pthread_mutex_unlock(&ss_list_mutex);
                const char* error_msg = "ERROR: Invalid storage server index\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            char ss_ip[16];
            int ss_client_port;
            strncpy(ss_ip, ss_list[ss_index].ip, sizeof(ss_ip) - 1);
            ss_ip[sizeof(ss_ip) - 1] = '\0';
            ss_client_port = ss_list[ss_index].client_port;
            pthread_mutex_unlock(&ss_list_mutex);
            
            printf("Redirecting client to Storage Server %d (%s:%d) for STREAM of '%s'\n",
                   ss_index, ss_ip, ss_client_port, filename);
            
            // Send REDIRECT message to client
            char redirect_msg[MAX_MSG_SIZE];
            snprintf(redirect_msg, sizeof(redirect_msg), "REDIRECT %s %d\n", ss_ip, ss_client_port);
            write(client_fd, redirect_msg, strlen(redirect_msg));
        }
        else {
            // Unknown command
            const char* error_msg = "ERROR: Unknown command\n";
            write(client_fd, error_msg, strlen(error_msg));
        }
    }
    
    // Note: We don't remove clients from the list to avoid index issues
    // with concurrent threads. The list persists for the session.
    // Alternative: Mark client as disconnected with a flag
    
    close(client_fd);
    return NULL;
}

