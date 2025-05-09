NGINX_SRC ?= /opt/nginx-src
NGINX_BUILD := build
CC := gcc

MODULE_NAME := ngx_http_adaptive_rl_module
MODULE_SRC := ngx_http_adaptive_rl_module.c

build-module:
	$(CC) -fPIC -o $(MODULE_NAME).so -shared $(MODULE_SRC) \
	-I$(NGINX_SRC)/src/core \
	-I$(NGINX_SRC)/src/event \
	-I$(NGINX_SRC)/src/http \
	-I$(NGINX_SRC)/objs \
	-I$(NGINX_SRC)/src/http/modules \
	-I$(NGINX_SRC)/src/os/unix \
	-I/opt/homebrew/include

format:
	find . -type f \( -name "*.h" -o -name "*.c" \) -exec clang-format -i {} +

run:
	docker build -t ngx_adaptive_rl .
	docker run --rm -p 8090:80 ngx_adaptive_rl
