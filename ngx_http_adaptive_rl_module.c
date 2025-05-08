/*
 * ngx_http_adaptive_rl_module.c
 *
 * NGINX module for adaptive rate limiting based on system CPU load
 * and upstream response latency.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <unistd.h>

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
static ngx_http_module_t ngx_http_adaptive_rl_module_ctx = {
    NULL, // preconfiguration
    NULL, // postconfiguration

    NULL, // create main conf
    NULL, // init main conf

    NULL, // create server conf
    NULL, // merge server conf

    ngx_http_adaptive_rl_create_conf, // create location conf
    ngx_http_adaptive_rl_merge_conf   // merge location conf
};

ngx_module_t ngx_http_adaptive_rl_module = {NGX_MODULE_V1,
                                            &ngx_http_adaptive_rl_module_ctx, // module context
                                            ngx_http_adaptive_rl_commands,    // module directives
                                            NGX_HTTP_MODULE,                  // module type
                                            NULL,
                                            NULL,
                                            NULL,
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

// -------------------- Request Handler --------------------
static ngx_int_t ngx_http_adaptive_rl_handler(ngx_http_request_t* r) {
    ngx_http_adaptive_rl_conf_t* conf;
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

    // TODO: добавить upstream latency measurement & adjust factor
    // TODO: хранить кол-во запросов в секунду (shared memory), применять factor

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "adaptive_rl: loadavg=%.2f factor=%.2f", load[0], factor);

    return NGX_DECLINED;
}

// -------------------- Register Handler --------------------
static ngx_int_t ngx_http_adaptive_rl_init(ngx_conf_t* cf) {
    ngx_http_handler_pt* h;
    ngx_http_core_main_conf_t* cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_adaptive_rl_handler;
    return NGX_OK;
}

static ngx_http_module_t ngx_http_adaptive_rl_module_ctx = {NULL,
                                                            ngx_http_adaptive_rl_init, // postconfiguration

                                                            NULL,
                                                            NULL,
                                                            NULL,
                                                            NULL,
                                                            ngx_http_adaptive_rl_create_conf,
                                                            ngx_http_adaptive_rl_merge_conf};
