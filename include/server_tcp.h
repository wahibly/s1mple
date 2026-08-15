#ifndef SERVER_TCP_H
#define SERVER_TCP_H
#include <signal.h>

#define SERVER_PORT     8080
#define SERVER_BACKLOG  10  //max connection the kerenl will hold
#define BUFFER_SIZE     16384
#define STATIC_DIR      "static"

//security limit
#define MAX_REQUEST_SIZE 4096  //4KB max
#define MAX_CONN_PER_IP 10      // max simultaneous connections per IP
#define MAX_CONN_TRACKED 256 // max unique IPs we track at once

extern volatile sig_atomic_t g_running;
#endif