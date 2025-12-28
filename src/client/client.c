#define _XOPEN_SOURCE 600
#define _POSIX_C_SOURCE 200112L
#include "common.h"
#include "socket_utils.h"

int main(int argc, char* argv[]) {
    char username[100];
    char ns_ip[100] = "127.0.0.1";  // Default to localhost
    
    printf("=== LangOS Document Collaboration Client ===\n");
    
    // Check if IP provided as command line argument
    if (argc >= 2) {
        strncpy(ns_ip, argv[1], sizeof(ns_ip) - 1);
        ns_ip[sizeof(ns_ip) - 1] = '\0';
        printf("Using Name Server IP: %s\n", ns_ip);
    } else {
        // Ask user for Name Server IP
        printf("Enter Name Server IP (or press Enter for localhost): ");
        fflush(stdout);
        
        char ip_input[100];
        if (fgets(ip_input, sizeof(ip_input), stdin) != NULL) {
            // Remove trailing newline
            char* newline = strchr(ip_input, '\n');
            if (newline) *newline = '\0';
            
            // Use provided IP if not empty
            if (strlen(ip_input) > 0) {
                strncpy(ns_ip, ip_input, sizeof(ns_ip) - 1);
                ns_ip[sizeof(ns_ip) - 1] = '\0';
            }
        }
    }
    
    printf("Enter your username: ");
    fflush(stdout);
    
    if (fgets(username, sizeof(username), stdin) == NULL) {
        fprintf(stderr, "Failed to read username\n");
        return 1;
    }
    
    // Remove trailing newline
    char* newline = strchr(username, '\n');
    if (newline) *newline = '\0';
    
    if (strlen(username) == 0) {
        fprintf(stderr, "Username cannot be empty\n");
        return 1;
    }
    
    printf("Connecting to Name Server...\n");
    
    // Connect to Name Server
    int ns_fd = connect_to_server(ns_ip, NS_PORT);
    if (ns_fd < 0) {
        fprintf(stderr, "Failed to connect to Name Server\n");
        return 1;
    }
    
    // Send registration message
    char reg_msg[MAX_MSG_SIZE];
    snprintf(reg_msg, sizeof(reg_msg), "REGISTER_CLIENT %s", username);
    
    if (write(ns_fd, reg_msg, strlen(reg_msg)) < 0) {
        perror("Failed to send registration");
        close(ns_fd);
        return 1;
    }
    
    // Wait for acknowledgment
    char response[MAX_MSG_SIZE];
    memset(response, 0, sizeof(response));
    
    ssize_t bytes_read = read(ns_fd, response, sizeof(response) - 1);
    if (bytes_read <= 0) {
        perror("Failed to read registration response");
        close(ns_fd);
        return 1;
    }
    response[bytes_read] = '\0';
    
    if (strncmp(response, "ACK_SUCCESS", 11) != 0 && strncmp(response, "OK", 2) != 0) {
        printf("Registration failed: %s", response);
        close(ns_fd);
        return 1;
    }
    
    printf("✓ Successfully registered as '%s'\n", username);
    printf("\n=== Available Commands ===\n");
    printf("  LIST                             - List all registered users\n");
    printf("  VIEW [flags]                     - View files (flags: -a all, -l detailed)\n");
    printf("  CREATE <filename>                - Create a new file\n");
    printf("  READ <filename>                  - Read a file's content\n");
    printf("  WRITE <filename> <sent#>         - Write to a sentence in file\n");
    printf("  UNDO <filename>                  - Revert file to previous version\n");
    printf("  INFO <filename>                  - Show file owner and access list\n");
    printf("  ADDACCESS -R <file> <user>       - Grant read access to user\n");
    printf("  ADDACCESS -W <file> <user>       - Grant write access to user\n");
    printf("  REMACCESS <file> <user>          - Revoke user access to file\n");
    printf("  DELETE <filename>                - Delete a file\n");
    printf("  STREAM <filename>                - Stream file content word-by-word\n");
    printf("  EXEC <filename>                  - Execute a script file (sh/bash)\n");
    printf("  CREATEFOLDER <foldername>        - Create a new folder\n");
    printf("  VIEWFOLDER <foldername>          - List contents of folder\n");
    printf("  MOVE <filename> <foldername>     - Move file to folder\n");
    printf("  CHECKPOINT <filename> <tag>      - Create a checkpoint with tag\n");
    printf("  LISTCHECKPOINTS <filename>       - List all checkpoints\n");
    printf("  VIEWCHECKPOINT <filename> <tag>  - View checkpoint content\n");
    printf("  REVERT <filename> <tag>          - Revert to checkpoint\n");
    printf("  EXIT                             - Disconnect from server\n");
    printf("===========================================\n\n");
    
    // Command loop
    char command[MAX_MSG_SIZE];
    while (1) {
        printf("%s> ", username);
        fflush(stdout);
        
        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }
        
        // Remove trailing newline
        newline = strchr(command, '\n');
        if (newline) *newline = '\0';
        
        // Check for empty command
        if (strlen(command) == 0) {
            continue;
        }
        
        // Handle EXIT command locally
        if (strcmp(command, "EXIT") == 0 || strcmp(command, "exit") == 0) {
            printf("Disconnecting...\n");
            break;
        }
        
        // Handle READ command specially
        if (strncmp(command, "READ ", 5) == 0) {
            char filename[100];
            if (sscanf(command, "READ %s", filename) != 1) {
                printf("ERROR: Invalid READ format. Use: READ <filename>\n");
                continue;
            }
            
            // Send READ request to Name Server
            if (write(ns_fd, command, strlen(command)) < 0) {
                perror("Failed to send command");
                break;
            }
            
            // Read response from Name Server
            memset(response, 0, sizeof(response));
            bytes_read = read(ns_fd, response, sizeof(response) - 1);
            if (bytes_read <= 0) {
                printf("Connection lost\n");
                break;
            }
            response[bytes_read] = '\0';
            
            // Check if response is a REDIRECT
            if (strncmp(response, "REDIRECT ", 9) == 0) {
                char ss_ip[16];
                int ss_port;
                if (sscanf(response, "REDIRECT %s %d", ss_ip, &ss_port) != 2) {
                    printf("ERROR: Invalid REDIRECT response\n");
                    continue;
                }
                
                printf("Connecting to Storage Server at %s:%d...\n", ss_ip, ss_port);
                
                // Connect to Storage Server
                int ss_fd = connect_to_server(ss_ip, ss_port);
                if (ss_fd < 0) {
                    fprintf(stderr, "ERROR: Failed to connect to Storage Server\n");
                    continue;
                }
                
                // Send READ_FILE command to Storage Server
                char ss_command[MAX_MSG_SIZE];
                snprintf(ss_command, sizeof(ss_command), "READ_FILE %s", filename);
                if (write(ss_fd, ss_command, strlen(ss_command)) < 0) {
                    perror("Failed to send READ_FILE to Storage Server");
                    close(ss_fd);
                    continue;
                }
                
                // Read file content from Storage Server
                char file_content[MAX_MSG_SIZE];
                memset(file_content, 0, sizeof(file_content));
                ssize_t content_bytes = read(ss_fd, file_content, sizeof(file_content) - 1);
                
                if (content_bytes < 0) {
                    perror("Failed to read file content");
                } else if (content_bytes == 0) {
                    printf("(File is empty)\n");
                } else if (strncmp(file_content, "ERROR:", 6) == 0) {
                    printf("%s", file_content);
                } else {
                    printf("\n--- Content of '%s' ---\n", filename);
                    printf("%s", file_content);
                    if (file_content[strlen(file_content) - 1] != '\n') {
                        printf("\n");
                    }
                    printf("--- End of file ---\n");
                }
                
                close(ss_fd);
            } else {
                // Not a redirect, display error response
                printf("%s", response);
                if (response[strlen(response) - 1] != '\n') {
                    printf("\n");
                }
            }
        }
        // Handle WRITE command specially
        else if (strncmp(command, "WRITE ", 6) == 0) {
            char filename[100];
            int sentence_num;
            if (sscanf(command, "WRITE %s %d", filename, &sentence_num) != 2) {
                printf("ERROR: Invalid WRITE format. Use: WRITE <filename> <sentence_num>\n");
                continue;
            }
            
            // Send WRITE request to Name Server
            if (write(ns_fd, command, strlen(command)) < 0) {
                perror("Failed to send command");
                break;
            }
            
            // Read response from Name Server
            memset(response, 0, sizeof(response));
            bytes_read = read(ns_fd, response, sizeof(response) - 1);
            if (bytes_read <= 0) {
                printf("Connection lost\n");
                break;
            }
            response[bytes_read] = '\0';
            
            // Check if response is a REDIRECT
            if (strncmp(response, "REDIRECT ", 9) == 0) {
                char ss_ip[16];
                int ss_port;
                if (sscanf(response, "REDIRECT %s %d", ss_ip, &ss_port) != 2) {
                    printf("ERROR: Invalid REDIRECT response\n");
                    continue;
                }
                
                printf("Connecting to Storage Server at %s:%d for WRITE...\n", ss_ip, ss_port);
                
                // Connect to Storage Server
                int ss_fd = connect_to_server(ss_ip, ss_port);
                if (ss_fd < 0) {
                    fprintf(stderr, "ERROR: Failed to connect to Storage Server\n");
                    continue;
                }
                
                printf("✓ Connected. Enter WRITE commands:\n");
                printf("  Format: <word_index> <content>\n");
                printf("  Type 'ETIRW' to finish writing\n");
                printf("  (Sentence delimiters: '.', '!', '?')\n\n");
                
                // Enter WRITE mode loop
                char write_input[MAX_MSG_SIZE];
                while (1) {
                    printf("WRITE> ");
                    fflush(stdout);
                    
                    if (fgets(write_input, sizeof(write_input), stdin) == NULL) {
                        break;
                    }
                    
                    // Remove trailing newline
                    char* nl = strchr(write_input, '\n');
                    if (nl) *nl = '\0';
                    
                    // Check for empty input
                    if (strlen(write_input) == 0) {
                        continue;
                    }
                    
                    // Check for ETIRW
                    if (strcmp(write_input, "ETIRW") == 0) {
                        // Send ETIRW to Storage Server
                        write(ss_fd, "ETIRW\n", 6);
                        
                        // Wait for acknowledgment
                        char ack[256];
                        read(ss_fd, ack, sizeof(ack) - 1);
                        
                        printf("✓ Write session ended\n");
                        break;
                    }
                    
                    // Parse word_index and content
                    int word_idx;
                    char content[256];
                    if (sscanf(write_input, "%d %[^\n]", &word_idx, content) != 2) {
                        printf("ERROR: Invalid format. Use: <word_index> <content>\n");
                        continue;
                    }
                    
                    // Send WRITE_DATA command to Storage Server
                    char ss_command[MAX_MSG_SIZE];
                    snprintf(ss_command, sizeof(ss_command), 
                             "WRITE_DATA %s %d %d %s\n", 
                             filename, sentence_num, word_idx, content);
                    
                    if (write(ss_fd, ss_command, strlen(ss_command)) < 0) {
                        perror("Failed to send WRITE_DATA to Storage Server");
                        break;
                    }
                    
                    // Wait for acknowledgment
                    char ss_response[256];
                    memset(ss_response, 0, sizeof(ss_response));
                    ssize_t ss_bytes = read(ss_fd, ss_response, sizeof(ss_response) - 1);
                    
                    if (ss_bytes <= 0) {
                        printf("ERROR: Connection to Storage Server lost\n");
                        break;
                    }
                    
                    if (strncmp(ss_response, "ACK_SUCCESS", 11) == 0) {
                        printf("✓ Word %d updated\n", word_idx);
                    } else {
                        printf("ERROR: %s", ss_response);
                    }
                }
                
                close(ss_fd);
            } else {
                // Not a redirect, display error response
                printf("%s", response);
                if (response[strlen(response) - 1] != '\n') {
                    printf("\n");
                }
            }
        }
        // Handle UNDO command specially
        else if (strncmp(command, "UNDO ", 5) == 0) {
            char filename[100];
            if (sscanf(command, "UNDO %s", filename) != 1) {
                printf("ERROR: Invalid UNDO format. Use: UNDO <filename>\n");
                continue;
            }
            
            // Send UNDO request to Name Server
            if (write(ns_fd, command, strlen(command)) < 0) {
                perror("Failed to send command");
                break;
            }
            
            // Read response from Name Server
            memset(response, 0, sizeof(response));
            bytes_read = read(ns_fd, response, sizeof(response) - 1);
            if (bytes_read <= 0) {
                printf("Connection lost\n");
                break;
            }
            response[bytes_read] = '\0';
            
            // Check if response is a REDIRECT
            if (strncmp(response, "REDIRECT ", 9) == 0) {
                char ss_ip[16];
                int ss_port;
                if (sscanf(response, "REDIRECT %s %d", ss_ip, &ss_port) != 2) {
                    printf("ERROR: Invalid REDIRECT response\n");
                    continue;
                }
                
                printf("Connecting to Storage Server at %s:%d for UNDO...\n", ss_ip, ss_port);
                
                // Connect to Storage Server
                int ss_fd = connect_to_server(ss_ip, ss_port);
                if (ss_fd < 0) {
                    fprintf(stderr, "ERROR: Failed to connect to Storage Server\n");
                    continue;
                }
                
                // Send UNDO_FILE command to Storage Server
                char ss_command[MAX_MSG_SIZE];
                snprintf(ss_command, sizeof(ss_command), "UNDO_FILE %s\n", filename);
                if (write(ss_fd, ss_command, strlen(ss_command)) < 0) {
                    perror("Failed to send UNDO_FILE to Storage Server");
                    close(ss_fd);
                    continue;
                }
                
                // Wait for acknowledgment
                char ss_response[256];
                memset(ss_response, 0, sizeof(ss_response));
                ssize_t ss_bytes = read(ss_fd, ss_response, sizeof(ss_response) - 1);
                
                if (ss_bytes <= 0) {
                    printf("ERROR: Connection to Storage Server lost\n");
                } else if (strncmp(ss_response, "ACK_SUCCESS", 11) == 0) {
                    printf("✓ File reverted to previous version\n");
                } else {
                    printf("%s", ss_response);
                }
                
                close(ss_fd);
            } else {
                // Not a redirect, display error response
                printf("%s", response);
                if (response[strlen(response) - 1] != '\n') {
                    printf("\n");
                }
            }
        }
        // Handle VIEWCHECKPOINT command specially
        else if (strncmp(command, "VIEWCHECKPOINT ", 15) == 0) {
            char filename[100], tag[50];
            if (sscanf(command, "VIEWCHECKPOINT %s %s", filename, tag) != 2) {
                printf("ERROR: Invalid VIEWCHECKPOINT format. Use: VIEWCHECKPOINT <filename> <tag>\n");
                continue;
            }
            
            // Send VIEWCHECKPOINT request to Name Server
            if (write(ns_fd, command, strlen(command)) < 0) {
                perror("Failed to send command");
                break;
            }
            
            // Read response from Name Server
            memset(response, 0, sizeof(response));
            bytes_read = read(ns_fd, response, sizeof(response) - 1);
            if (bytes_read <= 0) {
                printf("Connection lost\n");
                break;
            }
            response[bytes_read] = '\0';
            
            // Check if response is a REDIRECT_VIEWCHECKPOINT
            if (strncmp(response, "REDIRECT_VIEWCHECKPOINT ", 24) == 0) {
                char ss_ip[16];
                int ss_port;
                if (sscanf(response, "REDIRECT_VIEWCHECKPOINT %s %d", ss_ip, &ss_port) != 2) {
                    printf("ERROR: Invalid REDIRECT_VIEWCHECKPOINT response\n");
                    continue;
                }
                
                printf("Connecting to Storage Server at %s:%d for VIEWCHECKPOINT...\n", ss_ip, ss_port);
                
                // Connect to Storage Server
                int ss_fd = connect_to_server(ss_ip, ss_port);
                if (ss_fd < 0) {
                    fprintf(stderr, "ERROR: Failed to connect to Storage Server\n");
                    continue;
                }
                
                // Send VIEW_CHECKPOINT command to Storage Server
                char ss_command[MAX_MSG_SIZE];
                snprintf(ss_command, sizeof(ss_command), "VIEW_CHECKPOINT %s %s\n", filename, tag);
                if (write(ss_fd, ss_command, strlen(ss_command)) < 0) {
                    perror("Failed to send VIEW_CHECKPOINT to Storage Server");
                    close(ss_fd);
                    continue;
                }
                
                // Read and display checkpoint content
                char ss_response[MAX_MSG_SIZE];
                memset(ss_response, 0, sizeof(ss_response));
                ssize_t ss_bytes = read(ss_fd, ss_response, sizeof(ss_response) - 1);
                
                if (ss_bytes <= 0) {
                    printf("ERROR: Connection to Storage Server lost\n");
                } else {
                    printf("%s", ss_response);
                    if (ss_response[strlen(ss_response) - 1] != '\n') {
                        printf("\n");
                    }
                }
                
                close(ss_fd);
            } else {
                // Not a redirect, display error response
                printf("%s", response);
                if (response[strlen(response) - 1] != '\n') {
                    printf("\n");
                }
            }
        }
        // Handle REVERT command specially
        else if (strncmp(command, "REVERT ", 7) == 0) {
            char filename[100], tag[50];
            if (sscanf(command, "REVERT %s %s", filename, tag) != 2) {
                printf("ERROR: Invalid REVERT format. Use: REVERT <filename> <tag>\n");
                continue;
            }
            
            // Send REVERT request to Name Server
            if (write(ns_fd, command, strlen(command)) < 0) {
                perror("Failed to send command");
                break;
            }
            
            // Read response from Name Server
            memset(response, 0, sizeof(response));
            bytes_read = read(ns_fd, response, sizeof(response) - 1);
            if (bytes_read <= 0) {
                printf("Connection lost\n");
                break;
            }
            response[bytes_read] = '\0';
            
            // Check if response is a REDIRECT_REVERT
            if (strncmp(response, "REDIRECT_REVERT ", 16) == 0) {
                char ss_ip[16];
                int ss_port;
                if (sscanf(response, "REDIRECT_REVERT %s %d", ss_ip, &ss_port) != 2) {
                    printf("ERROR: Invalid REDIRECT_REVERT response\n");
                    continue;
                }
                
                printf("Connecting to Storage Server at %s:%d for REVERT...\n", ss_ip, ss_port);
                
                // Connect to Storage Server
                int ss_fd = connect_to_server(ss_ip, ss_port);
                if (ss_fd < 0) {
                    fprintf(stderr, "ERROR: Failed to connect to Storage Server\n");
                    continue;
                }
                
                // Send REVERT_CHECKPOINT command to Storage Server
                char ss_command[MAX_MSG_SIZE];
                snprintf(ss_command, sizeof(ss_command), "REVERT_CHECKPOINT %s %s\n", filename, tag);
                if (write(ss_fd, ss_command, strlen(ss_command)) < 0) {
                    perror("Failed to send REVERT_CHECKPOINT to Storage Server");
                    close(ss_fd);
                    continue;
                }
                
                // Wait for acknowledgment
                char ss_response[256];
                memset(ss_response, 0, sizeof(ss_response));
                ssize_t ss_bytes = read(ss_fd, ss_response, sizeof(ss_response) - 1);
                
                if (ss_bytes <= 0) {
                    printf("ERROR: Connection to Storage Server lost\n");
                } else if (strncmp(ss_response, "ACK_SUCCESS", 11) == 0) {
                    printf("✓ File reverted to checkpoint '%s'\n", tag);
                } else {
                    printf("%s", ss_response);
                }
                
                close(ss_fd);
            } else {
                // Not a redirect, display error response
                printf("%s", response);
                if (response[strlen(response) - 1] != '\n') {
                    printf("\n");
                }
            }
        }
        // Handle STREAM command specially
        else if (strncmp(command, "STREAM ", 7) == 0) {
            char filename[100];
            if (sscanf(command, "STREAM %s", filename) != 1) {
                printf("ERROR: Invalid STREAM format. Use: STREAM <filename>\n");
                continue;
            }
            
            // Trim any whitespace from filename
            char* trimmed = filename;
            while (*trimmed && (*trimmed == ' ' || *trimmed == '\t')) trimmed++;
            strncpy(filename, trimmed, sizeof(filename) - 1);
            
            // Send STREAM request to Name Server
            if (write(ns_fd, command, strlen(command)) < 0) {
                perror("Failed to send command");
                break;
            }
            
            // Read response from Name Server
            memset(response, 0, sizeof(response));
            bytes_read = read(ns_fd, response, sizeof(response) - 1);
            if (bytes_read <= 0) {
                printf("Connection lost\n");
                break;
            }
            response[bytes_read] = '\0';
            
            // Check if response is a REDIRECT
            if (strncmp(response, "REDIRECT ", 9) == 0) {
                char ss_ip[16];
                int ss_port;
                if (sscanf(response, "REDIRECT %s %d", ss_ip, &ss_port) != 2) {
                    printf("ERROR: Invalid REDIRECT response\n");
                    continue;
                }
                
                printf("Streaming '%s' from Storage Server at %s:%d...\n", filename, ss_ip, ss_port);
                
                // Connect to Storage Server
                int ss_fd = connect_to_server(ss_ip, ss_port);
                if (ss_fd < 0) {
                    fprintf(stderr, "ERROR: Failed to connect to Storage Server\n");
                    continue;
                }
                
                // Send STREAM_FILE command to Storage Server
                char ss_command[MAX_MSG_SIZE];
                snprintf(ss_command, sizeof(ss_command), "STREAM_FILE %s\n", filename);
                if (write(ss_fd, ss_command, strlen(ss_command)) < 0) {
                    perror("Failed to send STREAM_FILE to Storage Server");
                    close(ss_fd);
                    continue;
                }
                
                printf("\n--- Streaming '%s' ---\n", filename);
                
                // Read and display words with 0.1 second delay
                char word[256];
                int word_count = 0;
                
                while (1) {
                    memset(word, 0, sizeof(word));
                    ssize_t word_bytes = read(ss_fd, word, sizeof(word) - 1);
                    
                    if (word_bytes <= 0) {
                        // Check if storage server disconnected unexpectedly
                        if (word_bytes < 0) {
                            printf("\nERROR: Storage Server disconnected unexpectedly\n");
                        }
                        break;
                    }
                    
                    word[word_bytes] = '\0';
                    
                    // Check for end marker
                    if (strcmp(word, "STREAM_END") == 0) {
                        break;
                    }
                    
                    // Display word
                    printf("%s ", word);
                    fflush(stdout);
                    word_count++;
                    
                    // 0.1 second delay
                    usleep(100000);
                }
                
                printf("\n--- Streamed %d words ---\n", word_count);
                close(ss_fd);
            } else {
                // Not a redirect, display error response
                printf("%s", response);
                if (response[strlen(response) - 1] != '\n') {
                    printf("\n");
                }
            }
        }
        // Handle other commands normally
        else {
            // Send command to Name Server
            if (write(ns_fd, command, strlen(command)) < 0) {
                perror("Failed to send command");
                break;
            }
            
            // Read response
            memset(response, 0, sizeof(response));
            bytes_read = read(ns_fd, response, sizeof(response) - 1);
            if (bytes_read <= 0) {
                printf("Connection lost\n");
                break;
            }
            response[bytes_read] = '\0';
            
            // Display response
            printf("%s", response);
            if (response[strlen(response) - 1] != '\n') {
                printf("\n");
            }
        }
    }
    
    // Cleanup
    close(ns_fd);
    printf("Disconnected from Name Server\n");
    
    return 0;
}
