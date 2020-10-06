include Makefile.common

BINDIR := build-x86

HDRS := $(wildcard include/*/*.h)
SRCS := $(wildcard *.c)
OBJS := $(patsubst %.c,$(BINDIR)/%.o,$(SRCS))

TARGET := $(BINDIR)/libvhost.so

all: $(TARGET)

$(BINDIR):
	mkdir -p $@

$(BINDIR)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(BINDIR) $(HDRS) $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -shared -o $@

clean:
	rm -rf $(BINDIR)
