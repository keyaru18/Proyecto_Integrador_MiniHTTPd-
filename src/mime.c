#define _GNU_SOURCE

#include "mime.h"

#include <stddef.h>
#include <strings.h>
#include <string.h>

struct mime_entrada {
    const char *extension;
    const char *tipo;
};

static const struct mime_entrada tabla_mime[] = {
    /* Requeridos por el enunciado */
    {".html", "text/html"},
    {".css",  "text/css"},
    {".js",   "application/javascript"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    /* Adicionales de uso comun */
    {".htm",  "text/html"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".svg",  "image/svg+xml"},
    {".ico",  "image/x-icon"},
    {".txt",  "text/plain"},
    {".json", "application/json"},
};

#define N_MIME (sizeof(tabla_mime) / sizeof(tabla_mime[0]))

const char *mime_tipo_por_ruta(const char *ruta)
{
    if (ruta == NULL) {
        return "application/octet-stream";
    }

    /* Solo cuenta el punto del ultimo componente del path. */
    const char *base = strrchr(ruta, '/');
    base = (base != NULL) ? base + 1 : ruta;

    const char *ext = strrchr(base, '.');
    if (ext == NULL) {
        return "application/octet-stream";
    }

    for (size_t i = 0; i < N_MIME; i++) {
        if (strcasecmp(ext, tabla_mime[i].extension) == 0) {
            return tabla_mime[i].tipo;
        }
    }
    return "application/octet-stream";
}
