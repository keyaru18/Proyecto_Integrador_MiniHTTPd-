#define _GNU_SOURCE

#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "files.h"
#include "http.h"
#include "mime.h"

#define BUFFER_CAPACIDAD        HTTP_MAX_HEADER_BLOCK
#define SALIDA_CAPACIDAD        2048
#define MAX_EVENTOS             64
#define MAX_CLIENTES            400   /* cada cliente puede tener 1 socket + 1 archivo abierto */
#define TIMEOUT_INACTIVIDAD_S   HTTP_KEEPALIVE_TIMEOUT

struct cliente {
    int fd;

    /* Entrada: bytes recibidos y aun no consumidos (pueden incluir varias solicitudes). */
    char buffer[BUFFER_CAPACIDAD];
    size_t bytes_almacenados;

    /* Salida: cabeceras (o error completo) y luego, opcionalmente, un archivo. */
    char salida[SALIDA_CAPACIDAD];
    size_t salida_total;
    size_t salida_enviada;

    int archivo_fd;               /* -1 si no hay archivo pendiente */
    off_t archivo_offset;
    size_t archivo_restante;

    int respuesta_lista;          /* hay una respuesta preparada (enviandose) */
    int cerrar_al_terminar;       /* Connection: close */
    int esperando_escritura;      /* registrado en epoll con EPOLLOUT */

    time_t ultima_actividad;
    struct cliente *prev;
    struct cliente *next;
};

struct servidor {
    int epfd;
    int listen_fd;
    const char *raiz;
    struct cliente *cabeza;       /* lista de clientes (para timeouts y cierre) */
    size_t n_clientes;
};

static volatile sig_atomic_t g_detener = 0;

void servidor_detener(void)
{
    g_detener = 1;
}

/* ------------------------------------------------------------------ */
/* Socket de escucha                                                   */
/* ------------------------------------------------------------------ */

int crear_socket(int port)
{
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *res = NULL;

    char service[6] = {0};
    if (port <= 0 || port > 65535) {
        return -1;
    }

    int n = snprintf(service, sizeof(service), "%d", port);
    if (n < 0 || (size_t)n >= sizeof(service)) {
        return -1;
    }

    if (getaddrinfo(NULL, service, &hints, &res) != 0) {
        return -1;
    }

    int sockfd = -1;

    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1) {
            continue;
        }

        /* Permite reiniciar el servidor sin esperar el TIME_WAIT. */
        int uno = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof(uno)) == -1) {
            close(sockfd);
            sockfd = -1;
            continue;
        }

        /* Socket IPv6 de doble pila: tambien acepta clientes IPv4. */
        if (p->ai_family == AF_INET6) {
            int cero = 0;
            setsockopt(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, &cero, sizeof(cero));
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            sockfd = -1;
            continue;
        }

        break;
    }
    freeaddrinfo(res);

    if (sockfd == -1) {
        return -1;
    }

    if (listen(sockfd, SOMAXCONN) == -1) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/* ------------------------------------------------------------------ */
/* Utilidades de clientes                                              */
/* ------------------------------------------------------------------ */

static time_t ahora_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void cerrar_cliente(struct servidor *s, struct cliente *c)
{
    epoll_ctl(s->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    if (c->archivo_fd != -1) {
        close(c->archivo_fd);
    }

    if (c->prev != NULL) {
        c->prev->next = c->next;
    } else {
        s->cabeza = c->next;
    }
    if (c->next != NULL) {
        c->next->prev = c->prev;
    }

    s->n_clientes--;
    free(c);
}

static int cambiar_modo(struct servidor *s, struct cliente *c, uint32_t eventos)
{
    struct epoll_event ev = {0};
    ev.events = eventos;
    ev.data.ptr = c;
    return epoll_ctl(s->epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

static void aceptar_clientes(struct servidor *s)
{
    for (;;) {
        int fd = accept4(s->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd == -1) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("accept4");
            }
            return;
        }

        if (s->n_clientes >= MAX_CLIENTES) {
            close(fd);
            continue;
        }

        struct cliente *c = calloc(1, sizeof(*c));
        if (c == NULL) {
            perror("calloc");
            close(fd);
            continue;
        }
        c->fd = fd;
        c->archivo_fd = -1;
        c->ultima_actividad = ahora_s();

        struct epoll_event ev = {0};
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        if (epoll_ctl(s->epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
            perror("epoll_ctl cliente");
            close(fd);
            free(c);
            continue;
        }

        c->next = s->cabeza;
        if (s->cabeza != NULL) {
            s->cabeza->prev = c;
        }
        s->cabeza = c;
        s->n_clientes++;
    }
}

/*
 * Cierre ordenado tras una respuesta con "Connection: close": si quedaran
 * bytes sin leer, close() enviaria un RST y el cliente podria perder la
 * respuesta (p. ej. un 400 por solicitud demasiado grande). Se avisa el fin
 * de escritura y se descarta lo que el cliente ya haya enviado.
 */
static void cierre_ordenado(struct cliente *c)
{
    char descarte[1024];
    size_t total = 0;

    shutdown(c->fd, SHUT_WR);
    while (total < 65536) {
        ssize_t n = recv(c->fd, descarte, sizeof(descarte), 0);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
}

/* Cierra las conexiones sin actividad (protege contra clientes lentos / slowloris). */
static void barrer_inactivos(struct servidor *s)
{
    time_t ahora = ahora_s();
    struct cliente *c = s->cabeza;

    while (c != NULL) {
        struct cliente *sig = c->next;
        if (ahora - c->ultima_actividad >= TIMEOUT_INACTIVIDAD_S) {
            cerrar_cliente(s, c);
        }
        c = sig;
    }
}

/* ------------------------------------------------------------------ */
/* Preparacion de respuestas                                           */
/* ------------------------------------------------------------------ */

static void responder_error(struct cliente *c, int status, int keep_alive)
{
    int n = http_build_error(c->salida, SALIDA_CAPACIDAD, status, keep_alive);
    if (n < 0) {
        n = 0;
        c->cerrar_al_terminar = 1;
    }
    c->salida_total = (size_t)n;
    c->salida_enviada = 0;
}

static void consumir(struct cliente *c, size_t bytes)
{
    if (bytes > c->bytes_almacenados) {
        bytes = c->bytes_almacenados;
    }
    memmove(c->buffer, c->buffer + bytes, c->bytes_almacenados - bytes);
    c->bytes_almacenados -= bytes;
}

/*
 * Intenta extraer una solicitud completa del buffer y deja lista su respuesta.
 * Retorna 1 si hay una respuesta preparada, 0 si faltan datos.
 */
static int preparar_siguiente(struct servidor *s, struct cliente *c)
{
    char *fin = memmem(c->buffer, c->bytes_almacenados, "\r\n\r\n", 4);
    size_t bloque;
    int keep = 0;
    int status;

    if (fin == NULL) {
        if (c->bytes_almacenados < BUFFER_CAPACIDAD) {
            return 0;   /* aun incompleta: esperar mas datos */
        }
        /* Buffer lleno sin fin de cabeceras: solicitud demasiado grande. */
        bloque = c->bytes_almacenados;
        status = 400;
        printf("(solicitud demasiado grande) -> 400\n");
        responder_error(c, status, 0);
    } else {
        bloque = (size_t)(fin - c->buffer) + 4;

        struct http_request req;
        if (parse_http_request(c->buffer, bloque, &req) != 0) {
            status = 400;
            printf("(solicitud invalida) -> 400\n");
            responder_error(c, status, 0);
        } else if (strcmp(req.method, "GET") != 0) {
            /* Solo GET. Se cierra: no sabemos si el metodo trae un cuerpo. */
            status = 405;
            printf("%s %s -> 405\n", req.method, req.path);
            responder_error(c, status, 0);
        } else if (req.tiene_cuerpo) {
            /* Un GET con cuerpo no se soporta: se cierra para no desincronizar el flujo. */
            status = 400;
            printf("GET %s con cuerpo -> 400\n", req.path);
            responder_error(c, status, 0);
        } else {
            char ruta[PATH_MAX];
            int fd = -1;
            off_t tam = 0;

            keep = req.keep_alive;
            status = files_abrir(s->raiz, req.path, ruta, sizeof(ruta), &fd, &tam);

            if (status == 200) {
                int n = http_build_head(c->salida, SALIDA_CAPACIDAD, 200,
                                        mime_tipo_por_ruta(ruta), (size_t)tam, keep);
                if (n < 0) {
                    close(fd);
                    status = 500;
                    responder_error(c, status, keep);
                } else {
                    c->salida_total = (size_t)n;
                    c->salida_enviada = 0;
                    if (tam > 0) {
                        c->archivo_fd = fd;
                        c->archivo_offset = 0;
                        c->archivo_restante = (size_t)tam;
                    } else {
                        close(fd);
                    }
                }
            } else {
                if (status == 400) {
                    keep = 0;
                }
                responder_error(c, status, keep);
            }
            printf("GET %s -> %d\n", req.path, status);
        }
    }

    consumir(c, bloque);
    c->cerrar_al_terminar = c->cerrar_al_terminar || !keep;
    c->respuesta_lista = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Envio no bloqueante                                                 */
/* ------------------------------------------------------------------ */

/*
 * Envia lo pendiente (cabeceras y luego archivo con sendfile).
 * Retorna 1 si termino, 0 si el socket se bloqueo (esperar EPOLLOUT), -1 si hubo error.
 */
static int enviar_pendiente(struct cliente *c)
{
    while (c->salida_enviada < c->salida_total) {
        int flags = MSG_NOSIGNAL | (c->archivo_restante > 0 ? MSG_MORE : 0);
        ssize_t n = send(c->fd, c->salida + c->salida_enviada,
                         c->salida_total - c->salida_enviada, flags);
        if (n > 0) {
            c->salida_enviada += (size_t)n;
            c->ultima_actividad = ahora_s();
        } else if (n == -1 && errno == EINTR) {
            continue;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            return -1;
        }
    }

    while (c->archivo_restante > 0) {
        ssize_t n = sendfile(c->fd, c->archivo_fd, &c->archivo_offset, c->archivo_restante);
        if (n > 0) {
            c->archivo_restante -= (size_t)n;
            c->ultima_actividad = ahora_s();
        } else if (n == -1 && errno == EINTR) {
            continue;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            return -1;   /* error, o archivo truncado mientras se enviaba */
        }
    }

    if (c->archivo_fd != -1) {
        close(c->archivo_fd);
        c->archivo_fd = -1;
    }
    return 1;
}

/*
 * Avanza el estado de un cliente: envia lo pendiente y procesa las solicitudes
 * que ya esten en el buffer (pipelining). Puede liberar al cliente.
 */
static void atender(struct servidor *s, struct cliente *c)
{
    for (;;) {
        if (!c->respuesta_lista) {
            if (!preparar_siguiente(s, c)) {
                return;   /* esperando mas datos del cliente */
            }
        }

        int r = enviar_pendiente(c);
        if (r < 0) {
            cerrar_cliente(s, c);
            return;
        }
        if (r == 0) {
            /* Dejar de leer y esperar a poder escribir (contrapresion). */
            if (!c->esperando_escritura) {
                c->esperando_escritura = 1;
                if (cambiar_modo(s, c, EPOLLOUT) == -1) {
                    cerrar_cliente(s, c);
                }
            }
            return;
        }

        /* Respuesta enviada por completo. */
        c->respuesta_lista = 0;
        c->salida_total = 0;
        c->salida_enviada = 0;

        if (c->cerrar_al_terminar) {
            cierre_ordenado(c);
            cerrar_cliente(s, c);
            return;
        }
        if (c->esperando_escritura) {
            c->esperando_escritura = 0;
            if (cambiar_modo(s, c, EPOLLIN) == -1) {
                cerrar_cliente(s, c);
                return;
            }
        }
    }
}

static void leer_cliente(struct servidor *s, struct cliente *c)
{
    size_t libre = BUFFER_CAPACIDAD - c->bytes_almacenados;

    if (libre > 0) {
        ssize_t n = recv(c->fd, c->buffer + c->bytes_almacenados, libre, 0);
        if (n == 0) {
            cerrar_cliente(s, c);   /* el cliente cerro la conexion */
            return;
        }
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            cerrar_cliente(s, c);
            return;
        }
        c->bytes_almacenados += (size_t)n;
        c->ultima_actividad = ahora_s();
    }

    atender(s, c);
}

/* ------------------------------------------------------------------ */
/* Bucle de eventos                                                    */
/* ------------------------------------------------------------------ */

int iniciar_servidor(int sockfd, const char *raiz)
{
    struct servidor s = {0};
    s.epfd = -1;
    s.listen_fd = sockfd;
    s.raiz = raiz;

    int resultado = 0;

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl sockfd");
        return -1;
    }

    s.epfd = epoll_create1(EPOLL_CLOEXEC);
    if (s.epfd == -1) {
        perror("epoll_create1");
        return -1;
    }

    /* El socket de escucha se distingue de los clientes por data.ptr == NULL. */
    struct epoll_event evento = {0};
    evento.events = EPOLLIN;
    evento.data.ptr = NULL;
    if (epoll_ctl(s.epfd, EPOLL_CTL_ADD, sockfd, &evento) == -1) {
        perror("epoll_ctl");
        close(s.epfd);
        return -1;
    }

    struct epoll_event eventos[MAX_EVENTOS];
    time_t ultimo_barrido = ahora_s();

    while (!g_detener) {
        int n_eventos = epoll_wait(s.epfd, eventos, MAX_EVENTOS, 1000);

        if (n_eventos == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            resultado = -1;
            break;
        }

        for (int i = 0; i < n_eventos; i++) {
            if (eventos[i].data.ptr == NULL) {
                aceptar_clientes(&s);
                continue;
            }

            struct cliente *c = eventos[i].data.ptr;
            uint32_t ev = eventos[i].events;

            if (ev & (EPOLLERR | EPOLLHUP)) {
                cerrar_cliente(&s, c);
            } else if (ev & EPOLLOUT) {
                atender(&s, c);
            } else if (ev & EPOLLIN) {
                leer_cliente(&s, c);
            }
        }

        time_t ahora = ahora_s();
        if (ahora - ultimo_barrido >= 1) {
            barrer_inactivos(&s);
            ultimo_barrido = ahora;
        }
    }

    while (s.cabeza != NULL) {
        cerrar_cliente(&s, s.cabeza);
    }
    close(s.epfd);
    return resultado;
}
