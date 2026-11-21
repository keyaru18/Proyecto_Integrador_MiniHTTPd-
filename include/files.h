#ifndef FILES_H
#define FILES_H

#include <stddef.h>
#include <sys/types.h>

/*
 * Resuelve de forma segura 'uri_path' (path del URI, sin query) dentro del
 * directorio raiz 'raiz' (que debe ser un path canonico, es decir, ya pasado
 * por realpath()) y abre el archivo.
 *
 * Pasos de seguridad: decodificacion %XX (rechaza %00), realpath() y
 * verificacion de que el resultado sigue dentro de 'raiz' (directory
 * traversal, symlinks que escapan), solo archivos regulares.
 * Si el path es un directorio se sirve su index.html.
 *
 * Retorna el codigo HTTP correspondiente:
 *   200  OK: *fd_out es un descriptor abierto (el llamador lo cierra),
 *        *size_out el tamano y 'ruta_out' el path real (para el tipo MIME)
 *   400  URI mal codificado o demasiado largo
 *   403  fuera de la raiz, directorio sin index.html, sin permisos
 *   404  no existe
 *   500  error interno
 */
int files_abrir(const char *raiz, const char *uri_path,
                char *ruta_out, size_t ruta_cap,
                int *fd_out, off_t *size_out);

#endif
