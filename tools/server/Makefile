include ../../Makefile.common

BINDIR := $(abspath ../../build-x86/tools/server)
TARGET := $(BINDIR)/vhost-server
OBJS := $(patsubst %.c, $(BINDIR)/%.o, $(wildcard *.c))

all: $(TARGET)

$(BINDIR):
	mkdir -p $@

$(BINDIR)/%.o: %.c
	$(CC) $(CFLAGS) -I../../include -c $< -o $@

$(TARGET): $(BINDIR) $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -Wl,-rpath=$(BINDIR)/../../ -L$(BINDIR)/../../ -lvhost -o $@

clean:
	rm -rf $(TARGET) $(OBJS)
