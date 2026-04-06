#include "server.h"
#include "server_supporting.h"
#include "server_globals.h"
#include "thread_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
 #include <sys/stat.h>
#include <errno.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>

#define MAX_EVENTS 1024

#define PORT 8080

// Define the globals (declared extern in server_globals.h)
done_queue_t g_done_queue;
int          g_eventfd;


// parsing http req to get path and dynamically sending the file content using sendfile for zero-copy optimization

void parse_http_request(const char *request, char *path)
{

    sscanf(request, "GET %s HTTP/1.1", path);

    printf("Parsed path: %s\n", path);
}

void normalize_path(char *path)
{
    if (strcmp(path, "/") == 0)
    {
        strcpy(path, "/index.html");
    }
}

// usable function to check keep-alive here and use it in both read and write handlers to decide whether to close connection or not after request is processed and response is sent
void keep_alive_check(connection_t *conn)
{
    if (strstr(conn->read_buffer, "Connection: keep-alive"))
    {
        conn->keep_alive = 1;
    }
    else
    {
        conn->keep_alive = 0;
    }
}
void add_keep_alive_code(connection_t *conn, int epoll_fd, int client_fd)
{ // resuable function to add keep-alive code in response header if client requested keep-alive
    if (conn->keep_alive)
    {
        conn->state = STATE_READING;
        conn->read_len = 0;
        conn->write_len = 0;
        conn->write_sent = 0;

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET; // edge-triggered for reading
        ev.data.ptr = conn;

        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
    }
    else
    {
        conn->state = STATE_DONE;
        close(client_fd);
    }
}

// handle Read
void handle_read(connection_t *conn, int epoll_fd)
{
    int client_fd = conn->fd;

    while (1)
    {
        int n = recv(client_fd,
                     conn->read_buffer + conn->read_len,
                     sizeof(conn->read_buffer) - conn->read_len - 1,
                     0);

        if (n > 0)
        {
            conn->read_len += n;
            conn->read_buffer[conn->read_len] = '\0'; // always null-terminate

            if (strstr(conn->read_buffer, "\r\n\r\n"))
            {
                printf("Received full request:\n%s\n", conn->read_buffer);

                // detect keep-alive
                conn->keep_alive = strstr(conn->read_buffer, "Connection: keep-alive") ? 1 : 0;

                conn->state = STATE_PROCESSING;

                request_task_t *task = malloc(sizeof(request_task_t));
                task->conn = conn;
                task->epoll_fd = epoll_fd;
                thread_pool_add_task(process_request, task);
                free(task);

                break; // hand off to worker, stop reading
            }

            // buffer full but no complete request yet — attacker/garbage, drop it
            if (conn->read_len >= (int)sizeof(conn->read_buffer) - 1)
            {
                conn->state = STATE_DONE;
                close(client_fd);
                break;
            }
        }
        else if (n == 0)
        {
            // peer closed connection cleanly
            // previously this was INSIDE if(n>0) — unreachable
            conn->state = STATE_DONE;
            close(client_fd);
            break;
        }
        else // n < 0
        {
            // EAGAIN = no more data right now (edge-triggered), not an error
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;

            // real error
            conn->state = STATE_DONE;
            close(client_fd);
            break;
        }
    }
}

// handle write
void handle_write(connection_t *conn, int epoll_fd)
{
    int client_fd = conn->fd;

    // --- Phase 1: send HTTP headers ---
    while (conn->write_sent < conn->write_len)
    {
        int n = send(client_fd,
                     conn->write_buffer + conn->write_sent,
                     conn->write_len - conn->write_sent,
                     0);

        if (n > 0)
        {
            conn->write_sent += n;
        }
        else if (n == 0)
        {
            // send() returning 0 means peer closed — don't attempt keep-alive
            // previously this incorrectly called add_keep_alive_code()
            conn->state = STATE_DONE;
            close(client_fd);
            return; // use return, not break, to skip file phase too
        }
        else // n < 0
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return; // epoll will wake us again when socket is writable
            conn->state = STATE_DONE;
            close(client_fd);
            return;
        }
    }

    // --- Phase 2: send file body via sendfile (zero-copy) ---
    if (conn->file_fd > 0)
    {
        while (conn->file_offset < conn->file_size)
        {
            ssize_t sent = sendfile(client_fd,
                                    conn->file_fd,
                                    &conn->file_offset,
                                    conn->file_size - conn->file_offset);

            if (sent > 0)
            {
                // progress made — sendfile updates file_offset automatically
                // previously the loop had no `sent > 0` branch at all,
                // so it would hit the else and break even on success
                continue;
            }
            else if (sent == 0)
            {
                break; // EOF
            }
            else // sent < 0
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return; // epoll will fire EPOLLOUT again, resume from file_offset
                // real sendfile error
                conn->state = STATE_DONE;
                close(conn->file_fd);
                conn->file_fd = -1;
                close(client_fd);
                return;
            }
        }

        close(conn->file_fd);
        conn->file_fd = -1;
    }

    // --- Phase 3: both header and file fully sent ---
    add_keep_alive_code(conn, epoll_fd, client_fd);
}

void process_request(void *args)
{
    request_task_t *task = (request_task_t *)args;
    connection_t *conn = task->conn;
    int epoll_fd = task->epoll_fd;
    int client_fd = conn->fd;
    free(task); // free the wrapper, conn lives on

    printf("thread %lu processing request:\n%s\n", pthread_self(), conn->read_buffer);

    char path[256];
    parse_http_request(conn->read_buffer, path);

    // path traversal protection
    if (strstr(path, ".."))
    {
        const char *resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        conn->write_len = strlen(resp);
        strncpy(conn->write_buffer, resp, sizeof(conn->write_buffer) - 1);
        conn->write_sent = 0;
        conn->state = STATE_WRITING;

        if (done_queue_push(&g_done_queue, conn) == 0)
            signal_main_loop();
        return;
    }

    normalize_path(path);

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "./www%s", path); // snprintf over sprintf

    // open first, CHECK result, THEN fstat
    // previously: fstat was called before the file_fd < 0 check — instant crash on missing file
    int file_fd = open(full_path, O_RDONLY);

    if (file_fd < 0)
    {
        // 404 — file not found
        conn->write_len = snprintf(conn->write_buffer, sizeof(conn->write_buffer),
                                   "HTTP/1.1 404 Not Found\r\n"
                                   "Server: MyCServer\r\n"
                                   "Content-Type: text/html\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: %s\r\n"
                                   "\r\n",
                                   conn->keep_alive ? "keep-alive" : "close");
        conn->file_fd = -1;
        conn->write_sent = 0;
    }
    else
    {

        struct stat file_stat;
        if (fstat(file_fd, &file_stat) < 0)
        {
            close(file_fd);
            conn->state = STATE_DONE;
            goto done;
            close(client_fd);
            return;
        }

        const char *mime = get_mime_type(full_path);

        conn->file_fd = file_fd;
        conn->file_size = file_stat.st_size;
        conn->file_offset = 0;

        conn->write_len = snprintf(conn->write_buffer, sizeof(conn->write_buffer),
                                   "HTTP/1.1 200 OK\r\n"
                                   "Server: MyCServer\r\n"
                                   "Content-Type: %s\r\n"
                                   "Content-Length: %ld\r\n"
                                   "Connection: %s\r\n"
                                   "\r\n",
                                   mime,
                                   file_stat.st_size,
                                   conn->keep_alive ? "keep-alive" : "close");
        conn->write_sent = 0;
    }

    

done:
{

    conn->state = STATE_WRITING;

    if (done_queue_push(&g_done_queue, conn) == 0)
        signal_main_loop();
    else
    {
        // queue full — close gracefully rather than leak
        conn->state = STATE_DONE;
        close(conn->fd);
        if (conn->file_fd > 0) close(conn->file_fd);
        free(conn);
    }

}
}

void conn_destroy(connection_t *conn, int epoll_fd)
{
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    close(conn->fd);

    if (conn->timer_fd > 0)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->timer_fd, NULL);
        close(conn->timer_fd);
    }

    if (conn->file_fd > 0)
        close(conn->file_fd);

    free(conn);
}


int main(void)
{
    done_queue_init(&g_done_queue);

    // EFD_NONBLOCK: read returns EAGAIN when counter is 0
    // EFD_CLOEXEC:  child processes don't inherit it
    g_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_eventfd < 0) { perror("eventfd"); exit(EXIT_FAILURE); }

    // ... socket setup, bind, listen, fcntl (same as before) ...

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

     // SO_REUSEADDR — without this, restarting the server within ~60s
    // fails with "Address already in use" due to TIME_WAIT sockets
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family      = AF_INET;
    address.sin_port        = htons(PORT);
    address.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

     if (listen(server_fd, SOMAXCONN) < 0) 
     {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    // set server socket non-blocking BEFORE epoll_ctl add
    // so accept() in edge-triggered mode doesn't block
    if (fcntl(server_fd, F_SETFL, O_NONBLOCK) < 0)
    {
        perror("fcntl server");
        exit(EXIT_FAILURE);
    }


    int epoll_fd = epoll_create1(0);

    // Watch server socket for new connections
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    // Watch the eventfd — this is what workers will ping
    ev.events  = EPOLLIN;           // level-triggered is fine here
    ev.data.fd = g_eventfd;         // use data.fd (not .ptr) to distinguish it
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_eventfd, &ev);

    thread_pool_init();
    printf("Server ready on port %d\n", PORT);

    struct epoll_event events[MAX_EVENTS];

    while (1)
    {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }

        for (int i = 0; i < n; i++)
        {
            int fired_fd = events[i].data.fd;

            // ──────────────────────────────────────────────
            // Case 1: worker(s) finished — drain done_queue
            // ──────────────────────────────────────────────
            if (fired_fd == g_eventfd)
            {
                // Drain the counter — mandatory or epoll re-fires immediately.
                // The value tells us how many writes happened since last read,
                // but we don't use it — we drain the queue fully regardless.
                uint64_t counter;
                read(g_eventfd, &counter, sizeof(counter)); // clears to 0

                // Drain ALL ready connections in one shot.
                // Multiple workers may have each written 1 — handle them all
                // now rather than waiting for another epoll_wait cycle.
                connection_t *conn;
                while ((conn = done_queue_pop(&g_done_queue)) != NULL)
                {
                    if (conn->state == STATE_DONE)
                    {
                        // worker marked it done (e.g. fstat failed)
                        conn_destroy(conn, epoll_fd);
                        continue;
                    }

                    // Switch this connection to EPOLLOUT — safe to do here
                    // because we're on the main thread, which owns epoll.
                    // No race: worker has already finished writing to conn.
                    struct epoll_event wev;
                    wev.events   = EPOLLOUT | EPOLLET;
                    wev.data.ptr = conn;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &wev);
                }
                continue;
            }

            // ──────────────────────────────────────────────
            // Case 2: new incoming connection
            // ──────────────────────────────────────────────
            if (fired_fd == server_fd)
            {
                while (1)
                {
                    int client_fd = accept(server_fd, NULL, NULL);
                    if (client_fd < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept"); break;
                    }

                    fcntl(client_fd, F_SETFL, O_NONBLOCK);

                    connection_t *conn = calloc(1, sizeof(connection_t));
                    if (!conn) { close(client_fd); continue; }

                    conn->fd      = client_fd;
                    conn->state   = STATE_READING;
                    conn->file_fd = -1;

                    struct epoll_event cev;
                    cev.events   = EPOLLIN | EPOLLET;
                    cev.data.ptr = conn;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev);
                }
                continue;
            }

            // ──────────────────────────────────────────────
            // Case 3: activity on a client connection
            // ──────────────────────────────────────────────
            connection_t *conn = (connection_t *)events[i].data.ptr;

            if (events[i].events & (EPOLLERR | EPOLLHUP))
            {
                conn_destroy(conn, epoll_fd);
                continue;
            }

            if (events[i].events & EPOLLIN && conn->state == STATE_READING)
            {
                handle_read(conn, epoll_fd);
                // If handle_read dispatched to thread pool,
                // the worker will signal us via eventfd when done.
                // We do NOT check conn->state == STATE_WRITING here — that's
                // exactly the race we're eliminating.
            }

            if (events[i].events & EPOLLOUT && conn->state == STATE_WRITING)
            {
                handle_write(conn, epoll_fd);
            }

            if (conn->state == STATE_DONE)
            {
                conn_destroy(conn, epoll_fd);
            }
        }
    }

    close(g_eventfd);
    close(epoll_fd);
    close(server_fd);
    done_queue_destroy(&g_done_queue);
    thread_pool_destroy();
    return 0;
}