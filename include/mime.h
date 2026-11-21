#ifndef MIME_H
#define MIME_H

/*
 * Devuelve el tipo MIME segun la extension de 'ruta' (sin distinguir
 * mayusculas). Si la extension no es conocida: "application/octet-stream".
 * Nunca retorna NULL.
 */
const char *mime_tipo_por_ruta(const char *ruta);

#endif
