CXX ?= c++
PKG_CONFIG ?= pkg-config

CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic
CXXFLAGS += $(shell $(PKG_CONFIG) --cflags sqlite3 libcurl)
LDLIBS += $(shell $(PKG_CONFIG) --libs sqlite3 libcurl)

SDKROOT ?= $(shell xcrun --show-sdk-path 2>/dev/null)
ifneq ($(wildcard $(SDKROOT)/usr/include/c++/v1/algorithm),)
CXXFLAGS += -isystem $(SDKROOT)/usr/include/c++/v1
endif

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SRCS := $(wildcard src/*.cpp)
VERSION ?= v0.1.0
DISTDIR := dist
DISTFILE := $(DISTDIR)/ephemeris-$(VERSION).tar.gz

all: ephemeris

ephemeris: $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ $(LDLIBS)

test: ephemeris
	tests/smoke.sh ./ephemeris

install: ephemeris
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 ephemeris $(DESTDIR)$(BINDIR)/ephemeris

dist:
	install -d $(DISTDIR)
	git archive --format=tar --prefix=ephemeris-$(VERSION)/ HEAD | gzip -9 > $(DISTFILE)
	@echo $(DISTFILE)

clean:
	rm -f ephemeris
	rm -rf ephemeris.dSYM
	rm -rf $(DISTDIR)

.PHONY: all install clean test dist
