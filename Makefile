KDIR=/lib/modules/$(shell uname -r)/build

CFLAGS_user = -std=gnu11 -Wall -Wextra -Werror
LDFLAGS_user = -lpthread
TARGET_MODULE := khttpd
obj-m += khttpd.o
khttpd-objs := \
	http_parser.o \
	http_server.o \
        bignum.o \
	main.o

GIT_HOOKS := .git/hooks/applied
all: $(GIT_HOOKS) bignum.c http_parser.c htstress
	make -C $(KDIR) M=$(PWD) modules

$(GIT_HOOKS):
	@scripts/install-git-hooks
	@echo

htstress: htstress.c
	$(CC) $(CFLAGS_user) -o $@ $< $(LDFLAGS_user)

check: all
	@scripts/test.sh

clean:
	make -C $(KDIR) M=$(PWD) clean
	$(RM) htstress

load:
	sudo insmod $(TARGET_MODULE).ko port=1999

unload:
	sudo rmmod $(TARGET_MODULE) || true >/dev/null

relaod:
	$(MAKE) unload
	sudo dmesg -C
	$(MAKE) load

verify:
	$(MAKE) unload
	$(MAKE) load
	@./fibcheck.sh 1/10
	@./fibcheck.sh 100/10
	@./fibcheck.sh 1000/10
	@./fibcheck.sh 10000/10

request:
	wget localhost:1999/fib/$(k)
	dmesg

# Download http_parser.[ch] from nodejs/http-parser repository
# the inclusion of standard header files such as <string.h> will be replaced
# with "compat/string.h", which is just a wrapper to Linux kernel headers.
# TODO: rewrite with elegant scripts
http_parser.c:
	wget -q https://raw.githubusercontent.com/nodejs/http-parser/master/http_parser.c
	@sed -i 's/#include <assert.h>/#include "compat\/assert.h"/' $@
	@sed -i 's/#include <stddef.h>/#include "compat\/stddef.h"/' $@
	@sed -i 's/#include <ctype.h>/#include "compat\/ctype.h"/' $@
	@sed -i 's/#include <string.h>/#include "compat\/string.h"/' $@
	@sed -i 's/#include <limits.h>/#include "compat\/limits.h"/' $@
	@echo "File $@ was patched."
	wget -q https://raw.githubusercontent.com/nodejs/http-parser/master/http_parser.h
	@sed -i 's/#include <stddef.h>/#include "compat\/stddef.h"/' http_parser.h
	@sed -i 's/#include <stdint.h>/#include "compat\/stdint.h"/' http_parser.h
	@echo "File http_parser.h was patched."

distclean: clean
	$(RM) http_parser.c http_parser.h
