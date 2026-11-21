#ifndef HTTP_H
#define HTTP_H
#include <stddef.h>

struct http_request {
    char method[16];
    char path[2048];
    char version[16];
};

int parse_http_request(const char *buffer, size_t length, struct http_request *request);
#endif
