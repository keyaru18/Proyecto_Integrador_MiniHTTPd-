#define _GNU_SOURCE

#include "http.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define STR_(x) #x
#define STR(x) STR_(x)

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

/* Caracter valido de un "token" HTTP (metodo, nombre de encabezado). */
static int es_tchar(unsigned char c)
{
    if (c == '\0') {
        return 0;
    }
    if (isalnum(c)) {
        return 1;
    }
    return strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/*
 * Rechaza NUL y otros caracteres de control, y exige que todo CR vaya seguido
 * de LF y todo LF precedido de CR (evita request smuggling / inyeccion).
 */
static int bloque_valido(const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)b[i];

        if (c == '\r') {
            if (i + 1 >= n || b[i + 1] != '\n') {
                return 0;
            }
        } else if (c == '\n') {
            if (i == 0 || b[i - 1] != '\r') {
                return 0;
            }
        } else if (c == '\t') {
            continue;
        } else if (c < 0x20 || c == 0x7f) {
            return 0;
        }
    }
    return 1;
}

/* Copia acotada. Si no cabe: error (truncar=0) o recorta (truncar=1). */
static int copiar(char *dst, size_t cap, const char *src, size_t len, int truncar)
{
    if (len >= cap) {
        if (!truncar) {
            return -1;
        }
        len = cap - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

static int nombre_es(const char *nombre, size_t nlen, const char *esperado)
{
    return strlen(esperado) == nlen && strncasecmp(nombre, esperado, nlen) == 0;
}

/* Busca el token "close" en el valor de Connection (lista separada por comas). */
static int connection_pide_cerrar(const char *v, size_t len)
{
    size_t i = 0;

    while (i < len) {
        while (i < len && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) {
            i++;
        }
        size_t ini = i;
        while (i < len && v[i] != ',' && v[i] != ' ' && v[i] != '\t') {
            i++;
        }
        if (i - ini == 5 && strncasecmp(v + ini, "close", 5) == 0) {
            return 1;
        }
    }
    return 0;
}

int parse_http_request(const char *buffer, size_t length, struct http_request *request)
{
    if (buffer == NULL || request == NULL || length < 4 || length > HTTP_MAX_HEADER_BLOCK) {
        return -1;
    }

    memset(request, 0, sizeof(*request));

    if (!bloque_valido(buffer, length)) {
        return -1;
    }
    if (memcmp(buffer + length - 4, "\r\n\r\n", 4) != 0) {
        return -1;
    }

    const char *p = buffer;
    const char *fin = buffer + length;

    /* ---- Linea de solicitud: METODO SP URI SP VERSION ---- */
    const char *cr = memmem(p, (size_t)(fin - p), "\r\n", 2);
    if (cr == NULL) {
        return -1;
    }

    size_t line_len = (size_t)(cr - p);
    if (line_len == 0 || line_len > HTTP_MAX_REQUEST_LINE) {
        return -1;
    }

    const char *space1 = memchr(p, ' ', line_len);
    if (space1 == NULL) {
        return -1;
    }

    size_t method_len = (size_t)(space1 - p);
    if (method_len == 0 || method_len >= sizeof(request->method)) {
        return -1;
    }
    for (size_t i = 0; i < method_len; i++) {
        if (!es_tchar((unsigned char)p[i])) {
            return -1;
        }
    }
    memcpy(request->method, p, method_len);
    request->method[method_len] = '\0';

    const char *target = space1 + 1;
    const char *space2 = memchr(target, ' ', (size_t)(cr - target));
    if (space2 == NULL) {
        return -1;
    }

    size_t target_len = (size_t)(space2 - target);
    if (target_len == 0 || target_len > HTTP_MAX_URI || target[0] != '/') {
        return -1;
    }

    /* El path termina en '?' (query) o '#' (fragmento). */
    size_t path_len = 0;
    while (path_len < target_len && target[path_len] != '?' && target[path_len] != '#') {
        path_len++;
    }
    if (copiar(request->path, sizeof(request->path), target, path_len, 0) == -1) {
        return -1;
    }

    const char *ver = space2 + 1;
    size_t ver_len = (size_t)(cr - ver);
    if (ver_len == 0 || memchr(ver, ' ', ver_len) != NULL) {
        return -1;
    }
    if (copiar(request->version, sizeof(request->version), ver, ver_len, 0) == -1) {
        return -1;
    }
    if (strcmp(request->version, "HTTP/1.1") != 0) {
        return -1;
    }

    /* ---- Encabezados ---- */
    int n_headers = 0;
    int tiene_host = 0;
    int cerrar = 0;

    p = cr + 2;
    for (;;) {
        if (p >= fin) {
            return -1;
        }

        cr = memmem(p, (size_t)(fin - p), "\r\n", 2);
        if (cr == NULL) {
            return -1;
        }

        size_t len = (size_t)(cr - p);
        if (len == 0) {          /* linea vacia: fin de encabezados */
            p = cr + 2;
            break;
        }

        if (len > HTTP_MAX_HEADER_LINE || ++n_headers > HTTP_MAX_HEADERS) {
            return -1;
        }
        if (p[0] == ' ' || p[0] == '\t') {   /* obs-fold: no soportado */
            return -1;
        }

        const char *colon = memchr(p, ':', len);
        if (colon == NULL || colon == p) {
            return -1;
        }

        size_t nlen = (size_t)(colon - p);
        for (size_t i = 0; i < nlen; i++) {
            if (!es_tchar((unsigned char)p[i])) {
                return -1;
            }
        }

        const char *v = colon + 1;
        const char *vend = cr;
        while (v < vend && (*v == ' ' || *v == '\t')) {
            v++;
        }
        while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t')) {
            vend--;
        }
        size_t vlen = (size_t)(vend - v);

        if (nombre_es(p, nlen, "Host")) {
            if (tiene_host || vlen == 0) {
                return -1;
            }
            if (copiar(request->host, sizeof(request->host), v, vlen, 0) == -1) {
                return -1;
            }
            tiene_host = 1;
        } else if (nombre_es(p, nlen, "Connection")) {
            copiar(request->connection, sizeof(request->connection), v, vlen, 1);
            if (connection_pide_cerrar(v, vlen)) {
                cerrar = 1;
            }
        } else if (nombre_es(p, nlen, "User-Agent")) {
            copiar(request->user_agent, sizeof(request->user_agent), v, vlen, 1);
        } else if (nombre_es(p, nlen, "Content-Length")) {
            /* Debe ser numerico; si es > 0 se marca el cuerpo (el servidor decide). */
            if (vlen == 0) {
                return -1;
            }
            for (size_t i = 0; i < vlen; i++) {
                if (v[i] < '0' || v[i] > '9') {
                    return -1;
                }
                if (v[i] != '0') {
                    request->tiene_cuerpo = 1;
                }
            }
        } else if (nombre_es(p, nlen, "Transfer-Encoding")) {
            request->tiene_cuerpo = 1;
        }

        p = cr + 2;
    }

    if (p != fin) {
        return -1;
    }

    /* HTTP/1.1 exige el encabezado Host. */
    if (!tiene_host) {
        return -1;
    }

    request->keep_alive = !cerrar;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Generacion de respuestas                                            */
/* ------------------------------------------------------------------ */

const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default:  return "Unknown";
    }
}

static int fecha_http(char *out, size_t cap)
{
    time_t ahora = time(NULL);
    struct tm tm;

    if (gmtime_r(&ahora, &tm) == NULL) {
        return -1;
    }
    if (strftime(out, cap, "%a, %d %b %Y %H:%M:%S GMT", &tm) == 0) {
        return -1;
    }
    return 0;
}

int http_build_head(char *out, size_t cap, int status, const char *content_type,
                    size_t content_length, int keep_alive)
{
    char fecha[64];

    if (out == NULL || cap == 0 || content_type == NULL || fecha_http(fecha, sizeof(fecha)) == -1) {
        return -1;
    }

    int n = snprintf(out, cap,
                     "HTTP/1.1 %d %s\r\n"
                     "Date: %s\r\n"
                     "Server: MiniHTTPd/1.0\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "%s"
                     "%s"
                     "\r\n",
                     status, http_status_text(status),
                     fecha,
                     content_type,
                     content_length,
                     status == 405 ? "Allow: GET\r\n" : "",
                     keep_alive
                         ? "Connection: keep-alive\r\nKeep-Alive: timeout=" STR(HTTP_KEEPALIVE_TIMEOUT) "\r\n"
                         : "Connection: close\r\n");

    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return n;
}

int http_build_error(char *out, size_t cap, int status, int keep_alive)
{
    char cuerpo[256];

    int bl = snprintf(cuerpo, sizeof(cuerpo),
                      "<!DOCTYPE html>\n"
                      "<html><head><meta charset=\"utf-8\"><title>%d %s</title></head>"
                      "<body><h1>%d %s</h1></body></html>\n",
                      status, http_status_text(status),
                      status, http_status_text(status));
    if (bl < 0 || (size_t)bl >= sizeof(cuerpo)) {
        return -1;
    }

    int hl = http_build_head(out, cap, status, "text/html", (size_t)bl, keep_alive);
    if (hl < 0) {
        return -1;
    }

    if ((size_t)hl + (size_t)bl >= cap) {
        return -1;
    }
    memcpy(out + hl, cuerpo, (size_t)bl);
    out[hl + bl] = '\0';
    return hl + bl;
}
