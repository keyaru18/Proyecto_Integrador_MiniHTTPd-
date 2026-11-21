#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "server.h"

#define PUERTO_POR_DEFECTO 8080
#define RAIZ_POR_DEFECTO   "www"

static void manejador_senal(int sig)
{
    (void)sig;
    servidor_detener();
}

static void uso(const char *prog)
{
    fprintf(stderr, "Uso: %s [puerto] [directorio_raiz]\n"
                    "  puerto          por defecto %d\n"
                    "  directorio_raiz por defecto \"%s\"\n",
            prog, PUERTO_POR_DEFECTO, RAIZ_POR_DEFECTO);
}

int main(int argc, char **argv)
{
    int puerto = PUERTO_POR_DEFECTO;
    const char *dir = RAIZ_POR_DEFECTO;

    if (argc > 3) {
        uso(argv[0]);
        return EXIT_FAILURE;
    }

    if (argc >= 2) {
        char *fin = NULL;
        errno = 0;
        long v = strtol(argv[1], &fin, 10);
        if (errno != 0 || fin == argv[1] || *fin != '\0' || v < 1 || v > 65535) {
            fprintf(stderr, "Puerto invalido: %s\n", argv[1]);
            uso(argv[0]);
            return EXIT_FAILURE;
        }
        puerto = (int)v;
    }
    if (argc == 3) {
        dir = argv[2];
    }

    /* La raiz se canonicaliza una sola vez: files.c compara contra este path. */
    char raiz[PATH_MAX];
    struct stat st;
    if (realpath(dir, raiz) == NULL || stat(raiz, &st) == -1 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Directorio raiz invalido: %s\n", dir);
        return EXIT_FAILURE;
    }

    /* Escribir a un cliente que cerro no debe matar al servidor. */
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa = {0};
    sa.sa_handler = manejador_senal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    setvbuf(stdout, NULL, _IOLBF, 0);

    int sockfd = crear_socket(puerto);
    if (sockfd == -1) {
        perror("crear_socket");
        return EXIT_FAILURE;
    }

    printf("MiniHTTPd escuchando en el puerto %d, sirviendo %s\n", puerto, raiz);

    int rc = iniciar_servidor(sockfd, raiz);
    close(sockfd);

    if (rc == -1) {
        fprintf(stderr, "Error al ejecutar iniciar_servidor\n");
        return EXIT_FAILURE;
    }

    printf("Servidor detenido.\n");
    return EXIT_SUCCESS;
}
