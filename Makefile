CC      ?= gcc
CFLAGS  ?= -O2
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Iinclude -fstack-protector-strong -D_FORTIFY_SOURCE=2
TARGET  := minihttpd

SRC := src/main.c src/server.c src/http.c src/mime.c src/files.c
OBJ := $(SRC:.c=.o)

.PHONY: all clean run debug

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c $(wildcard include/*.h)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET) 8080 www

# Compilacion con AddressSanitizer/UBSan para detectar errores de memoria.
debug: clean
	$(MAKE) CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined"

clean:
	rm -f $(OBJ) $(TARGET)
