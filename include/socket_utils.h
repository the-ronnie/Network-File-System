#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

/**
 * Create a TCP server socket and start listening
 * @param port The port number to bind to
 * @return The server socket file descriptor, or -1 on error
 */
int create_tcp_server(int port);

/**
 * Accept a new client connection
 * @param server_fd The server socket file descriptor
 * @return The client socket file descriptor, or -1 on error
 */
int accept_client(int server_fd);

/**
 * Connect to a TCP server
 * @param ip The IP address of the server
 * @param port The port number of the server
 * @return The socket file descriptor, or -1 on error
 */
int connect_to_server(const char* ip, int port);

#endif // SOCKET_UTILS_H
