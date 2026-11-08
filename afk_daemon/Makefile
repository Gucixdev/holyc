# afkd - needs wayland-client + wayland-scanner + wayland-protocols
PROTO = /usr/share/wayland-protocols/staging/ext-idle-notify/ext-idle-notify-v1.xml
CFLAGS ?= -O2 -Wall -Wextra
B = build
all: $(B)/afkd
$(B)/ext-idle-notify-v1-client-protocol.h: $(PROTO)
	@mkdir -p $(B); wayland-scanner client-header $< $@
$(B)/ext-idle-notify-v1-server-protocol.h: $(PROTO)
	@mkdir -p $(B); wayland-scanner server-header $< $@
$(B)/ext-idle-notify-v1-protocol.c: $(PROTO)
	@mkdir -p $(B); wayland-scanner private-code $< $@
$(B)/afkd: src/afkd.c $(B)/ext-idle-notify-v1-client-protocol.h $(B)/ext-idle-notify-v1-protocol.c
	$(CC) $(CFLAGS) -s -I$(B) -o $@ src/afkd.c $(B)/ext-idle-notify-v1-protocol.c -lwayland-client
$(B)/fakecomp: tests/fakecomp.c $(B)/ext-idle-notify-v1-server-protocol.h $(B)/ext-idle-notify-v1-protocol.c
	$(CC) $(CFLAGS) -I$(B) -o $@ tests/fakecomp.c $(B)/ext-idle-notify-v1-protocol.c -lwayland-server
test: $(B)/afkd $(B)/fakecomp
	nice -n 19 tests/run
clean:
	rm -rf $(B)
.PHONY: all test clean
