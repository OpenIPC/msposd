CFLAGS=-O1 -g -fsanitize=address -fno-omit-frame-pointer -Wall -Wextra
LDFLAGS=-g -fsanitize=address
LDLIBS=-levent_core

mavfwd:
