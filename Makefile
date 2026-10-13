CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
CUPS_LIBS := $(shell pkg-config --libs cups 2>/dev/null || echo -lcups)

PREFIX    ?= /usr
FILTERDIR ?= $(shell pkg-config --variable=cups_serverbin cups 2>/dev/null || echo $(PREFIX)/lib/cups)/filter
PPDDIR    ?= $(PREFIX)/share/ppd/ricoh-ddst
GSTORASTER ?= $(FILTERDIR)/gstoraster

MODELS := rsp100 rsp111 rsp111su rsp112 rsp204
PPDS   := $(MODELS:%=ppd/%.ppd)

all: rastertoricohddst ppds

rastertoricohddst: src/rastertoricohddst.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(CUPS_LIBS) -ljbig -lm

ddst-check: tools/ddst-check.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) -ljbig -lm

# PPDs are generated from the driver information file by CUPS' ppdc.
ppds: $(PPDS)
$(PPDS) &: ppd/ricoh-ddst.drv
	ppdc -d ppd $<

# Offline test: render test/testpage.ps through CUPS' gstoraster and the new
# filter, then compare against the legacy filter's Ghostscript pbmraw
# rendering and JBIG encoder settings. Nothing is sent to a printer.
check: rastertoricohddst ddst-check ppds
	@rm -rf test/out && mkdir -p test/out
	gs -q -dSAFER -dBATCH -dNOPAUSE -sDEVICE=pdfwrite -o test/out/doc.pdf test/testpage.ps
	PPD=ppd/rsp111su.ppd $(GSTORASTER) 1 user title 1 "" test/out/doc.pdf > test/out/doc.ras 2> test/out/gstoraster.log
	./rastertoricohddst 1 user "test job" 1 "" test/out/doc.ras > test/out/doc.prn 2> test/out/filter.log
	gs -q -dSAFER -dBATCH -dNOPAUSE -r600 -sDEVICE=pbmraw -o test/out/ref-%03d.pbm test/out/doc.pdf
	./ddst-check test/out/doc.prn test/out/ref-*.pbm

install: rastertoricohddst ppds
	install -D -m 0755 rastertoricohddst $(DESTDIR)$(FILTERDIR)/rastertoricohddst
	install -d $(DESTDIR)$(PPDDIR)
	install -m 0644 $(PPDS) $(DESTDIR)$(PPDDIR)/

uninstall:
	rm -f $(DESTDIR)$(FILTERDIR)/rastertoricohddst
	rm -rf $(DESTDIR)$(PPDDIR)

clean:
	rm -rf rastertoricohddst ddst-check test/out

.PHONY: all ppds check install uninstall clean
