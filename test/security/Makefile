# =============================================================================
#  Makefile — Security fixture corpus (grouped by vulnerability type)
# =============================================================================

CC ?= gcc
CLANG ?= clang
CFLAGS ?= -Wall -Wextra -Wpedantic -g -std=c11
ASANFLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer

SRCS := $(shell find . -type f -name '*.c' | sort)
BINS := $(patsubst %.c,build/%,$(SRCS))
ASAN_BINS := $(patsubst %.c,build/%_asan,$(SRCS))

.PHONY: all analyze scan-build asan clean

all: $(BINS)

build/%: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

analyze:
	@echo "========================================="
	@echo " Clang Static Analyzer (security fixtures)"
	@echo "========================================="
	@for f in $(SRCS); do \
		echo "\n--- Analyzing $$f ---"; \
		$(CLANG) --analyze -Xanalyzer -analyzer-checker=core,unix,deadcode,security \
		         -Xanalyzer -analyzer-output=text $$f 2>&1 || true; \
	done

scan-build:
	@echo "========================================="
	@echo " scan-build (security fixtures)"
	@echo "========================================="
	@for f in $(SRCS); do \
		echo "\n--- scan-build $$f ---"; \
		scan-build $(CC) $(CFLAGS) -c $$f -o /dev/null 2>&1 || true; \
	done

asan: $(ASAN_BINS)

build/%_asan: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ASANFLAGS) -o $@ $<

clean:
	rm -rf build
	rm -f ./*.plist
