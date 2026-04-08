#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>      
#include <sys/socket.h> 
#include <netinet/in.h>  
#include <arpa/inet.h> //IP->String (inet_ntoa())
#include "../include/server_tcp.h"


int main(void) {
    int server_fd, conn_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    server_fd = socket(AF_INET, SOCK_STREAM, 0); //TCP
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    //without this if you restart the server quickly bind() fails with
    // "Address already in use" because the old socket is in TIME_WAIT state
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    if(listen(server_fd, SERVER_BACKLOG)<0){
        perror("listern");
    }

    printf("Server listening on port %d...\n", SERVER_PORT);
    printf("Connect with: nc localhost %d\n\n", SERVER_PORT); //nc: Netcat, a command-line tool used to create TCP/UDP connections


    while(1){
        conn_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (conn_fd < 0) {
            perror("accept");
            continue; //not kill server en cas bad accept
        }
        printf("New connection from %s:%d (fd=%d)\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port),
               conn_fd);

        bytes_read = read(conn_fd, buffer, BUFFER_SIZE - 1);
        if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("Received %zd bytes:\n---\n%s\n---\n", bytes_read, buffer);

        write(conn_fd, buffer, bytes_read); //for now just echo what we recieved
    }

        close(conn_fd);
        printf("Connection closed.\n\n");

    }
    close(server_fd);
    return 0;
}