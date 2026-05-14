#ifndef SECURITY_H
#define SECURITY_H

#include <netinet/in.h>

int  security_conn_allow(const char *ip);
void security_conn_release(const char *ip);

int security_path_is_safe(const char *filepath, const char *allowed_dir);
int security_request_size_ok(int size);

#endif