ROOTDIR := $(abspath $(CURDIR)/../..)
BINDIR := $(ROOTDIR)/build-x86/tests/virtio

include $(ROOTDIR)/Makefile.common

SRCS := $(sort $(wildcard unit_*.c))
OBJS := $(patsubst %.c, $(BINDIR)/%.o, $(SRCS))
TESTS := $(patsubst %.c, $(BINDIR)/%, $(SRCS))

all: $(TESTS)
	for t in $(TESTS); do $$t || exit 1; done

$(TESTS): $(BINDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

$(BINDIR)/%.o: %.c
	$(CC) $(CFLAGS) -I$(ROOTDIR)/include -DVHOST_TEST_SUITE_NAME=\"$(basename $<)\" -c $< -o $@

$(BINDIR)/%: $(BINDIR)/%.o
	$(CC) $(LDFLAGS) $< -lcunit -lvirtqueue -L$(ROOTDIR)/build-x86/virtio -o $@

clean:
	rm -rf $(BINDIR)

.PHONY: all clean
