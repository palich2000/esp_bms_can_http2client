CC=gcc
CCFLAGS=$(shell pkg-config --cflags libcurl) -ggdb3 -O0 --std=c99 -Wall -Wextra -Wwrite-strings
LDFLAGS=$(shell pkg-config --libs libcurl) -lpthread -ljson-c -lzip -lmosquitto
TESTFLAGS=-fsanitize=leak -fsanitize=address -fsanitize=undefined
TARGET=esp32-home-client
SOURCES=dexec.c dfork.c dlog.c dmem.c dnonblock.c dpid.c dsignal.c dzip.c esp32-home-client.c


all:	$(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CCFLAGS) $(SOURCES) $(LDFLAGS) -o $(TARGET)

clean:
	rm -rf $(TARGET)
	rm -rf *.o

rebuild: clean
	$(CC) $(CCFLAGS) $(SOURCES) $(LDFLAGS) -o $(TARGET)

test: clean
	$(CC) $(CCFLAGS) $(TESTFLAGS) $(SOURCES) $(LDFLAGS) -o $(TARGET)

install: $(TARGET)
	install $(TARGET) ~/bin/

.PHONY: all clean rebuild test install