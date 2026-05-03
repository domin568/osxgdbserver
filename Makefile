# Makefile - Build gdbserver for Darwin/macOS (PowerPC)
#
# Usage:
#   make                  (build gdbserver)
#   make clean            (remove build artifacts)
#   make install          (install to PREFIX, default /usr/local)
#   make uninstall        (remove installed binary)
#

SRCDIR  = src
INCDIR  = include
OBJDIR  = obj
BINDIR  = bin
PREFIX  = /usr/local

CC = cc
CFLAGS = -g -Wall -Wimplicit
LDFLAGS =
INCLUDES = -I$(INCDIR)

INTERNAL_CFLAGS = $(CFLAGS) $(INCLUDES)

NAMES = inferiors regcache remote-utils server target \
        utils mem-break signals \
        darwin-low darwin-ppc-low reg-ppc

OBS = $(addprefix $(OBJDIR)/, $(addsuffix .o, $(NAMES)))

LIBS = -framework System

.PHONY: all clean install uninstall

all: $(BINDIR)/gdbserver

$(OBJDIR) $(BINDIR):
	mkdir -p $@

$(BINDIR)/gdbserver: $(OBS) | $(BINDIR)
	$(CC) $(LDFLAGS) -o $@ $(OBS) $(LIBS)
	@echo "=== gdbserver built successfully ==="
	@echo "Usage: sudo ./bin/gdbserver HOST:PORT /path/to/program [args]"
	@echo "   or: sudo ./bin/gdbserver HOST:PORT --attach PID"

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) -c $(INTERNAL_CFLAGS) -o $@ $<

$(OBJDIR)/signals.o: $(SRCDIR)/signals.c | $(OBJDIR)
	$(CC) -c $(INTERNAL_CFLAGS) -DGDBSERVER -o $@ $<

# Dependencies
$(OBJDIR)/server.o:         $(SRCDIR)/server.c         $(INCDIR)/server.h $(INCDIR)/config.h
$(OBJDIR)/inferiors.o:      $(SRCDIR)/inferiors.c      $(INCDIR)/server.h
$(OBJDIR)/regcache.o:       $(SRCDIR)/regcache.c       $(INCDIR)/server.h $(INCDIR)/regdef.h
$(OBJDIR)/remote-utils.o:   $(SRCDIR)/remote-utils.c   $(INCDIR)/server.h $(INCDIR)/terminal.h
$(OBJDIR)/target.o:         $(SRCDIR)/target.c         $(INCDIR)/server.h
$(OBJDIR)/utils.o:          $(SRCDIR)/utils.c          $(INCDIR)/server.h
$(OBJDIR)/mem-break.o:      $(SRCDIR)/mem-break.c      $(INCDIR)/server.h
$(OBJDIR)/darwin-low.o:     $(SRCDIR)/darwin-low.c     $(INCDIR)/darwin-low.h $(INCDIR)/server.h
$(OBJDIR)/darwin-ppc-low.o: $(SRCDIR)/darwin-ppc-low.c $(INCDIR)/darwin-low.h $(INCDIR)/server.h
$(OBJDIR)/reg-ppc.o:        $(SRCDIR)/reg-ppc.c        $(INCDIR)/regdef.h

clean:
	rm -rf $(OBJDIR) $(BINDIR)

install: $(BINDIR)/gdbserver
	install -d $(PREFIX)/bin
	install -m 755 $(BINDIR)/gdbserver $(PREFIX)/bin/gdbserver
	@echo "=== gdbserver installed to $(PREFIX)/bin/gdbserver ==="

uninstall:
	rm -f $(PREFIX)/bin/gdbserver
	@echo "=== gdbserver removed from $(PREFIX)/bin ==="
