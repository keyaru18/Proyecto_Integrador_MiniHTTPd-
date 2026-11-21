#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include "server.h"

int main(void) {
    int sockfd = crear_socket(8080);
    if (sockfd == -1) {
        perror("crear_socket");
        return EXIT_FAILURE;
    }

    printf("Socket escuchando en el puerto 8080 con descriptor = %d\n", sockfd);

    if (iniciar_servidor(sockfd) == -1) {
        fprintf(stderr, "Error al ejecutar iniciar_servidor\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    close(sockfd);
    return EXIT_SUCCESS;
}
