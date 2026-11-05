CC ?= cc
CFLAGS ?= -O3 -DNDEBUG
CEDAR_FLAGS = -std=c11 -pthread -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -I. -I..
CEDAR_SOURCES = $(wildcard ecs/*.c) wexds.c

.PHONY: example
example:
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CEDAR_FLAGS) examples/quickstart.c $(CEDAR_SOURCES) $(LDFLAGS) -lm $(LDLIBS) -o build/quickstart
	./build/quickstart
