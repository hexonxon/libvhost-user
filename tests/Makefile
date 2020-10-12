SUBFOLDERS := $(dir $(wildcard ./*/.))

all:
	for f in $(SUBFOLDERS); do \
		$(MAKE) -C $$f; \
	done

.PHONY: all
