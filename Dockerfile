FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    wget \
    gcc \
    make \
    libssl-dev \
    zlib1g-dev \
    libpcre3-dev \
    clang-format \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/nginx-module-src

COPY ngx_http_adaptive_rl_module.c .
COPY config .

ARG NGINX_VERSION=1.27.5
WORKDIR /opt/nginx-src
RUN wget http://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz \
    && tar -xzf nginx-${NGINX_VERSION}.tar.gz \
    && mv nginx-${NGINX_VERSION}/* . \
    && rmdir nginx-${NGINX_VERSION}

RUN ./configure --add-module=/opt/nginx-module-src \
    --with-http_ssl_module \
    --with-http_realip_module \
    --with-http_addition_module \
    --with-http_sub_module \
    --with-http_dav_module \
    --with-http_flv_module \
    --with-http_mp4_module \
    --with-http_gunzip_module \
    --with-http_gzip_static_module \
    --with-http_random_index_module \
    --with-http_secure_link_module \
    --with-http_slice_module \
    --with-http_stub_status_module \
    --with-threads \
    --with-stream \
    --with-stream_ssl_module \
    --with-stream_ssl_preread_module \
    --with-http_v2_module

RUN make && make install

COPY nginx.conf /usr/local/nginx/conf/nginx.conf

RUN mkdir -p /usr/local/nginx/logs

EXPOSE 80

CMD ["/usr/local/nginx/sbin/nginx", "-g", "daemon off;"]
