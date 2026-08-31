CC := clang
CFLAGS := -O1 -Werror -Wall -pedantic -Wextra -g -fsanitize=address,undefined $(shell pkg-config --cflags libcurl ncursesw libcjson)
LFLAGS := $(shell pkg-config --libs libcurl ncursesw libcjson)

.PHONY: clean

build/out: build/main.o build/cmd.o build/common.o build/serv.o build/servfetch.o build/states.o
	${CC} ${CFLAGS} ${LFLAGS} $^ -o $@

build/main.o: src/main.c src/cmd.h src/common.h src/serv.h src/servfetch.h src/states.h
	${CC} ${CFLAGS} $< -c -o $@
	
build/cmd.o: src/cmd.c src/cmd.h src/common.h src/serv.h
	${CC} ${CFLAGS} $< -c -o $@

build/common.o: src/common.c src/common.h
	${CC} ${CFLAGS} $< -c -o $@

build/serv.o: src/serv.c src/serv.h src/states.h
	${CC} ${CFLAGS} $< -c -o $@

build/servfetch.o: src/servfetch.c src/servfetch.h src/serv.h
	${CC} ${CFLAGS} $< -c -o $@

build/states.o: src/states.c src/states.h src/serv.h src/cmd.h
	${CC} ${CFLAGS} $< -c -o $@

clean:
	-rm -r build/*
