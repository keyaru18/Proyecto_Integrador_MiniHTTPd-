#include "http.h"

#include <stddef.h>
#include <string.h>

int parse_http_request(const char *buffer, size_t length, struct http_request *request)
{
    if (buffer == NULL || request == NULL || length == 0) {
        return -1;
    }

    const char *cr = memchr(buffer, '\r', length);
    if (cr == NULL || (size_t)(cr - buffer) + 1 >= length || cr[1] != '\n') {
        return -1;
    }

    size_t line_len = (size_t)(cr - buffer);

    const char *space1 = memchr(buffer, ' ', line_len);
    if (space1 == NULL) {
        return -1;
    }

    size_t method_len = (size_t)(space1 - buffer);
    if (method_len == 0 || method_len >= sizeof(request->method)) {
        return -1;
    }

    memcpy(request->method, buffer, method_len);
    request->method[method_len] = '\0';

    const char *path_start = space1 + 1;
    size_t remaining_len = (size_t)(cr - path_start);

    const char *space2 = memchr(path_start, ' ', remaining_len);
    if (space2 == NULL) {
        return -1;
    }

    size_t path_len = (size_t)(space2 - path_start);
    if (path_len == 0 || path_len >= sizeof(request->path)) {
        return -1;
    }

    memcpy(request->path, path_start, path_len);
    request->path[path_len] = '\0';
    
   	if (request->path[0] != '/') {
    	return -1;
    }

    const char *version_start = space2 + 1;
    size_t version_len = (size_t)(cr - version_start);
    if (version_len == 0 || version_len >= sizeof(request->version)) {
        return -1;
    }

    memcpy(request->version, version_start, version_len);
    request->version[version_len] = '\0';

    if (strcmp(request->version, "HTTP/1.1") != 0) {
        return -1;
    }

    return 0;
}
