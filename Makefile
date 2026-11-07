CC = gcc
CFLAGS = -O2 -Wall -Wextra $(shell pkg-config --cflags gtk+-3.0 gtk-layer-shell-0 json-glib-1.0)
LDFLAGS = $(shell pkg-config --libs gtk+-3.0 gtk-layer-shell-0 json-glib-1.0)

all: ytmusic-popup

ytmusic-popup: ytmusic-popup.c
	$(CC) $(CFLAGS) ytmusic-popup.c -o ytmusic-popup $(LDFLAGS)

clean:
	rm -f ytmusic-popup
