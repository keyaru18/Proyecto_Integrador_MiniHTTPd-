#ifndef SERVER_H
#define SERVER_H

/* Crea un socket TCP en modo escucha en 'port' (IPv4/IPv6). -1 si falla. */
int crear_socket(int port);

/*
 * Bucle de eventos (epoll). Atiende multiples clientes con conexiones
 * persistentes y sirve archivos estaticos desde 'raiz' (path canonico).
 * Retorna 0 al terminar por servidor_detener() y -1 ante un error fatal.
 */
int iniciar_servidor(int sockfd, const char *raiz);

/* Pide al bucle que termine (seguro para llamar desde un manejador de senales). */
void servidor_detener(void);

#endif
