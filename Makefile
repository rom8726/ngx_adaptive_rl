NGINX_SRC ?= /opt/nginx-src
NGINX_BUILD := build
CC := gcc

MODULE_NAME := ngx_http_adaptive_rl_module
MODULE_SRC := ngx_http_adaptive_rl_module.c

NGINX_CONFIGURE_ARGS := \
    --add-module=$(shell pwd) \
    --with-cc-opt="-O2 -g" \
    --with-http_ssl_module \
    --with-http_v2_module

all: build-nginx

clean:
	rm -rf $(NGINX_BUILD)

build-module:
	$(CC) -fPIC -o $(MODULE_NAME).so -shared $(MODULE_SRC) \
	-I$(NGINX_SRC)/src/core \
	-I$(NGINX_SRC)/src/event \
	-I$(NGINX_SRC)/src/http \
	-I$(NGINX_SRC)/objs \
	-I$(NGINX_SRC)/src/http/modules \
	-I$(NGINX_SRC)/src/os/unix \
	-I/opt/homebrew/include

rebuild:
	make clean
	make all

format:
	find . -type f \( -name "*.h" -o -name "*.c" \) -exec clang-format -i {} +
