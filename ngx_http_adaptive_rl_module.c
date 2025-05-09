/*
 * ngx_http_adaptive_rl_module.c
 *
 * NGINX module for adaptive rate limiting based on system CPU load
 * and upstream response latency.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_event.h>
#include <unistd.h>

static ngx_int_t ngx_http_adaptive_rl_init_process(ngx_cycle_t *cycle);

// -------------------- Module Config --------------------
typedef struct {
    ngx_flag_t enable;
    double cpu_threshold;
    ngx_msec_t latency_threshold;
    ngx_uint_t base_rps;
    double decay_factor;
} ngx_http_adaptive_rl_conf_t;

static ngx_int_t ngx_http_adaptive_rl_handler(ngx_http_request_t* r);
static void* ngx_http_adaptive_rl_create_conf(ngx_conf_t* cf);
static char* ngx_http_adaptive_rl_merge_conf(ngx_conf_t* cf, void* parent, void* child);

// -------------------- Config Directives --------------------
static ngx_command_t ngx_http_adaptive_rl_commands[] = {
    {ngx_string("rate_limit_adaptive"), NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_http_adaptive_rl_conf_t, enable), NULL},

    {ngx_string("rate_limit_cpu_threshold"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1, ngx_conf_set_num_slot,
     NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_http_adaptive_rl_conf_t, cpu_threshold), NULL},

    {ngx_string("rate_limit_latency_threshold"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1, ngx_conf_set_msec_slot,
     NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_http_adaptive_rl_conf_t, latency_threshold), NULL},

    {ngx_string("rate_limit_base"), NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_http_adaptive_rl_conf_t, base_rps), NULL},

    {ngx_string("rate_limit_decay"), NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_http_adaptive_rl_conf_t, decay_factor), NULL},

    ngx_null_command};

// -------------------- Context and Module --------------------
static ngx_int_t ngx_http_adaptive_rl_init(ngx_conf_t* cf);
static ngx_http_module_t ngx_http_adaptive_rl_module_ctx = {NULL,
                                                            ngx_http_adaptive_rl_init, // postconfiguration
                                                            NULL,
                                                            NULL,
                                                            NULL,
                                                            NULL,
                                                            ngx_http_adaptive_rl_create_conf,
                                                            ngx_http_adaptive_rl_merge_conf};

ngx_module_t ngx_http_adaptive_rl_module = {NGX_MODULE_V1,
                                            &ngx_http_adaptive_rl_module_ctx, // module context
                                            ngx_http_adaptive_rl_commands,    // module directives
                                            NGX_HTTP_MODULE,                  // module type
                                            NULL,
                                            NULL,
                                            ngx_http_adaptive_rl_init_process,
                                            NULL,
                                            NULL,
                                            NULL,
                                            NULL,
                                            NGX_MODULE_V1_PADDING};

// -------------------- Config Functions --------------------
static void* ngx_http_adaptive_rl_create_conf(ngx_conf_t* cf) {
    ngx_http_adaptive_rl_conf_t* conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_adaptive_rl_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->enable = NGX_CONF_UNSET;
    conf->cpu_threshold = NGX_CONF_UNSET;
    conf->latency_threshold = NGX_CONF_UNSET_MSEC;
    conf->base_rps = NGX_CONF_UNSET_UINT;
    conf->decay_factor = NGX_CONF_UNSET;

    return conf;
}

static char* ngx_http_adaptive_rl_merge_conf(ngx_conf_t* cf, void* parent, void* child) {
    ngx_http_adaptive_rl_conf_t* prev = parent;
    ngx_http_adaptive_rl_conf_t* conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    ngx_conf_merge_value(conf->cpu_threshold, prev->cpu_threshold, 1.5);
    ngx_conf_merge_msec_value(conf->latency_threshold, prev->latency_threshold, 300);
    ngx_conf_merge_uint_value(conf->base_rps, prev->base_rps, 100);
    ngx_conf_merge_value(conf->decay_factor, prev->decay_factor, 0.5);

    return NGX_CONF_OK;
}

// -------------------- Shared memory --------------------
static ngx_shm_zone_t *shm_zone = NULL;

typedef struct {
    ngx_atomic_t rps;
} ngx_http_adaptive_rl_shctx_t;

static ngx_int_t ngx_http_adaptive_rl_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data) {
    ngx_http_adaptive_rl_shctx_t *shctx;
    ngx_slab_pool_t *shpool;

    if (data) {
        shm_zone->data = data;

        return NGX_OK;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    shctx = (ngx_http_adaptive_rl_shctx_t *) ngx_slab_alloc(shpool, sizeof(ngx_http_adaptive_rl_shctx_t));
    if (shctx == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(shctx, sizeof(ngx_http_adaptive_rl_shctx_t));
    shm_zone->data = shctx;

    return NGX_OK;
}

static ngx_int_t ngx_http_adaptive_rl_init_shm(ngx_conf_t *cf) {
    ngx_str_t shm_name = ngx_string("ngx_http_adaptive_rl");
    size_t shm_size = ngx_align(8 * ngx_pagesize, ngx_pagesize);

    shm_zone = ngx_shared_memory_add(cf, &shm_name, shm_size, &ngx_http_adaptive_rl_module);
    if (shm_zone == NULL) {
        return NGX_ERROR;
    }

    shm_zone->init = ngx_http_adaptive_rl_init_shm_zone;
    shm_zone->data = NULL;

    return NGX_OK;
}

// ----------------------- RPS -----------------------------
static ngx_event_t *rps_reset_ev = NULL;

static void ngx_http_adaptive_rl_reset_rps(ngx_event_t *ev) {
    ngx_http_adaptive_rl_shctx_t *shctx;

    if (shm_zone == NULL || shm_zone->data == NULL) {
        return;
    }

    shctx = (ngx_http_adaptive_rl_shctx_t *)shm_zone->data;

    ngx_atomic_t old_value = ngx_atomic_fetch_add(&shctx->rps, 0);
    while (1) {
        if (ngx_atomic_cmp_set(&shctx->rps, old_value, 0)) {
            break;
        }

        old_value = ngx_atomic_fetch_add(&shctx->rps, 0);
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ev->log, 0, "adaptive_rl: RPS reset to 0 (previous value: %ui)", old_value);

    ngx_add_timer(ev, 1000);
}

static ngx_int_t ngx_http_adaptive_rl_init_process(ngx_cycle_t *cycle) {
    rps_reset_ev = ngx_pcalloc(cycle->pool, sizeof(ngx_event_t));
    if (rps_reset_ev == NULL) {
        return NGX_ERROR;
    }

    rps_reset_ev->handler = ngx_http_adaptive_rl_reset_rps;
    rps_reset_ev->log = cycle->log;
    rps_reset_ev->data = NULL;
    rps_reset_ev->timedout = 1;

    ngx_add_timer(rps_reset_ev, 1000);

    return NGX_OK;
}

// -------------------- Request Handler --------------------
// TODO: добавить upstream latency measurement & adjust factor
static ngx_int_t ngx_http_adaptive_rl_handler(ngx_http_request_t *r) {
    ngx_http_adaptive_rl_conf_t *conf;
    conf = ngx_http_get_module_loc_conf(r, ngx_http_adaptive_rl_module);

    if (!conf->enable) {
        return NGX_DECLINED;
    }

    double load[1];
    if (getloadavg(load, 1) != 1) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    double factor = 1.0;
    if (load[0] >= conf->cpu_threshold) {
        factor *= conf->decay_factor;
    }

    ngx_http_adaptive_rl_shctx_t *shctx;
    shctx = (ngx_http_adaptive_rl_shctx_t *)shm_zone->data;

    if (shctx == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_adaptive_rl: shared memory is not initialized");

        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_atomic_t current_rps = ngx_atomic_fetch_add(&shctx->rps, 1);
    ngx_uint_t max_rps = (ngx_uint_t)(conf->base_rps * factor);

    if (current_rps >= max_rps) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "ngx_adaptive_rl: rejecting request due to high RPS (current: %ui, limit: %ui)",
                      current_rps, max_rps);

        return NGX_HTTP_SERVICE_UNAVAILABLE;
    }

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                  "ngx_adaptive_rl: allowing request (RPS: %ui, max RPS: %ui, load: %.2f, factor: %.2f)",
                  current_rps, max_rps, load[0], factor);

    return NGX_DECLINED;
}

// -------------------- Register Handler --------------------
static ngx_int_t ngx_http_adaptive_rl_init(ngx_conf_t* cf) {
    ngx_http_handler_pt* h;
    ngx_http_core_main_conf_t* cmcf;

    if (ngx_http_adaptive_rl_init_shm(cf) != NGX_OK) {
        return NGX_ERROR;
    }

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_adaptive_rl_handler;

    return NGX_OK;
}
