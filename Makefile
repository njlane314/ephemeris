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

all: ephemeris

ephemeris: $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ $(LDLIBS)

install: ephemeris
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 ephemeris $(DESTDIR)$(BINDIR)/ephemeris

clean:
	rm -f ephemeris
	rm -rf ephemeris.dSYM

.PHONY: all install clean
