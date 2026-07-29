#ifndef EVENT_H
#define EVENT_H

// connection states for the epoll state machine
typedef enum {
    CONN_READING_HEADERS = 0,
    CONN_READING_BODY,
    CONN_WRITING,
    CONN_CLOSING
} conn_state_t;


typedef struct {
    int           fd;
    char          client_ip[46];

    // Read buffer
    char          rbuf[8192];
    int           rbuf_len;

    // Write buffer
    char         *wbuf;
    int           wbuf_len;
    int           wbuf_sent;   // how many bytes written so far

    conn_state_t  state;
    int           keep_alive;
    int           requests;    // requests handled on this connection
} conn_t;

// run the epoll event loop,  Block forever or until signal
void event_loop_run(int server_fd);

#endif