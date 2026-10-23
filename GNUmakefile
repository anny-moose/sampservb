CC ?= cc
TARGET ?= DEBUG

ifeq ($(TARGET),DEBUG) 
	CFLAGS := -Og -g -fsanitize=address,undefined
	BUILDDIR := build
else ifeq ($(TARGET),RELEASE)
	CFLAGS := -O2
	BUILDDIR := release
else
$(error Unrecognised build target $(TARGET))
endif

WERROR ?= FALSE

ifeq ($(WERROR),TRUE)
	CFLAGS += -Werror
endif

CFLAGS += -std=c99 -Wall -Wshadow -pedantic -Wextra $(shell pkg-config --cflags libcurl ncursesw libcjson)

LDLIBS := $(shell pkg-config --libs libcurl ncursesw libcjson)

BUILDDIR ?= build

OBJS := $(addprefix $(BUILDDIR)/, main.o cmd.o common.o serv.o servfetch.o states.o list.o keymap.o vector.o)

.PHONY: clean

$(BUILDDIR)/out: $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILDDIR)/%.o: src/%.c | $(BUILDDIR)
	$(CC) -c $(CFLAGS) $(CPPFLAGS) $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/main.o: src/main.c src/cmd.h src/common.h src/serv.h src/servfetch.h src/states.h src/list.h src/keymap.h
$(BUILDDIR)/cmd.o: src/cmd.c src/cmd.h src/common.h src/serv.h src/servfetch.h src/states.h src/keymap.h src/list.h
$(BUILDDIR)/common.o: src/common.c src/common.h
$(BUILDDIR)/serv.o: src/serv.c src/serv.h src/states.h src/common.h
$(BUILDDIR)/servfetch.o: src/servfetch.c src/servfetch.h src/serv.h
$(BUILDDIR)/states.o: src/states.c src/states.h src/serv.h src/servfetch.h src/cmd.h src/vector.h src/keymap.h
$(BUILDDIR)/list.o: src/list.c src/list.h
$(BUILDDIR)/keymap.o: src/keymap.c src/keymap.h src/cmd.h
$(BUILDDIR)/vector.o: src/vector.c src/vector.h

clean:
	-rm -r build release
