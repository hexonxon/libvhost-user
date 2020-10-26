include Makefile.common

BINDIR := build-x86

HDRS := $(wildcard include/*/*.h)
SRCS := $(wildcard *.c)
OBJS := $(patsubst %.c,$(BINDIR)/%.o,$(SRCS))
LIBS := $(BINDIR)/virtio/libvirtqueue.a

TARGET := $(BINDIR)/libvhost.so

all: $(TARGET)

libs:
	$(MAKE) CONFIG_DEBUG=$(CONFIG_DEBUG) -C virtio

$(LIBS): libs

$(BINDIR):
	mkdir -p $@

$(BINDIR)/%.o: %.c
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(TARGET): $(BINDIR) $(HDRS) $(OBJS) $(LIBS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -shared -o $@

server: $(TARGET)
	$(MAKE) -C tools/server

unit-tests: $(TARGET) $(LIBS)
	$(MAKE) -C tests

clean:
	rm -rf $(BINDIR)

.PHONY: all clean server libs
