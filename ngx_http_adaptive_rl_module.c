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

static ngx_int_t ngx_http_adaptive_rl_init_process(ngx_cycle_t* cycle);

// -------------------- Module Config --------------------
typedef struct {
    ngx_flag_t enable;
    ngx_uint_t cpu_threshold_x100;
    ngx_uint_t base_rps;
    ngx_uint_t decay_factor_percents;
} ngx_http_adaptive_rl_conf_t;

static ngx_int_t ngx_http_adaptive_rl_handler(ngx_http_request_t* r);
static void* ngx_http_adaptive_rl_create_conf(ngx_conf_t* cf);
static char* ngx_http_adaptive_rl_merge_conf(ngx_conf_t* cf, void* parent, void* child);

// -------------------- Config Directives --------------------
static ngx_command_t ngx_http_adaptive_rl_commands[] = {
    {ngx_string("rate_limit_adaptive"), NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_http_adaptive_rl_conf_t, enable), NULL},

    {ngx_string("rate_limit_cpu_threshold_x100"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1, ngx_conf_set_num_slot,
     NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_http_adaptive_rl_conf_t, cpu_threshold_x100), NULL},

    {ngx_string("rate_limit_base"), NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_http_adaptive_rl_conf_t, base_rps), NULL},

    {ngx_string("rate_limit_decay_percents"), NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_http_adaptive_rl_conf_t, decay_factor_percents), NULL},

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
    conf->cpu_threshold_x100 = NGX_CONF_UNSET_UINT;
    conf->base_rps = NGX_CONF_UNSET_UINT;
    conf->decay_factor_percents = NGX_CONF_UNSET_UINT;

    return conf;
}

static char* ngx_http_adaptive_rl_merge_conf(ngx_conf_t* cf, void* parent, void* child) {
    ngx_http_adaptive_rl_conf_t* prev = parent;
    ngx_http_adaptive_rl_conf_t* conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    ngx_conf_merge_uint_value(conf->cpu_threshold_x100, prev->cpu_threshold_x100, 150);
    ngx_conf_merge_uint_value(conf->base_rps, prev->base_rps, 10000);
    ngx_conf_merge_uint_value(conf->decay_factor_percents, prev->decay_factor_percents, 80);

    return NGX_CONF_OK;
}

// -------------------- Shared memory --------------------
static ngx_shm_zone_t* shm_zone = NULL;

typedef struct {
    ngx_atomic_t rps;
} ngx_http_adaptive_rl_shctx_t;

static ngx_int_t ngx_http_adaptive_rl_init_shm_zone(ngx_shm_zone_t* shm_zone, void* data) {
    ngx_http_adaptive_rl_shctx_t* shctx;
    ngx_slab_pool_t* shpool;

    if (data) {
        shm_zone->data = data;

        return NGX_OK;
    }

    shpool = (ngx_slab_pool_t*)shm_zone->shm.addr;

    shctx = (ngx_http_adaptive_rl_shctx_t*)ngx_slab_alloc(shpool, sizeof(ngx_http_adaptive_rl_shctx_t));
    if (shctx == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(shctx, sizeof(ngx_http_adaptive_rl_shctx_t));
    shm_zone->data = shctx;

    return NGX_OK;
}

static ngx_int_t ngx_http_adaptive_rl_init_shm(ngx_conf_t* cf) {
    ngx_str_t shm_name = ngx_string("ngx_http_adaptive_rl");
    size_t shm_size = ngx_align(8 * ngx_pagesize, ngx_pagesize);

    shm_zone = ngx_shared_memory_add(cf, &shm_name, shm_size, &ngx_http_adaptive_rl_module);
    if (shm_zone == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "init shm: shm_zone is NULL");

        return NGX_ERROR;
    }

    shm_zone->init = ngx_http_adaptive_rl_init_shm_zone;
    shm_zone->data = NULL;

    return NGX_OK;
}

// ----------------------- RPS -----------------------------
static ngx_event_t* rps_reset_ev = NULL;

static void ngx_http_adaptive_rl_reset_rps(ngx_event_t* ev) {
    ngx_http_adaptive_rl_shctx_t* shctx;

    if (shm_zone == NULL || shm_zone->data == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "reset timer: shm_zone is NULL");

        return;
    }

    shctx = (ngx_http_adaptive_rl_shctx_t*)shm_zone->data;

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

static ngx_int_t ngx_http_adaptive_rl_init_process(ngx_cycle_t* cycle) {
    rps_reset_ev = ngx_pcalloc(cycle->pool, sizeof(ngx_event_t));
    if (rps_reset_ev == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "rps_reset_ev == NULL");

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
static ngx_int_t ngx_http_adaptive_rl_handler(ngx_http_request_t *r) {
    ngx_log_error(NGX_LOG_DEBUG, ngx_cycle->log, 0, "ngx_http_adaptive_rl_handler triggered");

    ngx_http_adaptive_rl_conf_t* conf;
    conf = ngx_http_get_module_loc_conf(r, ngx_http_adaptive_rl_module);

    if (!conf->enable) {
        return NGX_DECLINED;
    }

    ngx_http_adaptive_rl_shctx_t* shctx = shm_zone->data;
    ngx_atomic_t current_rps = ngx_atomic_fetch_add(&shctx->rps, 1);
    if (current_rps >= conf->base_rps) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "ngx_adaptive_rl: rejecting request due to high RPS (current: %ui, limit (base): %ui)",
                      current_rps, conf->base_rps);

        return NGX_HTTP_SERVICE_UNAVAILABLE;
    }

    double load[1];
    if (getloadavg(load, 1) != 1) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    double factor = 1.0;
    double cpu_threshold = (double) conf->cpu_threshold_x100 / 100.0;
    if (load[0] >= cpu_threshold) {
        factor *= (double)conf->decay_factor_percents/100.0;
    }

    if (shctx == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_adaptive_rl: shared memory is not initialized");

        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_uint_t max_rps = (ngx_uint_t)(conf->base_rps * factor);
    if (current_rps >= max_rps) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "ngx_adaptive_rl: rejecting request due to high RPS (current: %ui, limit (decreased): %ui)",
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
    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "Registering ngx_http_adaptive_rl_handler");

    if (ngx_http_adaptive_rl_init_shm(cf) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "Failed to init ngx_http_adaptive_rl_init_shm");

        return NGX_ERROR;
    }

    ngx_http_handler_pt* h;
    ngx_http_core_main_conf_t* cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers); // TODO: NGX_HTTP_ACCESS_PHASE???
    if (h == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "Failed to register ngx_http_adaptive_rl_handler");

        return NGX_ERROR;
    }

    *h = ngx_http_adaptive_rl_handler;
    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "Handler registered successfully");

    return NGX_OK;
}
