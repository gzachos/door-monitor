
CC = gcc
CFLAGS = -Wall -Wundef
LDLIBS = -lwiringPi -lpthread
OBJECTS =
BIN = door-monitor.elf
TARGETDIR = /root/bin/

airtemp-lcd: door-monitor.c
	$(CC) $(CFLAGS) -DTARGET_DIR="$(TARGETDIR)" $^ --output $(BIN) $(LDLIBS)

.PHONY: clean install uninstall

clean:
	rm -f $(BIN)

install:
	if [ ! -d "$(TARGETDIR)" ]; then \
		mkdir -p "$(TARGETDIR)"; \
	fi
	cp $(BIN) "$(TARGETDIR)"
	cp door-sendmail.sh "$(TARGETDIR)"

uninstall:
	rm -f "$(TARGETDIR)/$(BIN)"
	rm -f "$(TARGETDIR)/door-sendmail.sh"

