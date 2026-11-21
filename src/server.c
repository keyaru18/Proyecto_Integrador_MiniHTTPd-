#define _POSIX_C_SOURCE 200112L
#define BUFFER_CAPACIDAD 8192

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"

struct cliente {
    int fd;
    char buffer[BUFFER_CAPACIDAD];
    size_t bytes_almacenados;

    const char *respuesta;
    size_t bytes_respuesta_total;
    size_t bytes_respuesta_enviados;
    int cerrar_al_terminar;
};


int crear_socket(int port) {
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

	int status = getaddrinfo(NULL, service, &hints, &res);
	if (status != 0) {
       	return -1;
	}

	int sockfd = -1;

	for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
		sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (sockfd == -1) {
        continue;
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

    if (listen(sockfd, 10) == -1) {
        close(sockfd);
        return -1;
    }
        
    return sockfd;
}

int iniciar_servidor(int sockfd) {
	int flags = fcntl(sockfd, F_GETFL, 0);
	if (flags == -1) {
		perror("fcntl F_GETFL sockfd");
		return -1;
	}
	if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
	    perror("fcntl F_SETFL sockfd");
	    return -1;
	}
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1");
        return -1;
    }

    struct epoll_event evento = {0};
    evento.events = EPOLLIN;
    evento.data.ptr = NULL;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &evento) == -1) {
        perror("epoll_ctl");
        close(epfd);
        return -1;
    }

    struct epoll_event eventos[10];

    while (1) {
        int n_eventos = epoll_wait(epfd, eventos, 10, -1);

        if (n_eventos == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("epoll_wait");
            close(epfd);
            return -1;
        }

        for (int i = 0; i < n_eventos; i++) {
            if (eventos[i].data.ptr == NULL) {
                int client_fd = accept(sockfd, NULL, NULL);
                if (client_fd == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;
                    }
                    perror("accept");
                    continue;
                }

                struct cliente *cliente = malloc(sizeof(*cliente));
                if (cliente == NULL) {
                    perror("malloc");
                    close(client_fd);
                    continue;
                }

                cliente->fd = client_fd;
                cliente->bytes_almacenados = 0;
                cliente->respuesta = NULL;
                cliente->bytes_respuesta_total = 0;
                cliente->bytes_respuesta_enviados = 0;
                cliente->cerrar_al_terminar = 0;

                int client_flags = fcntl(client_fd, F_GETFL, 0);
                if (client_flags == -1) {
                    perror("fcntl F_GETFL");
                    close(client_fd);
                    free(cliente);
                    continue;
                }

                if (fcntl(client_fd, F_SETFL, client_flags | O_NONBLOCK) == -1) {
                    perror("fcntl F_SETFL");
                    close(client_fd);
                    free(cliente);
                    continue;
                }

                struct epoll_event ev_cliente = {0};
                ev_cliente.events = EPOLLIN;
                ev_cliente.data.ptr = cliente;

                if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev_cliente) == -1) {
                    perror("epoll_ctl cliente");
                    close(client_fd);
                    free(cliente);
                    continue;
                }
            } else {
                struct cliente *cliente = eventos[i].data.ptr;
                if (cliente->bytes_almacenados >= BUFFER_CAPACIDAD) {
                	epoll_ctl(epfd, EPOLL_CTL_DEL, cliente->fd, NULL);
                	close(cliente->fd);
                	free(cliente);
                	continue;
                }
                ssize_t leidos = recv(
                	cliente->fd, 
                	cliente->buffer + cliente->bytes_almacenados, 
                	BUFFER_CAPACIDAD - cliente->bytes_almacenados, 
                		0);

                if (leidos > 0) {
                cliente->bytes_almacenados += (size_t)leidos;
            
                static const char patron[] = "\r\n\r\n";
                size_t pos_fin_cabeceras = 0;
                int cabeceras_encontradas = 0;
            
                if (cliente->bytes_almacenados >= 4) {
                    for (size_t i = 0; i <= cliente->bytes_almacenados - 4; i++) {
                        if (memcmp(cliente->buffer + i, patron, 4) == 0) {
                            pos_fin_cabeceras = i;
                            cabeceras_encontradas = 1;
                            break;
                        }
                    }
                }
            
                if (!cabeceras_encontradas) {
                    continue;
                }
            
                const char *cr = memchr(cliente->buffer, '\r', cliente->bytes_almacenados);
                if (cr == NULL) {
                    continue;
                }
            
                if ((size_t)(cr - cliente->buffer) + 1 >= cliente->bytes_almacenados) {
                    continue;
                }
            
                if (cr[1] != '\n') {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, cliente->fd, NULL);
                    close(cliente->fd);
                    free(cliente);
                    continue;
                }
            
                size_t longitud_linea = (size_t)(cr - cliente->buffer) + 2;
                struct http_request req = {0};
            
                int resultado = parse_http_request(cliente->buffer, longitud_linea, &req);
                if (resultado == -1) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, cliente->fd, NULL);
                    close(cliente->fd);
                    free(cliente);
                    continue;
                }
            
                if (strcmp(req.method, "GET") != 0) {
            		const char *respuesta =
            		    "HTTP/1.1 405 Method Not Allowed\r\n"
            		    "Allow: GET\r\n"
            		    "Content-Length: 0\r\n"
            		    "Connection: close\r\n"
            		    "\r\n";

            		cliente->respuesta = respuesta;
           		    cliente->bytes_respuesta_total = strlen(respuesta);
           		    cliente->bytes_respuesta_enviados = 0;
           		    cliente->cerrar_al_terminar = 1;

           		    ssize_t enviados = send(cliente->fd, respuesta, strlen(respuesta), 0);
           		    if (enviados == -1) {
           		        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
           		        
           		        } else {
           		            perror("send");
           		            epoll_ctl(epfd, EPOLL_CTL_DEL, cliente->fd, NULL);
           		            close(cliente->fd);
           		            free(cliente);
           		            continue;
           		        }
           		    }
                }
            } else if (leidos == 0) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, cliente->fd, NULL);
                close(cliente->fd);
                free(cliente);
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                perror("recv");
                epoll_ctl(epfd, EPOLL_CTL_DEL, cliente->fd, NULL);
                close(cliente->fd);
                free(cliente);
            }
        }
    }
}
