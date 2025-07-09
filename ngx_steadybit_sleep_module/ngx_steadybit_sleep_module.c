/*
 * Copyright 2025 steadybit GmbH. All rights reserved.
 */

/**
 * NGINX Sleep Module for Steadybit
 *
 * This module provides the ability to introduce configurable delays (sleep)
 * in HTTP request processing for testing and chaos engineering purposes.
 *
 * The module implements asynchronous sleeping using nginx's event system,
 * ensuring that worker processes don't block while waiting.
 */

 #include <ngx_config.h>
 #include <ngx_core.h>
 #include <ngx_http.h>
 #include <ngx_http_core_module.h>

 /**
  * Location Configuration Structure
  *
  * Stores the sleep duration configuration for each location block.
  * The sleep_ms field can contain complex values (variables, expressions).
  */
 typedef struct {
     ngx_http_complex_value_t  *sleep_ms;  /* Sleep duration in milliseconds */
 } ngx_http_sleep_loc_conf_t;

 /**
  * Request Context Structure for Sleep Events
  *
  * This structure maintains the state for each request that is currently sleeping.
  * It's stored in the request context and contains the event timer and request reference.
  */
 typedef struct {
     ngx_event_t  sleep_event;    /* Timer event for waking up after sleep */
     ngx_http_request_t *request; /* Reference to the HTTP request */
     ngx_flag_t  waiting;         /* Flag indicating if request is currently waiting */
 } ngx_http_sleep_ctx_t;

 /* Function prototypes - these functions implement the module's core functionality */
 static ngx_int_t ngx_http_sleep_init(ngx_conf_t *cf);
 static void *ngx_http_sleep_create_loc_conf(ngx_conf_t *cf);
 static char *ngx_http_sleep_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
 static char *ngx_http_sleep_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
 static ngx_int_t ngx_http_sleep_handler(ngx_http_request_t *r);
 static void ngx_http_sleep_wake_handler(ngx_event_t *ev);
 static void ngx_http_sleep_cleanup_handler(void *data);

 /**
  * Module Commands Configuration
  *
  * Defines the "sb_sleep_ms" directive that can be used in nginx configuration.
  * This directive accepts one parameter (the sleep duration in milliseconds).
  */
 static ngx_command_t ngx_http_sleep_commands[] = {
     { ngx_string("sb_sleep_ms"),                           /* Directive name */
       NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1, /* Context and argument count */
       ngx_http_sleep_set,                                  /* Handler function */
       NGX_HTTP_LOC_CONF_OFFSET,                           /* Configuration level */
       offsetof(ngx_http_sleep_loc_conf_t, sleep_ms),      /* Field offset in config struct */
       NULL },                                              /* Post-processing function */
     ngx_null_command                                       /* Terminator */
 };

 /**
  * HTTP Module Context
  *
  * Defines the module's lifecycle hooks and configuration management functions.
  */
 static ngx_http_module_t ngx_steadybit_sleep_module_ctx = {
     NULL,                          /* preconfiguration - called before config parsing */
     ngx_http_sleep_init,           /* postconfiguration - called after config parsing */
     NULL,                          /* create main configuration */
     NULL,                          /* init main configuration */
     NULL,                          /* create server configuration */
     NULL,                          /* merge server configuration */
     ngx_http_sleep_create_loc_conf,/* create location configuration */
     ngx_http_sleep_merge_loc_conf  /* merge location configuration */
 };

 /**
  * Main Module Definition
  *
  * This is the primary module structure that nginx uses to identify and load the module.
  */
 ngx_module_t ngx_steadybit_sleep_module = {
     NGX_MODULE_V1,
     &ngx_steadybit_sleep_module_ctx,    /* module context */
     ngx_http_sleep_commands,       /* module directives */
     NGX_HTTP_MODULE,               /* module type */
     NULL,                          /* init master */
     NULL,                          /* init module */
     NULL,                          /* init process */
     NULL,                          /* init thread */
     NULL,                          /* exit thread */
     NULL,                          /* exit process */
     NULL,                          /* exit master */
     NGX_MODULE_V1_PADDING
 };

 /**
  * Create Location Configuration
  *
  * Called when nginx encounters a location block that might use this module.
  * Allocates and initializes the configuration structure for this location.
  */
 static void *
 ngx_http_sleep_create_loc_conf(ngx_conf_t *cf)
 {
     ngx_http_sleep_loc_conf_t  *conf;

     /* Allocate memory for the configuration structure */
     conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_sleep_loc_conf_t));
     if (conf == NULL) {
         return NULL;
     }

     /* Initialize sleep_ms to NULL (no sleep configured by default) */
     conf->sleep_ms = NULL;

     return conf;
 }

 /**
  * Merge Location Configurations
  *
  * Called when nginx needs to merge configurations from parent and child contexts.
  * Child configurations inherit from parent if not explicitly set.
  */
 static char *
 ngx_http_sleep_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
 {
     ngx_http_sleep_loc_conf_t *prev = parent;  /* Parent configuration */
     ngx_http_sleep_loc_conf_t *conf = child;   /* Child configuration */

     /* If child doesn't have sleep_ms configured, inherit from parent */
     if (conf->sleep_ms == NULL) {
         conf->sleep_ms = prev->sleep_ms;
     }

     return NGX_CONF_OK;
 }

 /**
  * Parse sb_sleep_ms Directive
  *
  * Called when nginx encounters the "sb_sleep_ms" directive in configuration.
  * Parses the sleep duration value and stores it as a complex value.
  */
 static char *
 ngx_http_sleep_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
 {
     ngx_http_sleep_loc_conf_t *slcf = conf;
     ngx_str_t *value;
     ngx_http_compile_complex_value_t ccv;

     /* Check if directive is already configured (prevent duplicates) */
     if (slcf->sleep_ms != NULL) {
         return "is duplicate";
     }

     /* Get the directive arguments */
     value = cf->args->elts;

     /* Allocate memory for the complex value structure */
     slcf->sleep_ms = ngx_palloc(cf->pool, sizeof(ngx_http_complex_value_t));
     if (slcf->sleep_ms == NULL) {
         return NGX_CONF_ERROR;
     }

     /* Initialize complex value compilation context */
     ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

     ccv.cf = cf;
     ccv.value = &value[1];           /* First argument (value[0] is directive name) */
     ccv.complex_value = slcf->sleep_ms;

     /* Compile the complex value (handles variables, expressions, etc.) */
     if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
         return NGX_CONF_ERROR;
     }

     return NGX_CONF_OK;
 }

 /**
  * Module Initialization
  *
  * Called during nginx configuration phase to register the module's handler
  * in the HTTP request processing pipeline.
  */
 static ngx_int_t
 ngx_http_sleep_init(ngx_conf_t *cf)
 {
     ngx_http_handler_pt        *h;
     ngx_http_core_main_conf_t  *cmcf;

     /* Get the main HTTP configuration */
     cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

     /* Register our handler in the ACCESS phase (before content generation) */
     h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
     if (h == NULL) {
         return NGX_ERROR;
     }

     *h = ngx_http_sleep_handler;

     return NGX_OK;
 }

 /**
  * Main Request Handler
  *
  * This function is called for each HTTP request during the ACCESS phase.
  * It checks if sleep is configured, evaluates the sleep duration, and
  * initiates asynchronous sleeping if needed.
  */
 static ngx_int_t
 ngx_http_sleep_handler(ngx_http_request_t *r)
 {
     ngx_http_sleep_loc_conf_t  *slcf;
     ngx_str_t                   val;
     ngx_int_t                   sleep_time;
     ngx_http_sleep_ctx_t       *ctx;

     /* Get the location configuration for this request */
     slcf = ngx_http_get_module_loc_conf(r, ngx_steadybit_sleep_module);

     /* If no sleep is configured for this location, continue normally */
     if (slcf->sleep_ms == NULL) {
         return NGX_DECLINED;
     }

    /* Check if we already have a context (prevent re-processing) */
    ctx = ngx_http_get_module_ctx(r, ngx_steadybit_sleep_module);
    if (ctx != NULL) {
        return NGX_DECLINED;
    }

    /* Evaluate the complex value to get the actual sleep duration */
    if (ngx_http_complex_value(r, slcf->sleep_ms, &val) != NGX_OK) {
        return NGX_ERROR;
    }

    /* If the evaluated value is empty, no sleep needed */
    if (val.len == 0) {
        return NGX_DECLINED;
    }

    /* Convert the string value to integer (milliseconds) */
    sleep_time = ngx_atoi(val.data, val.len);
    if (sleep_time == NGX_ERROR || sleep_time < 0) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "invalid sb_sleep_ms value \"%V\"", &val);
        return NGX_DECLINED;
    }

    /* If sleep time is 0, no need to sleep */
    if (sleep_time == 0) {
        return NGX_DECLINED;
    }

    /* Create and initialize request context for this sleep operation */
    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_sleep_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }
    ctx->request = r;
    ngx_http_set_ctx(r, ctx, ngx_steadybit_sleep_module);

    /* Register cleanup handler to prevent memory leaks if request terminates early */
    ngx_pool_cleanup_t *cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_ERROR;
    }
    cln->handler = ngx_http_sleep_cleanup_handler;
    cln->data = ctx;

    ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                  "sleeping (async) for %i ms", sleep_time);

    /* Initialize the timer event for asynchronous sleeping */
    ngx_memzero(&ctx->sleep_event, sizeof(ngx_event_t));
    ctx->sleep_event.handler = ngx_http_sleep_wake_handler; /* Callback when timer expires */
    ctx->sleep_event.data = ctx;                            /* Pass context to callback */
    ctx->sleep_event.log = r->connection->log;              /* Use request's log context */

    /* Start the timer - this is non-blocking */
    ngx_add_timer(&ctx->sleep_event, (ngx_msec_t)sleep_time);

    /* Increment request reference count to prevent cleanup during sleep */
    r->main->count++;

    /* Return NGX_DONE to pause request processing until timer expires */
    return NGX_DONE;
 }

 /**
  * Timer Wake-up Handler
  *
  * This function is called when the sleep timer expires. It resumes
  * request processing and cleans up the sleep context.
  */
 static void ngx_http_sleep_wake_handler(ngx_event_t *ev)
 {
     ngx_http_sleep_ctx_t *ctx = ev->data;      /* Get context from event data */
     ngx_http_request_t *r = ctx->request;      /* Get request from context */

     ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                   "finished sleeping (async)");

     /* Clean up the timer event to prevent memory issues */
     if (ctx->sleep_event.timer_set) {
         ngx_del_timer(&ctx->sleep_event);
     }

     /* Resume normal HTTP request processing from where we left off */
     ngx_http_core_run_phases(r);

     /* Decrement reference count (matches increment in sleep_handler) */
     r->main->count--;
 }

 /**
  * Cleanup Handler
  *
  * This function is called to clean up the sleep context if the request is
  * terminated before the sleep duration expires. It prevents memory leaks
  * by ensuring that timers are cancelled and reference counts are balanced.
  */
 static void ngx_http_sleep_cleanup_handler(void *data)
 {
     ngx_http_sleep_ctx_t *ctx = data;

     ngx_log_error(NGX_LOG_NOTICE, ctx->request->connection->log, 0,
                   "request terminated, cleaning up sleep context");

     /* If the timer is still set, cancel it */
     if (ctx->sleep_event.timer_set) {
         ngx_del_timer(&ctx->sleep_event);
     }

     /* Decrement the request's reference count */
     ctx->request->main->count--;

     /* Note: Context memory is automatically freed when request pool is destroyed */
 }
