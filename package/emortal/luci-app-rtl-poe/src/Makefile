# luci-app-rtl-poe — RTL8238B PoE PSE for OpenWrt
# Makefile for C daemon (src/)

CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -Werror
LDLIBS += -luci

OBJECTS := rtl-poe.o

.PHONY: compile install clean
compile: rtl-poe

rtl-poe: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

install: compile
	install -d $(DESTDIR)/usr/sbin
	install -m 0755 rtl-poe $(DESTDIR)/usr/sbin/rtl-poe

clean:
	rm -f rtl-poe $(OBJECTS)

