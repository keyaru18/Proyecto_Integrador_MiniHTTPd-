#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* Limites de tamanio: si se exceden, la solicitud es invalida (400). */
#define HTTP_MAX_URI           2047   /* longitud maxima del URI (target)   */
#define HTTP_MAX_REQUEST_LINE  2112   /* "METODO URI HTTP/1.1"              */
#define HTTP_MAX_HEADER_LINE   4096   /* una linea de encabezado            */
#define HTTP_MAX_HEADERS       64     /* cantidad de encabezados            */
#define HTTP_MAX_HEADER_BLOCK  8192   /* linea de solicitud + encabezados   */

#define HTTP_KEEPALIVE_TIMEOUT 15     /* segundos de inactividad tolerados  */

struct http_request {
    char method[16];
    char path[2048];       /* URI sin query (?) ni fragmento (#), sin decodificar */
    char version[16];
    char host[256];        /* Host:        (obligatorio en HTTP/1.1) */
    char connection[64];   /* Connection:  (valor crudo, truncado)   */
    char user_agent[256];  /* User-Agent:  (truncado)                */
    int keep_alive;        /* 1 = conexion persistente (por defecto en HTTP/1.1) */
    int tiene_cuerpo;      /* 1 si declara cuerpo (Content-Length > 0 o Transfer-Encoding);
                              este servidor no los acepta en un GET (400) */
};

/*
 * Analiza el bloque completo de cabeceras: linea de solicitud + encabezados,
 * incluyendo el "\r\n\r\n" final. 'length' es el tamano exacto del bloque.
 * Retorna 0 si es valido y -1 si es malformado o excede los limites (400).
 */
int parse_http_request(const char *buffer, size_t length, struct http_request *request);

/* Texto de razon para un codigo de estado ("OK", "Not Found", ...). */
const char *http_status_text(int status);

/*
 * Construye la linea de estado y los encabezados de una respuesta (termina en
 * linea vacia). Retorna la longitud escrita o -1 si no cabe en 'cap'.
 */
int http_build_head(char *out, size_t cap, int status, const char *content_type,
                    size_t content_length, int keep_alive);

/*
 * Construye una respuesta de error completa (encabezados + cuerpo HTML corto).
 * Retorna la longitud escrita o -1 si no cabe en 'cap'.
 */
int http_build_error(char *out, size_t cap, int status, int keep_alive);

#endif
