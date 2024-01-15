CC=gcc
CCFLAGS=$(shell pkg-config --cflags libcurl) -ggdb3 -O0 --std=c99 -Wall -Wextra -Wwrite-strings
LDFLAGS=$(shell pkg-config --libs libcurl) -lpthread -ljson-c -lzip -lmosquitto
TESTFLAGS=-fsanitize=leak -fsanitize=address -fsanitize=undefined
TARGET=esp32-home-client
SOURCES=*.c

all:	$(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CCFLAGS) $(SOURCES) $(LDFLAGS) -o $(TARGET)

clean:
	rm -rf $(TARGET)
	rm -rf *.o

rebuild:
	$(clean)
	$(CC) $(LDFLAGS) $(CCFLAGS) $(SOURCES) -o $(TARGET)

test:
	$(clean)
	$(CC) $(LDFLAGS) $(CCFLAGS) $(TESTFLAGS) $(SOURCES) -o $(TARGET)
install: $(TARGET)
	install $(TARGET) ~/bin/