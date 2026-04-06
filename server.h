#ifndef SERVER_H
#define SERVER_H

#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include "thread_pool.h"
#include <errno.h>
#include <sys/sendfile.h>
#include "server_supporting.h"
#define MAX_EVENTS 1024
#define PORT 8080

typedef enum
{
    STATE_READING,
    STATE_WRITING,
    STATE_PROCESSING,
    STATE_DONE
} conn_state_t;

typedef struct
{
    int fd;
    conn_state_t state;

    char read_buffer[8192];
    char write_buffer[4096];

    int read_len;
    int write_len;
    int write_sent;

    int file_fd;       // for sendfile
    off_t file_size;   // for sendfile
    off_t file_offset; // for sendfile

    int keep_alive;
} connection_t;


typedef struct
{
    connection_t *conn;
    int epoll_fd;
} request_task_t; // make separate struct for conn and epoll for process req


//function prototypes
void parse_http_request(const char *request, char *path);
void normalize_path(char *path);
void keep_alive_check(connection_t *conn);
void handle_read(connection_t *conn, int epoll_fd);
void handle_write(connection_t *conn, int epoll_fd);
void process_request(void *args);

#endif // SERVER_H
