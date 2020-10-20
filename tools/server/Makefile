ROOT_DIR := $(abspath $(CURDIR)/../..)

include $(ROOT_DIR)/Makefile.common

BINDIR := $(ROOT_DIR)/build-x86/tools/server
TARGET := $(BINDIR)/vhost-server
OBJS := $(patsubst %.c, $(BINDIR)/%.o, $(wildcard *.c))
LDFLAGS += -Wl,-rpath=$(ROOT_DIR)/build-x86 -L$(ROOT_DIR)/build-x86 -L$(ROOT_DIR)/build-x86/virtio

all: $(TARGET)

$(BINDIR):
	mkdir -p $@

$(BINDIR)/%.o: %.c
	$(CC) $(CFLAGS) -I$(ROOT_DIR)/include -c $< -o $@

$(TARGET): $(BINDIR) $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -lvhost -lvirtqueue -o $@

clean:
	rm -rf $(TARGET) $(OBJS)
