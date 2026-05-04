#ifndef PROCESS_H
#define PROCESS_H

#define MAX_OUTPUT 65536  // 64KB max script output

// execute a program and capture its output
int process_exec_capture(char *const argv[], char **out_buf);

#endif