ROOT_DIR := $(abspath $(CURDIR)/..)
include $(ROOT_DIR)/Makefile.common

BINDIR := $(ROOT_DIR)/build-x86/virtio

HDRS := $(wildcard include/*/*.h)
SRCS := $(wildcard *.c)
OBJS := $(patsubst %.c,$(BINDIR)/%.o,$(SRCS))

TARGET := $(BINDIR)/libvirtqueue.a

all: $(TARGET)

$(BINDIR):
	mkdir -p $@

$(BINDIR)/%.o: %.c
	$(CC) $(CFLAGS) -I$(ROOT_DIR)/include -fPIC -c $< -o $@

$(TARGET): $(BINDIR) $(HDRS) $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -rf $(BINDIR)

.PHONY: all clean
