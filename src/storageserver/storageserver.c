#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _BSD_SOURCE
#include "common.h"
#include "socket_utils.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

// File lock structure for sentence-level locking
#define MAX_LOCKS 100
#define MAX_CHECKPOINTS_PER_FILE 10
#define MAX_TAG_LENGTH 50

typedef struct {
    char filename[100];
    int sentence_num;
    pthread_mutex_t lock;
    int in_use;
} FileLock;

// Checkpoint metadata structure
typedef struct {
    char filename[256];
    char tag[MAX_TAG_LENGTH];
    char checkpoint_path[300];  // Path to checkpoint file
    time_t created_time;
    char created_by[100];
} CheckpointInfo;

// Global lock manager
FileLock lock_table[MAX_LOCKS];
pthread_mutex_t lock_table_mutex = PTHREAD_MUTEX_INITIALIZER;

// Global checkpoint tracking
CheckpointInfo checkpoint_registry[1000];
int checkpoint_count = 0;
pthread_mutex_t checkpoint_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function prototypes
void* handle_ns_command(void* arg);
void* handle_client_request(void* arg);
void* client_listener_thread(void* arg);
int lock_sentence(char* filename, int sent_num);
void unlock_sentence(char* filename, int sent_num);
char* get_sentence(char* file_content, int sent_num);
void update_sentence(char* filename, int sent_num, int word_index, char* new_content);
int count_complete_sentences(char* file_content);
void process_escape_sequences(char* str);

// Process escape sequences in a string (e.g., \n -> newline, \t -> tab)
void process_escape_sequences(char* str) {
    char* src = str;
    char* dst = str;
    
    while (*src != '\0') {
        if (*src == '\\' && *(src + 1) != '\0') {
            src++;  // Move past the backslash
            switch (*src) {
                case 'n':
                    *dst++ = '\n';
                    break;
                case 't':
                    *dst++ = '\t';
                    break;
                case 'r':
                    *dst++ = '\r';
                    break;
                case '\\':
                    *dst++ = '\\';
                    break;
                case '"':
                    *dst++ = '"';
                    break;
                case '\'':
                    *dst++ = '\'';
                    break;
                default:
                    // Unknown escape sequence, keep the backslash
                    *dst++ = '\\';
                    *dst++ = *src;
                    break;
            }
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Lock a specific sentence in a file
// Returns: 0 on success, -1 if previous sentence incomplete, -2 if lock table full, -3 if empty file needs sentence 0
int lock_sentence(char* filename, int sent_num) {
    // Check file content
    FILE* file = fopen(filename, "r");
    if (file != NULL) {
        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        fseek(file, 0, SEEK_SET);
        
        // If file is empty or only has whitespace, must start at sentence 0
        if (file_size == 0 && sent_num != 0) {
            fclose(file);
            fprintf(stderr, "ERROR: File is empty. Must start writing at sentence 0, not sentence %d\n", sent_num);
            return -3;
        }
        
        if (file_size > 0) {
            char* file_content = (char*)malloc(file_size + 1);
            if (file_content != NULL) {
                size_t read_size = fread(file_content, 1, file_size, file);
                file_content[read_size] = '\0';
                fclose(file);
                
                // Check if file only contains whitespace
                int only_whitespace = 1;
                for (size_t i = 0; i < read_size; i++) {
                    if (file_content[i] != ' ' && file_content[i] != '\t' && 
                        file_content[i] != '\n' && file_content[i] != '\r') {
                        only_whitespace = 0;
                        break;
                    }
                }
                
                if (only_whitespace && sent_num != 0) {
                    free(file_content);
                    fprintf(stderr, "ERROR: File is empty. Must start writing at sentence 0, not sentence %d\n", sent_num);
                    return -3;
                }
                
                // Validate: if trying to write sentence N (N > 0), ensure sentence N-1 is complete
                if (sent_num > 0) {
                    int complete_sentences = count_complete_sentences(file_content);
                    
                    // To write to sentence N, we need at least N complete sentences
                    // (sentences 0 through N-1 must be complete)
                    if (complete_sentences < sent_num) {
                        free(file_content);
                        fprintf(stderr, "ERROR: Cannot write to sentence %d. Previous sentence %d is not complete (missing '.', '!', or '?')\n", 
                                sent_num, sent_num - 1);
                        return -1;
                    }
                }
                
                free(file_content);
            } else {
                fclose(file);
            }
        } else {
            fclose(file);
        }
    }
    
    pthread_mutex_lock(&lock_table_mutex);
    
    // Find existing lock
    for (int i = 0; i < MAX_LOCKS; i++) {
        if (lock_table[i].in_use &&
            strcmp(lock_table[i].filename, filename) == 0 &&
            lock_table[i].sentence_num == sent_num) {
            pthread_mutex_unlock(&lock_table_mutex);
            pthread_mutex_lock(&lock_table[i].lock);
            return 0;
        }
    }
    
    // Create new lock
    for (int i = 0; i < MAX_LOCKS; i++) {
        if (!lock_table[i].in_use) {
            strncpy(lock_table[i].filename, filename, sizeof(lock_table[i].filename) - 1);
            lock_table[i].filename[sizeof(lock_table[i].filename) - 1] = '\0';
            lock_table[i].sentence_num = sent_num;
            pthread_mutex_init(&lock_table[i].lock, NULL);
            lock_table[i].in_use = 1;
            pthread_mutex_unlock(&lock_table_mutex);
            pthread_mutex_lock(&lock_table[i].lock);
            printf("Created lock for %s, sentence %d\n", filename, sent_num);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&lock_table_mutex);
    fprintf(stderr, "ERROR: Lock table full!\n");
    return -2;
}

// Unlock a specific sentence in a file
void unlock_sentence(char* filename, int sent_num) {
    pthread_mutex_lock(&lock_table_mutex);
    
    for (int i = 0; i < MAX_LOCKS; i++) {
        if (lock_table[i].in_use &&
            strcmp(lock_table[i].filename, filename) == 0 &&
            lock_table[i].sentence_num == sent_num) {
            pthread_mutex_unlock(&lock_table[i].lock);
            printf("Unlocked %s, sentence %d\n", filename, sent_num);
            pthread_mutex_unlock(&lock_table_mutex);
            return;
        }
    }
    
    pthread_mutex_unlock(&lock_table_mutex);
    fprintf(stderr, "WARNING: Lock not found for %s, sentence %d\n", filename, sent_num);
}

// Count completed sentences in file content
// A sentence is complete if it ends with '.', '!', or '?'
int count_complete_sentences(char* file_content) {
    if (file_content == NULL || strlen(file_content) == 0) {
        return 0;
    }
    
    int count = 0;
    char* ptr = file_content;
    
    while (*ptr != '\0') {
        if (*ptr == '.' || *ptr == '!' || *ptr == '?') {
            count++;
        }
        ptr++;
    }
    
    return count;
}

// Get pointer to the Nth sentence in file content
// Sentences are delimited by '.', '!', or '?'
char* get_sentence(char* file_content, int sent_num) {
    if (file_content == NULL || sent_num < 0) {
        return NULL;
    }
    
    int current_sentence = 0;
    char* ptr = file_content;
    
    // If requesting sentence 0 and file is empty or starts immediately
    if (sent_num == 0) {
        return file_content;
    }
    
    // Find the start of the requested sentence
    while (*ptr != '\0') {
        if (*ptr == '.' || *ptr == '!' || *ptr == '?') {
            current_sentence++;
            ptr++;
            // Skip whitespace after delimiter
            while (*ptr == ' ' || *ptr == '\t' || *ptr == '\n') {
                ptr++;
            }
            
            if (current_sentence == sent_num) {
                return ptr;
            }
        } else {
            ptr++;
        }
    }
    
    // If we didn't find enough sentences, return NULL
    return NULL;
}

// Update a specific word in a specific sentence
void update_sentence(char* filename, int sent_num, int word_index, char* new_content) {
    // Process escape sequences in the new content (e.g., \n -> newline)
    process_escape_sequences(new_content);
    
    // Read the entire file
    FILE* file = fopen(filename, "r");
    if (file == NULL) {
        fprintf(stderr, "ERROR: Cannot open file %s for reading\n", filename);
        return;
    }
    
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* file_content = (char*)malloc(file_size + 1);
    if (file_content == NULL) {
        fclose(file);
        fprintf(stderr, "ERROR: Memory allocation failed\n");
        return;
    }
    
    size_t read_size = fread(file_content, 1, file_size, file);
    file_content[read_size] = '\0';
    fclose(file);
    
    // Find the sentence
    char* sentence_start = get_sentence(file_content, sent_num);
    if (sentence_start == NULL) {
        // This is a new sentence - append at the end
        sentence_start = file_content + strlen(file_content);
    }
    
    // Find the end of this sentence
    char* sentence_end = sentence_start;
    while (*sentence_end != '\0' && *sentence_end != '.' && *sentence_end != '!' && *sentence_end != '?') {
        sentence_end++;
    }
    
    // Include the delimiter if present
    if (*sentence_end == '.' || *sentence_end == '!' || *sentence_end == '?') {
        sentence_end++;
    }
    
    // Extract the sentence
    size_t sentence_len = sentence_end - sentence_start;
    char sentence[MAX_MSG_SIZE];
    strncpy(sentence, sentence_start, sentence_len);
    sentence[sentence_len] = '\0';
    
    // Split sentence into words
    char words[100][256];
    int word_count = 0;
    
    // Only tokenize if sentence has content
    if (sentence_len > 0 && sentence[0] != '\0') {
        char* token = strtok(sentence, " \t\n");
        while (token != NULL && word_count < 100) {
            strncpy(words[word_count], token, sizeof(words[word_count]) - 1);
            words[word_count][sizeof(words[word_count]) - 1] = '\0';
            word_count++;
            token = strtok(NULL, " \t\n");
        }
    }
    
    // Insert word at word_index (1-based indexing per specification)
    // word_index 1 means first word, 2 means second word, etc.
    // If a word exists at that index, shift all words to the right
    if (word_index >= 1) {
        int array_index = word_index - 1;  // Convert to 0-based array index
        
        if (array_index <= word_count) {
            // Shift words to the right to make space
            for (int i = word_count; i > array_index; i--) {
                strncpy(words[i], words[i-1], sizeof(words[i]) - 1);
                words[i][sizeof(words[i]) - 1] = '\0';
            }
            // Insert new word at the index
            strncpy(words[array_index], new_content, sizeof(words[array_index]) - 1);
            words[array_index][sizeof(words[array_index]) - 1] = '\0';
            word_count++;
        } else {
            // Fill gaps with empty strings if needed (for sparse updates)
            while (word_count < array_index) {
                words[word_count][0] = '\0';
                word_count++;
            }
            strncpy(words[array_index], new_content, sizeof(words[array_index]) - 1);
            words[array_index][sizeof(words[array_index]) - 1] = '\0';
            word_count = array_index + 1;
        }
    } else {
        fprintf(stderr, "ERROR: Invalid word index %d (must be >= 1)\n", word_index);
        free(file_content);
        return;
    }
    
    // Reassemble sentence
    char new_sentence[MAX_MSG_SIZE] = "";
    for (int i = 0; i < word_count; i++) {
        if (i > 0) {
            strcat(new_sentence, " ");
        }
        strcat(new_sentence, words[i]);
    }
    
    // Build new file content
    char new_file_content[MAX_MSG_SIZE * 10] = "";
    
    // Copy content before the sentence
    size_t prefix_len = sentence_start - file_content;
    if (prefix_len > 0) {
        strncat(new_file_content, file_content, prefix_len);
        // Add single space before new sentence if prefix doesn't already end with whitespace
        if (strlen(new_file_content) > 0) {
            char last_char = new_file_content[strlen(new_file_content) - 1];
            if (last_char != ' ' && last_char != '\t' && last_char != '\n') {
                strcat(new_file_content, " ");
            }
        }
    }
    
    // Add the modified sentence
    if (strlen(new_sentence) > 0) {
        strcat(new_file_content, new_sentence);
    }
    
    // Copy content after the sentence
    if (*sentence_end != '\0') {
        // Skip any whitespace that was after the original sentence
        const char* next_content = sentence_end;
        while (*next_content == ' ' || *next_content == '\t' || *next_content == '\n') {
            next_content++;
        }
        
        // Add single space before next content only if current sentence ends with delimiter
        if (*next_content != '\0' && strlen(new_sentence) > 0) {
            char last_char = new_sentence[strlen(new_sentence) - 1];
            // Only add space if sentence ends with delimiter
            if (last_char == '.' || last_char == '!' || last_char == '?') {
                strcat(new_file_content, " ");
            }
            strcat(new_file_content, next_content);
        }
    }
    
    // Backup original file
    char backup_name[256];
    snprintf(backup_name, sizeof(backup_name), "%s.bak", filename);
    rename(filename, backup_name);
    
    // Write new content
    file = fopen(filename, "w");
    if (file == NULL) {
        fprintf(stderr, "ERROR: Cannot open file %s for writing\n", filename);
        free(file_content);
        return;
    }
    
    fwrite(new_file_content, 1, strlen(new_file_content), file);
    fclose(file);
    free(file_content);
    
    printf("Updated %s: sentence %d, word %d -> '%s'\n", filename, sent_num, word_index, new_content);
}

// Get local IP address (non-loopback)
int get_local_ip(char* buffer, size_t buflen) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }
    
    // Connect to a public DNS (doesn't actually send data)
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr("8.8.8.8");
    serv.sin_port = htons(53);
    
    if (connect(sock, (const struct sockaddr*)&serv, sizeof(serv)) < 0) {
        close(sock);
        // Fallback to localhost
        strncpy(buffer, "127.0.0.1", buflen - 1);
        buffer[buflen - 1] = '\0';
        return 0;
    }
    
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    if (getsockname(sock, (struct sockaddr*)&name, &namelen) < 0) {
        close(sock);
        strncpy(buffer, "127.0.0.1", buflen - 1);
        buffer[buflen - 1] = '\0';
        return 0;
    }
    
    inet_ntop(AF_INET, &name.sin_addr, buffer, buflen);
    close(sock);
    return 0;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    printf("=== Storage Server Starting ===\n");
    
    // Prompt for Name Server IP
    char ns_ip[64] = "127.0.0.1";
    printf("Enter Name Server IP (or press Enter for localhost): ");
    fflush(stdout);
    
    char input[64];
    if (fgets(input, sizeof(input), stdin) != NULL) {
        // Remove newline
        input[strcspn(input, "\n")] = '\0';
        
        // If user entered something, use it
        if (strlen(input) > 0) {
            strncpy(ns_ip, input, sizeof(ns_ip) - 1);
            ns_ip[sizeof(ns_ip) - 1] = '\0';
        }
    }
    
    printf("Will connect to Name Server at: %s\n", ns_ip);
    
    // Scan local directory for existing files
    printf("Scanning local storage...\n");
    DIR* dir = opendir(".");
    char file_list[MAX_MSG_SIZE] = "";
    int file_count = 0;
    
    if (dir != NULL) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            // Skip directories, hidden files, and backup files
            if (entry->d_name[0] == '.' || strstr(entry->d_name, ".bak") != NULL) {
                continue;
            }
            
            // Use stat to check if it's a regular file
            struct stat file_stat;
            if (stat(entry->d_name, &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
                // Add to file list (comma-separated)
                if (file_count > 0) {
                    strcat(file_list, ",");
                }
                strcat(file_list, entry->d_name);
                file_count++;
            }
        }
        closedir(dir);
    }
    printf("Found %d existing files\n", file_count);
    
    // Get local IP address
    char local_ip[64];
    get_local_ip(local_ip, sizeof(local_ip));
    printf("Local IP address: %s\n", local_ip);
    
    // Connect to Name Server for registration
    printf("Connecting to Name Server at %s:%d...\n", ns_ip, SS_REG_PORT);
    int ns_fd = connect_to_server(ns_ip, SS_REG_PORT);
    if (ns_fd < 0) {
        fprintf(stderr, "Failed to connect to Name Server at %s:%d\n", ns_ip, SS_REG_PORT);
        return 1;
    }
    
    // Format registration message with file list
    char msg[MAX_MSG_SIZE];
    if (strlen(file_list) > 0) {
        snprintf(msg, sizeof(msg), "REGISTER %s %d %d FILES:%s", 
                 local_ip, SS_NS_PORT, SS_CLIENT_PORT, file_list);
    } else {
        snprintf(msg, sizeof(msg), "REGISTER %s %d %d", 
                 local_ip, SS_NS_PORT, SS_CLIENT_PORT);
    }
    
    printf("Sending registration message: %s\n", msg);
    
    // Send registration to Name Server
    ssize_t bytes_sent = write(ns_fd, msg, strlen(msg));
    if (bytes_sent < 0) {
        perror("Failed to send registration message");
        close(ns_fd);
        return 1;
    }
    
    // Wait for response from Name Server
    char response[MAX_MSG_SIZE];
    memset(response, 0, sizeof(response));
    
    ssize_t bytes_read = read(ns_fd, response, sizeof(response) - 1);
    if (bytes_read <= 0) {
        perror("Failed to read response from Name Server");
        close(ns_fd);
        return 1;
    }
    response[bytes_read] = '\0';
    
    // Check if registration was successful
    if (strncmp(response, "ACK_SUCCESS", 11) == 0) {
        printf("✓ Successfully registered with Name Server\n");
        printf("Response: %s", response);
    } else {
        printf("✗ Registration failed\n");
        printf("Response: %s", response);
        close(ns_fd);
        return 1;
    }
    
    // Close registration connection to Name Server
    close(ns_fd);
    
    printf("\nStorage Server initialization complete.\n");
    printf("Starting NS command server on port %d...\n", SS_NS_PORT);
    printf("Starting Client request server on port %d...\n", SS_CLIENT_PORT);
    
    // Create TCP server to listen for commands from Name Server
    int command_server_fd = create_tcp_server(SS_NS_PORT);
    if (command_server_fd < 0) {
        fprintf(stderr, "Failed to create NS command server on port %d\n", SS_NS_PORT);
        return 1;
    }
    
    // Create a separate thread to handle client requests
    pthread_t client_thread;
    if (pthread_create(&client_thread, NULL, client_listener_thread, NULL) != 0) {
        perror("Failed to create client listener thread");
        close(command_server_fd);
        return 1;
    }
    pthread_detach(client_thread);
    
    printf("Storage Server ready. Waiting for commands...\n\n");
    
    // Main loop - accept connections from Name Server and handle commands
    while (1) {
        int ns_command_fd = accept_client(command_server_fd);
        if (ns_command_fd < 0) {
            fprintf(stderr, "Failed to accept NS command connection\n");
            continue;
        }
        
        // Spawn thread to handle command
        pthread_t thread_id;
        int* fd_ptr = malloc(sizeof(int));
        if (fd_ptr == NULL) {
            perror("Failed to allocate memory for fd");
            close(ns_command_fd);
            continue;
        }
        *fd_ptr = ns_command_fd;
        
        if (pthread_create(&thread_id, NULL, handle_ns_command, fd_ptr) != 0) {
            perror("Failed to create thread for NS command");
            free(fd_ptr);
            close(ns_command_fd);
            continue;
        }
        
        pthread_detach(thread_id);
    }
    
    // Cleanup (unreachable)
    close(command_server_fd);
    return 0;
}

/**
 * Handle commands from Name Server
 */
void* handle_ns_command(void* arg) {
    int ns_fd = *(int*)arg;
    free(arg);
    
    char buffer[MAX_MSG_SIZE];
    memset(buffer, 0, sizeof(buffer));
    
    // Read command from Name Server
    ssize_t bytes_read = read(ns_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        perror("Failed to read command from NS");
        close(ns_fd);
        return NULL;
    }
    buffer[bytes_read] = '\0';
    
    // Remove trailing newline
    char* newline = strchr(buffer, '\n');
    if (newline) *newline = '\0';
    
    printf("Received command from NS: %s\n", buffer);
    
    // Parse command
    char command[32];
    char filename[100];
    int parsed = sscanf(buffer, "%s %s", command, filename);
    
    // Handle PING command for heartbeat
    if (parsed >= 1 && strcmp(command, "PING") == 0) {
        const char* pong_msg = "PONG\n";
        write(ns_fd, pong_msg, strlen(pong_msg));
        close(ns_fd);
        return NULL;
    }
    // Handle REPLICATE command
    else if (parsed >= 1 && strcmp(command, "REPLICATE") == 0) {
        char source_ip[16];
        int source_port;
        
        if (sscanf(buffer, "REPLICATE %s %s %d", filename, source_ip, &source_port) != 3) {
            fprintf(stderr, "REPLICATE: Invalid format\n");
            close(ns_fd);
            return NULL;
        }
        
        printf("→ Replicating file '%s' from %s:%d\n", filename, source_ip, source_port);
        
        // Connect to source storage server
        int source_fd = connect_to_server(source_ip, source_port);
        if (source_fd < 0) {
            fprintf(stderr, "Failed to connect to source SS for replication\n");
            close(ns_fd);
            return NULL;
        }
        
        // Request file content from source SS
        char request[MAX_MSG_SIZE];
        snprintf(request, sizeof(request), "GET_FILE_CONTENT %s\n", filename);
        write(source_fd, request, strlen(request));
        
        // Read file content from source
        char file_content[MAX_MSG_SIZE * 4];
        memset(file_content, 0, sizeof(file_content));
        ssize_t content_bytes = read(source_fd, file_content, sizeof(file_content) - 1);
        close(source_fd);
        
        if (content_bytes > 0) {
            // Write content to local file
            FILE* file = fopen(filename, "w");
            if (file != NULL) {
                fwrite(file_content, 1, content_bytes, file);
                fclose(file);
                printf("✓ Replicated file '%s' (%zd bytes)\n", filename, content_bytes);
            } else {
                fprintf(stderr, "Failed to create replica file '%s'\n", filename);
            }
        } else {
            fprintf(stderr, "Failed to read content for replication\n");
        }
        
        close(ns_fd);
        return NULL;
    }
    else if (parsed >= 1 && strcmp(command, "CREATE_FILE") == 0) {
        if (parsed < 2) {
            fprintf(stderr, "CREATE_FILE: Missing filename\n");
            const char* error_msg = "ACK_FAILURE: Missing filename\n";
            write(ns_fd, error_msg, strlen(error_msg));
            close(ns_fd);
            return NULL;
        }
        
        // Create the file on disk
        FILE* file = fopen(filename, "w");
        if (file == NULL) {
            perror("Failed to create file");
            const char* error_msg = "ACK_FAILURE: Cannot create file\n";
            write(ns_fd, error_msg, strlen(error_msg));
            close(ns_fd);
            return NULL;
        }
        
        fclose(file);
        printf("✓ Created file: %s\n", filename);
        
        // Send success acknowledgment
        const char* ack_msg = "ACK_SUCCESS\n";
        write(ns_fd, ack_msg, strlen(ack_msg));
    }
    else if (parsed >= 1 && strcmp(command, "DELETE_FILE") == 0) {
        if (parsed < 2) {
            fprintf(stderr, "DELETE_FILE: Missing filename\n");
            const char* error_msg = "ACK_FAILURE: Missing filename\n";
            write(ns_fd, error_msg, strlen(error_msg));
            close(ns_fd);
            return NULL;
        }
        
        // Delete the file from disk
        if (remove(filename) != 0) {
            perror("Failed to delete file");
            const char* error_msg = "ACK_FAILURE: Cannot delete file\n";
            write(ns_fd, error_msg, strlen(error_msg));
            close(ns_fd);
            return NULL;
        }
        
        // Also delete backup file if it exists
        char backup_name[256];
        snprintf(backup_name, sizeof(backup_name), "%s.bak", filename);
        remove(backup_name);  // Ignore errors
        
        printf("✓ Deleted file: %s\n", filename);
        
        // Send success acknowledgment
        const char* ack_msg = "ACK_SUCCESS\n";
        write(ns_fd, ack_msg, strlen(ack_msg));
    }
    else if (parsed >= 1 && strcmp(command, "GET_STATS") == 0) {
        if (parsed < 2) {
            fprintf(stderr, "GET_STATS: Missing filename\n");
            const char* error_msg = "ERROR: Missing filename\n";
            write(ns_fd, error_msg, strlen(error_msg));
            close(ns_fd);
            return NULL;
        }
        
        // Open file and count words and characters
        FILE* file = fopen(filename, "r");
        if (file == NULL) {
            const char* error_msg = "ERROR: File not found\n";
            write(ns_fd, error_msg, strlen(error_msg));
            close(ns_fd);
            return NULL;
        }
        
        int word_count = 0;
        int char_count = 0;
        int in_word = 0;
        int c;
        
        while ((c = fgetc(file)) != EOF) {
            char_count++;
            
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
                if (in_word) {
                    word_count++;
                    in_word = 0;
                }
            } else {
                in_word = 1;
            }
        }
        
        // Count last word if file doesn't end with whitespace
        if (in_word) {
            word_count++;
        }
        
        fclose(file);
        
        // Get file metadata using stat
        struct stat file_stat;
        long file_size = 0;
        time_t created_time = 0;
        time_t modified_time = 0;
        time_t accessed_time = 0;
        
        if (stat(filename, &file_stat) == 0) {
            file_size = file_stat.st_size;
            // On Linux, st_ctime is change time (metadata), not creation time
            // Use the older of st_mtime and st_ctime as "creation" time
            created_time = (file_stat.st_mtime < file_stat.st_ctime) ? 
                          file_stat.st_mtime : file_stat.st_ctime;
            modified_time = file_stat.st_mtime;  // Modification time
            accessed_time = file_stat.st_atime;  // Access time
        }
        
        // Send stats back: "STATS <word_count> <char_count> <file_size> <created> <modified> <accessed>"
        char stats_response[512];
        snprintf(stats_response, sizeof(stats_response), 
                "STATS %d %d %ld %ld %ld %ld\n", 
                word_count, char_count, file_size, 
                (long)created_time, (long)modified_time, (long)accessed_time);
        write(ns_fd, stats_response, strlen(stats_response));
        
        printf("✓ Sent stats for %s: %d words, %d chars, %ld bytes\n", 
               filename, word_count, char_count, file_size);
    }
    else if (parsed >= 1 && strcmp(command, "GET_FILE_CONTENT") == 0) {
        if (parsed < 2) {
            fprintf(stderr, "GET_FILE_CONTENT: Missing filename\n");
            const char* error_msg = "ERROR: Missing filename\n";
            write(ns_fd, error_msg, strlen(error_msg));
            close(ns_fd);
            return NULL;
        }
        
        // Open and read entire file content
        FILE* file = fopen(filename, "rb");
        if (file == NULL) {
            const char* error_msg = "ERROR: File not found\n";
            write(ns_fd, error_msg, strlen(error_msg));
            close(ns_fd);
            return NULL;
        }
        
        // Read file content
        char content[MAX_MSG_SIZE * 4];
        size_t bytes_read = fread(content, 1, sizeof(content) - 1, file);
        fclose(file);
        
        // Send content back
        if (bytes_read > 0) {
            write(ns_fd, content, bytes_read);
        }
        
        printf("✓ Sent file content for '%s' (%zu bytes)\n", filename, bytes_read);
    }
    else if (parsed >= 1 && strcmp(command, "CREATE_CHECKPOINT") == 0) {
        char filename[256], tag[MAX_TAG_LENGTH], created_by[100];
        if (sscanf(buffer, "CREATE_CHECKPOINT %s %s %s", filename, tag, created_by) != 3) {
            const char* error_msg = "ACK_FAILURE: Invalid CREATE_CHECKPOINT format\n";
            write(ns_fd, error_msg, strlen(error_msg));
            close(ns_fd);
            return NULL;
        }
        
        // Check if file exists
        FILE* file = fopen(filename, "r");
        if (file == NULL) {
            const char* error_msg = "ACK_FAILURE: File not found\n";
            write(ns_fd, error_msg, strlen(error_msg));
            close(ns_fd);
            return NULL;
        }
        fclose(file);
        
        // Create checkpoint filename
        char checkpoint_path[300];
        snprintf(checkpoint_path, sizeof(checkpoint_path), "%s.%s.ckpt", filename, tag);
        
        // Check if checkpoint already exists
        pthread_mutex_lock(&checkpoint_mutex);
        for (int i = 0; i < checkpoint_count; i++) {
            if (strcmp(checkpoint_registry[i].filename, filename) == 0 &&
                strcmp(checkpoint_registry[i].tag, tag) == 0) {
                pthread_mutex_unlock(&checkpoint_mutex);
                const char* error_msg = "ACK_FAILURE: Checkpoint with this tag already exists\n";
                write(ns_fd, error_msg, strlen(error_msg));
                close(ns_fd);
                return NULL;
            }
        }
        pthread_mutex_unlock(&checkpoint_mutex);
        
        // Copy file to checkpoint
        char cp_command[700];
        snprintf(cp_command, sizeof(cp_command), "cp %s %s", filename, checkpoint_path);
        if (system(cp_command) != 0) {
            const char* error_msg = "ACK_FAILURE: Failed to create checkpoint\n";
            write(ns_fd, error_msg, strlen(error_msg));
            close(ns_fd);
            return NULL;
        }
        
        // Register checkpoint
        pthread_mutex_lock(&checkpoint_mutex);
        if (checkpoint_count < 1000) {
            strncpy(checkpoint_registry[checkpoint_count].filename, filename, 255);
            strncpy(checkpoint_registry[checkpoint_count].tag, tag, MAX_TAG_LENGTH - 1);
            strncpy(checkpoint_registry[checkpoint_count].checkpoint_path, checkpoint_path, 299);
            strncpy(checkpoint_registry[checkpoint_count].created_by, created_by, 99);
            checkpoint_registry[checkpoint_count].created_time = time(NULL);
            checkpoint_count++;
        }
        pthread_mutex_unlock(&checkpoint_mutex);
        
        printf("✓ Created checkpoint: %s (tag: %s)\n", checkpoint_path, tag);
        const char* ack_msg = "ACK_SUCCESS\n";
        write(ns_fd, ack_msg, strlen(ack_msg));
    }
    else if (parsed >= 1 && strcmp(command, "LIST_CHECKPOINTS") == 0) {
        char filename[256];
        if (sscanf(buffer, "LIST_CHECKPOINTS %s", filename) != 1) {
            const char* error_msg = "ERROR: Invalid LIST_CHECKPOINTS format\n";
            write(ns_fd, error_msg, strlen(error_msg));
            close(ns_fd);
            return NULL;
        }
        
        char response[MAX_MSG_SIZE];
        int offset = 0;
        int count = 0;
        
        pthread_mutex_lock(&checkpoint_mutex);
        for (int i = 0; i < checkpoint_count && offset < MAX_MSG_SIZE - 200; i++) {
            if (strcmp(checkpoint_registry[i].filename, filename) == 0) {
                char time_str[64];
                struct tm* tm_info = localtime(&checkpoint_registry[i].created_time);
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
                
                offset += snprintf(response + offset, sizeof(response) - offset,
                                 "%s|%s|%s\n", 
                                 checkpoint_registry[i].tag,
                                 time_str,
                                 checkpoint_registry[i].created_by);
                count++;
            }
        }
        pthread_mutex_unlock(&checkpoint_mutex);
        
        if (count == 0) {
            snprintf(response, sizeof(response), "NO_CHECKPOINTS\n");
        }
        
        write(ns_fd, response, strlen(response));
        printf("✓ Listed %d checkpoints for %s\n", count, filename);
    }
    else {
        // Unknown command
        fprintf(stderr, "Unknown command: %s\n", command);
        const char* error_msg = "ACK_FAILURE: Unknown command\n";
        write(ns_fd, error_msg, strlen(error_msg));
    }
    
    close(ns_fd);
    return NULL;
}

/**
 * Thread function to listen for client connections
 */
void* client_listener_thread(void* arg) {
    (void)arg;
    
    // Create TCP server for client connections
    int client_server_fd = create_tcp_server(SS_CLIENT_PORT);
    if (client_server_fd < 0) {
        fprintf(stderr, "Failed to create client server on port %d\n", SS_CLIENT_PORT);
        return NULL;
    }
    
    printf("Client listener thread started on port %d\n", SS_CLIENT_PORT);
    
    // Accept client connections
    while (1) {
        int client_fd = accept_client(client_server_fd);
        if (client_fd < 0) {
            fprintf(stderr, "Failed to accept client connection\n");
            continue;
        }
        
        // Spawn thread to handle client request
        pthread_t thread_id;
        int* client_fd_ptr = malloc(sizeof(int));
        if (client_fd_ptr == NULL) {
            perror("Failed to allocate memory for client_fd");
            close(client_fd);
            continue;
        }
        *client_fd_ptr = client_fd;
        
        if (pthread_create(&thread_id, NULL, handle_client_request, client_fd_ptr) != 0) {
            perror("Failed to create thread for client request");
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
 * Handle client requests (READ_FILE, WRITE_DATA)
 * This is now stateful to support multi-step WRITE operations
 */
void* handle_client_request(void* arg) {
    int client_fd = *(int*)arg;
    free(arg);
    
    char buffer[MAX_MSG_SIZE];
    char filename[100] = "";
    int sentence_num = -1;
    int write_mode = 0;
    
    // Stateful loop for handling multiple commands
    while (1) {
        memset(buffer, 0, sizeof(buffer));
        
        // Read request from client
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read <= 0) {
            if (write_mode) {
                // Client disconnected during WRITE, unlock
                unlock_sentence(filename, sentence_num);
            }
            break;
        }
        buffer[bytes_read] = '\0';
        
        // Remove trailing newline
        char* newline = strchr(buffer, '\n');
        if (newline) *newline = '\0';
        
        printf("Received request from client: %s\n", buffer);
        
        // Parse command
        char command[32];
        sscanf(buffer, "%s", command);
        
        if (strcmp(command, "READ_FILE") == 0) {
            char read_filename[100];
            if (sscanf(buffer, "READ_FILE %s", read_filename) != 1) {
                const char* error_msg = "ERROR: Missing filename\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            // Open and read the file
            FILE* file = fopen(read_filename, "r");
            if (file == NULL) {
                perror("Failed to open file");
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            // Read file content
            char file_content[MAX_MSG_SIZE];
            size_t content_len = fread(file_content, 1, sizeof(file_content) - 1, file);
            file_content[content_len] = '\0';
            fclose(file);
            
            printf("✓ Read file: %s (%zu bytes)\n", read_filename, content_len);
            
            // Send file content to client
            write(client_fd, file_content, content_len);
            break; // READ is single-shot, close connection
        }
        else if (strcmp(command, "WRITE_DATA") == 0) {
            char write_filename[100];
            int sent_num, word_idx;
            char new_content[256];
            
            // Parse: WRITE_DATA <filename> <sent_num> <word_index> <content>
            int parsed = sscanf(buffer, "WRITE_DATA %s %d %d %[^\n]", 
                                write_filename, &sent_num, &word_idx, new_content);
            
            if (parsed < 4) {
                const char* error_msg = "ERROR: Invalid WRITE_DATA format\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // If this is the first WRITE_DATA for this connection, acquire lock
            if (!write_mode) {
                strncpy(filename, write_filename, sizeof(filename) - 1);
                filename[sizeof(filename) - 1] = '\0';
                sentence_num = sent_num;
                
                int lock_result = lock_sentence(filename, sentence_num);
                if (lock_result != 0) {
                    if (lock_result == -1) {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg), 
                                "ERROR: Cannot write to sentence %d. Previous sentence must be completed with '.', '!', or '?' first.\n", 
                                sent_num);
                        write(client_fd, error_msg, strlen(error_msg));
                    } else if (lock_result == -3) {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg), 
                                "ERROR: File is empty. Must start writing at sentence 0, not sentence %d.\n", 
                                sent_num);
                        write(client_fd, error_msg, strlen(error_msg));
                    } else {
                        const char* error_msg = "ERROR: Lock table full\n";
                        write(client_fd, error_msg, strlen(error_msg));
                    }
                    continue;
                }
                
                write_mode = 1;
                printf("Entered WRITE mode for %s, sentence %d\n", filename, sentence_num);
            }
            
            // Verify this write is for the same file/sentence
            if (strcmp(filename, write_filename) != 0 || sentence_num != sent_num) {
                const char* error_msg = "ERROR: Cannot write to different file/sentence in same session\n";
                write(client_fd, error_msg, strlen(error_msg));
                continue;
            }
            
            // Perform the update
            update_sentence(filename, sentence_num, word_idx, new_content);
            
            // Send acknowledgment
            const char* ack_msg = "ACK_SUCCESS\n";
            write(client_fd, ack_msg, strlen(ack_msg));
        }
        else if (strcmp(command, "ETIRW") == 0) {
            // End of WRITE session
            if (write_mode) {
                unlock_sentence(filename, sentence_num);
                printf("Exited WRITE mode for %s, sentence %d\n", filename, sentence_num);
            }
            const char* ack_msg = "ACK_SUCCESS\n";
            write(client_fd, ack_msg, strlen(ack_msg));
            break;
        }
        else if (strcmp(command, "UNDO_FILE") == 0) {
            char undo_filename[100];
            if (sscanf(buffer, "UNDO_FILE %s", undo_filename) != 1) {
                const char* error_msg = "ERROR: Missing filename\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            // Check if backup file exists
            char backup_name[256];
            snprintf(backup_name, sizeof(backup_name), "%s.bak", undo_filename);
            
            FILE* backup_file = fopen(backup_name, "r");
            if (backup_file == NULL) {
                const char* error_msg = "ERROR: No backup file found\n";
                write(client_fd, error_msg, strlen(error_msg));
                printf("UNDO failed: %s not found\n", backup_name);
                break;
            }
            fclose(backup_file);
            
            // Perform the undo operation
            // 1. Rename current file to .tmp
            char temp_name[256];
            snprintf(temp_name, sizeof(temp_name), "%s.tmp", undo_filename);
            if (rename(undo_filename, temp_name) != 0) {
                const char* error_msg = "ERROR: Failed to backup current file\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            // 2. Rename .bak to original filename
            if (rename(backup_name, undo_filename) != 0) {
                // Rollback: restore from .tmp
                rename(temp_name, undo_filename);
                const char* error_msg = "ERROR: Failed to restore backup\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            // 3. Delete .tmp file
            remove(temp_name);
            
            printf("✓ UNDO successful: %s restored from backup\n", undo_filename);
            
            const char* ack_msg = "ACK_SUCCESS\n";
            write(client_fd, ack_msg, strlen(ack_msg));
            break;
        }
        else if (strcmp(command, "VIEW_CHECKPOINT") == 0) {
            char filename[256], tag[MAX_TAG_LENGTH];
            if (sscanf(buffer, "VIEW_CHECKPOINT %s %s", filename, tag) != 2) {
                const char* error_msg = "ERROR: Invalid VIEW_CHECKPOINT format\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            // Find checkpoint
            char checkpoint_path[300] = "";
            pthread_mutex_lock(&checkpoint_mutex);
            for (int i = 0; i < checkpoint_count; i++) {
                if (strcmp(checkpoint_registry[i].filename, filename) == 0 &&
                    strcmp(checkpoint_registry[i].tag, tag) == 0) {
                    strncpy(checkpoint_path, checkpoint_registry[i].checkpoint_path, 299);
                    break;
                }
            }
            pthread_mutex_unlock(&checkpoint_mutex);
            
            if (strlen(checkpoint_path) == 0) {
                const char* error_msg = "ERROR: Checkpoint not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            // Read checkpoint file
            FILE* ckpt_file = fopen(checkpoint_path, "r");
            if (ckpt_file == NULL) {
                const char* error_msg = "ERROR: Cannot open checkpoint file\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            fseek(ckpt_file, 0, SEEK_END);
            long ckpt_size = ftell(ckpt_file);
            fseek(ckpt_file, 0, SEEK_SET);
            
            char* ckpt_content = (char*)malloc(ckpt_size + 1);
            if (ckpt_content == NULL) {
                fclose(ckpt_file);
                const char* error_msg = "ERROR: Memory allocation failed\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            fread(ckpt_content, 1, ckpt_size, ckpt_file);
            ckpt_content[ckpt_size] = '\0';
            fclose(ckpt_file);
            
            // Send content to client
            write(client_fd, ckpt_content, strlen(ckpt_content));
            free(ckpt_content);
            
            printf("✓ Sent checkpoint content: %s (tag: %s)\n", filename, tag);
            break;
        }
        else if (strcmp(command, "REVERT_CHECKPOINT") == 0) {
            char filename[256], tag[MAX_TAG_LENGTH];
            if (sscanf(buffer, "REVERT_CHECKPOINT %s %s", filename, tag) != 2) {
                const char* error_msg = "ERROR: Invalid REVERT_CHECKPOINT format\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            // Find checkpoint
            char checkpoint_path[300] = "";
            pthread_mutex_lock(&checkpoint_mutex);
            for (int i = 0; i < checkpoint_count; i++) {
                if (strcmp(checkpoint_registry[i].filename, filename) == 0 &&
                    strcmp(checkpoint_registry[i].tag, tag) == 0) {
                    strncpy(checkpoint_path, checkpoint_registry[i].checkpoint_path, 299);
                    break;
                }
            }
            pthread_mutex_unlock(&checkpoint_mutex);
            
            if (strlen(checkpoint_path) == 0) {
                const char* error_msg = "ERROR: Checkpoint not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            // Backup current file
            char temp_name[300];
            snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename);
            if (rename(filename, temp_name) != 0) {
                const char* error_msg = "ERROR: Failed to backup current file\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            // Copy checkpoint to original filename
            char cp_command[700];
            snprintf(cp_command, sizeof(cp_command), "cp %s %s", checkpoint_path, filename);
            if (system(cp_command) != 0) {
                // Rollback
                rename(temp_name, filename);
                const char* error_msg = "ERROR: Failed to restore checkpoint\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            // Delete temp file
            remove(temp_name);
            
            printf("✓ REVERT successful: %s restored from checkpoint '%s'\n", filename, tag);
            
            const char* ack_msg = "ACK_SUCCESS\n";
            write(client_fd, ack_msg, strlen(ack_msg));
            break;
        }
        else if (strcmp(command, "STREAM_FILE") == 0) {
            char stream_filename[100];
            if (sscanf(buffer, "STREAM_FILE %s", stream_filename) != 1) {
                const char* error_msg = "ERROR: Missing filename\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            // Open and read the file
            FILE* file = fopen(stream_filename, "r");
            if (file == NULL) {
                perror("Failed to open file for streaming");
                const char* error_msg = "ERROR: File not found\n";
                write(client_fd, error_msg, strlen(error_msg));
                break;
            }
            
            // Read file content
            char file_content[MAX_MSG_SIZE];
            size_t content_len = fread(file_content, 1, sizeof(file_content) - 1, file);
            file_content[content_len] = '\0';
            fclose(file);
            
            printf("✓ Streaming file: %s (%zu bytes)\n", stream_filename, content_len);
            
            // Stream content character by character, with delay on word boundaries
            // This preserves exact spacing like READ but adds delays
            size_t i = 0;
            int word_count = 0;
            int in_word = 0;
            
            while (i < content_len) {
                char c = file_content[i];
                
                // Check if we're at a word boundary
                if (c == ' ' || c == '\t' || c == '\n') {
                    if (in_word) {
                        // Just finished a word, add delay before sending space
                        usleep(100000);  // 0.1 seconds
                        word_count++;
                        in_word = 0;
                    }
                } else {
                    in_word = 1;
                }
                
                // Send character
                write(client_fd, &c, 1);
                i++;
            }
            
            // Send end marker
            const char* end_marker = "STREAM_END";
            write(client_fd, end_marker, strlen(end_marker));
            
            printf("✓ Streamed %d words from %s\n", word_count, stream_filename);
            break;
        }
        else {
            // Unknown command
            fprintf(stderr, "Unknown client command: %s\n", command);
            const char* error_msg = "ERROR: Unknown command\n";
            write(client_fd, error_msg, strlen(error_msg));
            break;
        }
    }
    
    close(client_fd);
    return NULL;
}
