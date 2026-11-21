# MiniHTTPd

Servidor HTTP/1.1 basico para Linux escrito en C, **sin bibliotecas HTTP externas**.
Proyecto integrador: sockets, parsing de protocolos, arquitectura event-driven con
`epoll`, interaccion con el sistema de archivos y seguridad.

## Caracteristicas

- Sirve archivos estaticos (HTML, CSS, JS, imagenes) desde `www/`.
- Procesa `GET` y analiza los encabezados `Host`, `Connection` y `User-Agent`.
- Multiples clientes con `epoll` (sockets no bloqueantes) y **conexiones persistentes**
  (keep-alive, pipelining, timeout de inactividad de 15 s).
- Envio de archivos con `sendfile()` y escritura no bloqueante (`EPOLLOUT`), de modo
  que un cliente lento no bloquea a los demas.
- Codigos de estado: `200`, `400`, `403`, `404`, `405`, `500`.
- Tipos MIME por extension.
- Defensas contra vulnerabilidades comunes (ver mas abajo).

## Estructura

```
minihttpd/
├── Makefile
├── README.md
├── include/
│   ├── http.h      parsing de la solicitud y generacion de respuestas
│   ├── server.h    socket de escucha y bucle de eventos
│   ├── mime.h      tipos de contenido
│   └── files.h     acceso seguro a archivos estaticos
├── src/
│   ├── main.c      argumentos, senales, arranque
│   ├── server.c    bucle epoll, clientes, keep-alive, envio no bloqueante
│   ├── http.c      parser GET + encabezados; construccion de respuestas
│   ├── mime.c      tabla extension -> MIME
│   └── files.c     decodificacion %XX, realpath(), apertura de archivos
└── www/
    ├── index.html
    ├── style.css
    └── image.png
```

## Compilar y ejecutar

```sh
make                      # genera ./minihttpd
./minihttpd               # puerto 8080, raiz ./www
./minihttpd 9000 www      # puerto y directorio raiz opcionales
```

Abrir <http://localhost:8080/>. Se detiene con `Ctrl+C` (cierra los clientes de forma ordenada).

Para detectar errores de memoria: `make debug` (AddressSanitizer + UBSan).

## Tipos MIME

| Extension | Content-Type             |
|-----------|--------------------------|
| `.html`   | `text/html`              |
| `.css`    | `text/css`               |
| `.js`     | `application/javascript` |
| `.png`    | `image/png`              |
| `.jpg`    | `image/jpeg`             |

Tambien `.htm`, `.jpeg`, `.gif`, `.svg`, `.ico`, `.txt`, `.json`. Cualquier otra
extension se sirve como `application/octet-stream`.

## Codigos de estado

| Codigo | Cuando                                                                              |
|--------|-------------------------------------------------------------------------------------|
| 200    | Archivo encontrado                                                                  |
| 400    | Solicitud malformada, sin `Host`, URI mal codificado o limites de tamano excedidos |
| 403    | Fuera de la raiz (traversal), directorio sin `index.html`, sin permisos             |
| 404    | El archivo no existe                                                                |
| 405    | Metodo distinto de `GET` (incluye `Allow: GET`)                                     |
| 500    | Error interno                                                                       |

## Seguridad

- **Directory traversal**: el URI se decodifica (`%2e%2e%2f` tambien se detecta); si un
  `..` sube por encima de la raiz se responde `403` de inmediato. Luego se une a la raiz
  y se resuelve con `realpath()`. Si el resultado no queda dentro de la
  raiz (comparando por componentes, para que `www-secreto` no pase como `www`) se
  responde `403`. Los symlinks que apuntan fuera de la raiz tambien se bloquean.
- **Buffers**: no se usa `strcpy` ni `sprintf`; solo `memcpy`/`snprintf` con tamanos
  verificados. Todo campo se copia con su longitud comprobada.
- **Metodos invalidos**: todo lo que no sea `GET` recibe `405 Method Not Allowed`.
- **Tamano de la solicitud** (`400`): URI > 2047 bytes, linea de solicitud > 2112,
  linea de encabezado > 4096, mas de 64 encabezados, bloque de cabeceras > 8192.
- **Solicitudes ambiguas** (`400`): bytes de control o NUL, `CR`/`LF` sueltos,
  encabezados con espacios antes de `:`, `Host` ausente o repetido, y un `GET` con
  cuerpo (`Content-Length` > 0 o `Transfer-Encoding`), lo que evita request smuggling.
  Un metodo distinto de `GET` se evalua primero y recibe `405` aunque traiga cuerpo.
- **Denegacion de servicio**: maximo de clientes simultaneos, cierre por inactividad
  (15 s) y cierre inmediato ante solicitudes invalidas.
- Solo se sirven archivos regulares; no se listan directorios.

## Pruebas manuales

```sh
curl -i http://localhost:8080/                            # 200 text/html
curl -i http://localhost:8080/style.css                   # 200 text/css
curl -i http://localhost:8080/image.png -o /dev/null      # 200 image/png
curl -i http://localhost:8080/no-existe                   # 404
curl -i --path-as-is http://localhost:8080/../../etc/passwd        # 403
curl -i --path-as-is "http://localhost:8080/%2e%2e/%2e%2e/etc/passwd"  # 403
curl -i -X POST http://localhost:8080/                    # 405
curl -i -H "Host:" http://localhost:8080/                 # 400 (sin Host)
curl -v http://localhost:8080/ http://localhost:8080/style.css  # reutiliza la conexion
printf 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | nc localhost 8080
```
