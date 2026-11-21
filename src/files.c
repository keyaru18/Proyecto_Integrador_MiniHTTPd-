#define _GNU_SOURCE

#include "files.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define URI_DECODIFICADO_MAX 2048

static int valor_hex(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Decodifica %XX. Rechaza secuencias invalidas y %00 (byte nulo). */
static int decodificar_url(const char *src, char *dst, size_t cap)
{
    size_t j = 0;

    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c == '%') {
            int alto = valor_hex((unsigned char)src[i + 1]);
            if (alto < 0) {
                return -1;
            }
            int bajo = valor_hex((unsigned char)src[i + 2]);
            if (bajo < 0) {
                return -1;
            }
            c = (unsigned char)(alto * 16 + bajo);
            if (c == '\0') {
                return -1;
            }
            i += 2;
        }

        if (j + 1 >= cap) {
            return -1;
        }
        dst[j++] = (char)c;
    }

    dst[j] = '\0';
    return 0;
}

/*
 * Verificacion lexica previa: 1 si algun ".." intenta subir por encima de la
 * raiz (p. ej. "/../../etc/passwd"). Asi el resultado es 403 sin depender de
 * que el archivo de destino exista; realpath() sigue siendo la defensa real.
 */
static int escapa_de_raiz(const char *ruta)
{
    int profundidad = 0;
    const char *p = ruta;

    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        const char *ini = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        size_t len = (size_t)(p - ini);

        if (len == 2 && ini[0] == '.' && ini[1] == '.') {
            if (--profundidad < 0) {
                return 1;
            }
        } else if (len > 0 && !(len == 1 && ini[0] == '.')) {
            profundidad++;
        }
    }
    return 0;
}

/* 1 si 'ruta' es 'raiz' o esta dentro de ella (compara por componentes). */
static int dentro_de_raiz(const char *raiz, const char *ruta)
{
    size_t len = strlen(raiz);

    if (len == 1 && raiz[0] == '/') {
        return ruta[0] == '/';
    }
    if (strncmp(ruta, raiz, len) != 0) {
        return 0;
    }
    /* Evita que /var/www-secreto pase como si estuviera dentro de /var/www */
    return ruta[len] == '/' || ruta[len] == '\0';
}

static int estado_por_errno(int e)
{
    switch (e) {
    case ENOENT:
    case ENOTDIR:
    case ENAMETOOLONG:
        return 404;
    case EACCES:
    case ELOOP:
    case EPERM:
        return 403;
    default:
        return 500;
    }
}

int files_abrir(const char *raiz, const char *uri_path,
                char *ruta_out, size_t ruta_cap,
                int *fd_out, off_t *size_out)
{
    char decodificada[URI_DECODIFICADO_MAX];
    char candidato[PATH_MAX];
    char real[PATH_MAX];
    struct stat st;
    int n;

    if (fd_out == NULL || size_out == NULL || ruta_out == NULL || ruta_cap == 0) {
        return 500;
    }
    *fd_out = -1;
    *size_out = 0;

    if (raiz == NULL || uri_path == NULL || uri_path[0] != '/') {
        return 400;
    }
    if (decodificar_url(uri_path, decodificada, sizeof(decodificada)) == -1) {
        return 400;
    }
    if (escapa_de_raiz(decodificada)) {
        return 403;
    }

    n = snprintf(candidato, sizeof(candidato), "%s%s", raiz, decodificada);
    if (n < 0 || (size_t)n >= sizeof(candidato)) {
        return 400;
    }

    /* realpath() resuelve "..", "." y symlinks: es la defensa contra traversal. */
    if (realpath(candidato, real) == NULL) {
        return estado_por_errno(errno);
    }
    if (!dentro_de_raiz(raiz, real)) {
        return 403;
    }
    if (stat(real, &st) == -1) {
        return estado_por_errno(errno);
    }

    if (S_ISDIR(st.st_mode)) {
        n = snprintf(candidato, sizeof(candidato), "%s/index.html", real);
        if (n < 0 || (size_t)n >= sizeof(candidato)) {
            return 400;
        }
        if (realpath(candidato, real) == NULL) {
            /* Directorio sin index.html: no se lista su contenido. */
            return (errno == ENOENT) ? 403 : estado_por_errno(errno);
        }
        if (!dentro_de_raiz(raiz, real)) {
            return 403;
        }
        if (stat(real, &st) == -1) {
            return estado_por_errno(errno);
        }
    }

    if (!S_ISREG(st.st_mode)) {
        return 403;
    }

    int fd = open(real, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd == -1) {
        return estado_por_errno(errno);
    }

    /* Revalida sobre el descriptor ya abierto. */
    if (fstat(fd, &st) == -1 || !S_ISREG(st.st_mode)) {
        close(fd);
        return 403;
    }

    n = snprintf(ruta_out, ruta_cap, "%s", real);
    if (n < 0 || (size_t)n >= ruta_cap) {
        close(fd);
        return 500;
    }

    *fd_out = fd;
    *size_out = st.st_size;
    return 200;
}
