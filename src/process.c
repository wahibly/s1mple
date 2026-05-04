#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include "process.h"

int process_exec_capture(char *const argv[], char **out_buf) {

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    //child 
    if (pid == 0){

        //close read
        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) < 0) { //stdout -> pipe 1
            perror("dup2");
            exit(1);
        }
        close(pipefd[1]);

        dup2(STDOUT_FILENO, STDERR_FILENO); //also error

        execvp(argv[0], argv);

        fprintf(stderr, "execvp failed: %s\n", argv[0]);
        exit(1);
    }

    //parent

    close(pipefd[1]);

    *out_buf = malloc(MAX_OUTPUT);
    if(!*out_buf) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return -1;
    }

    int total = 0;
    ssize_t n;

    while (total < MAX_OUTPUT - 1) {
        n = read(pipefd[0], *out_buf + total, MAX_OUTPUT -1 - total);
        if ( n <= 0) break;
        total += n;
    }

    (*out_buf)[total]= '\0';
    close(pipefd[0]);

    int status;
    waitpid(pid , &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[process] child exited with code %d\n",
                WEXITSTATUS(status));
        // We still return the output it likely contains the error message
    }

    return total;
}