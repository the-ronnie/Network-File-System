#ifndef COMMON_H
#define COMMON_H

// Basic libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

// Port definitions
#define NS_PORT 9000           // Name Server port for clients
#define SS_REG_PORT 9001       // Storage Server registration port (NS to SS communication)
#define SS_NS_PORT 9002        // Storage Server port for NS commands
#define SS_CLIENT_PORT 9003    // Storage Server port for direct client access

// Buffer size
#define MAX_MSG_SIZE 4096

// Standardized System-Wide Error Codes (per specification)
typedef enum {
    OK = 0,
    ERR_UNKNOWN = 1,
    
    // Name Server Errors (1001-1999)
    ERR_ACL_DENIED = 1001,
    ERR_FILE_NOT_FOUND = 1002,
    ERR_FILE_EXISTS = 1003,
    ERR_NOT_OWNER = 1004,
    ERR_LOCK_DENIED = 1005,
    ERR_INVALID_PATH = 1006,
    
    // Storage Server Errors (2001-2999)
    ERR_SS_UNAVAILABLE = 2001,
    ERR_SS_DISK_FULL = 2002,
    ERR_SS_FILE_CORRUPT = 2003,
    ERR_NO_UNDO_STATE = 2004
} ErrorCode;

// Access permission types
typedef enum {
    ACCESS_NONE = 0,
    ACCESS_READ = 1,
    ACCESS_WRITE = 2  // Write implies read
} AccessType;

#endif // COMMON_H
