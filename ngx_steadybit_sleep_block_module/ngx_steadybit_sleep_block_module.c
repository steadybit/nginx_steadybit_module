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

 #include <ngx_config.h> // NGINX core configuration header
 #include <ngx_core.h>   // NGINX core definitions
 #include <ngx_http.h>   // NGINX HTTP module definitions
 #include <ngx_http_core_module.h> // NGINX HTTP core module definitions

 /**
  * Location Configuration Structure
  *
  * Stores the sleep duration and block configuration for each context (main/server/location).
  * Both fields can contain complex values (variables, expressions).
  */
 typedef struct {
     ngx_http_complex_value_t  *sleep_ms;  /* Sleep duration in milliseconds (can be a variable/expression) */
     ngx_http_complex_value_t  *block;    /* Block condition (can be a variable/expression) */
     ngx_http_complex_value_t  *block_status; /* Optional status code for block (can be a variable/expression) */
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
     ngx_flag_t  cleaned_up;      /* Flag indicating if context has been cleaned up */
     ngx_int_t   saved_phase_handler; /* Saved phase handler to continue from */
 } ngx_http_sleep_ctx_t;

 /* Function prototypes - these functions implement the module's core functionality */
 static ngx_int_t ngx_http_sleep_init(ngx_conf_t *cf); // Module initialization function
 static void *ngx_http_sleep_create_loc_conf(ngx_conf_t *cf); // Create location config
 static char *ngx_http_sleep_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child); // Merge location config
 static char *ngx_http_sleep_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf); // Parse sb_sleep_ms directive
 static char *ngx_http_block_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf); // Parse sb_block directive
 static ngx_int_t ngx_http_sleep_handler(ngx_http_request_t *r); // Main request handler
 static void ngx_http_sleep_wake_handler(ngx_event_t *ev); // Timer wake-up handler
 static void ngx_http_sleep_cleanup_handler(void *data); // Cleanup handler

 // Handler prototypes for if-block support
static char *ngx_http_sleep_ms_handler(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_block_handler(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

 // Forward declarations for per-request handlers
static ngx_int_t ngx_http_sleep_ms_if_request_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_block_if_request_handler(ngx_http_request_t *r);

 /**
  * Module Commands Configuration
  *
  * Defines the "sb_sleep_ms" and "sb_block" directives that can be used in nginx configuration.
  * Both directives accept one or more parameters and can be used in main, server, or location context.
  */
 static ngx_command_t ngx_http_sleep_commands[] = {
     { ngx_string("sb_sleep_ms"),
       NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_SIF_CONF|NGX_CONF_TAKE1,
       ngx_http_sleep_ms_handler,
       NGX_HTTP_LOC_CONF_OFFSET,
       0,
       NULL },
     { ngx_string("sb_block"),
       NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_SIF_CONF|NGX_CONF_TAKE12, /* 1 or 2 args */
       ngx_http_block_handler,
       NGX_HTTP_LOC_CONF_OFFSET,
       0,
       NULL },
     ngx_null_command
 };

 /**
  * HTTP Module Context
  *
  * Defines the module's lifecycle hooks and configuration management functions.
  */
 static ngx_http_module_t ngx_steadybit_sleep_block_module_ctx = {
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
 ngx_module_t ngx_steadybit_sleep_block_module = {
     NGX_MODULE_V1, // Module version macro
     &ngx_steadybit_sleep_block_module_ctx,    /* module context */
     ngx_http_sleep_commands,       /* module directives */
     NGX_HTTP_MODULE,               /* module type */
     NULL,                          /* init master */
     NULL,                          /* init module */
     NULL,                          /* init process */
     NULL,                          /* init thread */
     NULL,                          /* exit thread */
     NULL,                          /* exit process */
     NULL,                          /* exit master */
     NGX_MODULE_V1_PADDING          // Padding for module structure
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
     ngx_http_sleep_loc_conf_t  *conf; // Pointer to location config struct

     /* Allocate memory for the configuration structure */
     conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_sleep_loc_conf_t)); // Allocate and zero memory
     if (conf == NULL) { // Check allocation success
         return NULL; // Return NULL on failure
     }

     /* Initialize sleep_ms to NULL (no sleep configured by default) */
     conf->sleep_ms = NULL; // No sleep by default
     conf->block = NULL;
     conf->block_status = NULL;

     return conf; // Return the allocated config
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
         conf->sleep_ms = prev->sleep_ms; // Inherit sleep_ms from parent
     }
     if (conf->block == NULL) {
         conf->block = prev->block;
     }
     if (conf->block_status == NULL) {
         conf->block_status = prev->block_status;
     }

     return NGX_CONF_OK; // Return OK
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
     ngx_http_sleep_loc_conf_t *slcf = conf; // Cast conf to our config struct
     ngx_str_t *value; // Pointer to directive arguments
     ngx_http_compile_complex_value_t ccv; // Complex value compilation context

     /* Check if directive is already configured (prevent duplicates) */
     if (slcf->sleep_ms != NULL) {
         return "is duplicate"; // Error if already set
     }

     /* Get the directive arguments */
     value = cf->args->elts; // Get arguments array

     /* Allocate memory for the complex value structure */
     slcf->sleep_ms = ngx_palloc(cf->pool, sizeof(ngx_http_complex_value_t)); // Allocate memory
     if (slcf->sleep_ms == NULL) {
         return NGX_CONF_ERROR; // Error if allocation fails
     }

     /* Initialize complex value compilation context */
     ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t)); // Zero the struct

     ccv.cf = cf; // Set config pointer
     ccv.value = &value[1];           /* First argument (value[0] is directive name) */
     ccv.complex_value = slcf->sleep_ms; // Set output pointer

     /* Compile the complex value (handles variables, expressions, etc.) */
     if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
         return NGX_CONF_ERROR; // Error if compilation fails
     }

     return NGX_CONF_OK; // Success
 }

 /**
  * Parse sb_block Directive
  *
  * Called when nginx encounters the "sb_block" directive in configuration.
  * Parses the block condition and optional status code, stores as complex values.
  */
 static char *
 ngx_http_block_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
 {
     ngx_http_sleep_loc_conf_t *slcf = conf;
     ngx_str_t *value = cf->args->elts;
     ngx_http_compile_complex_value_t ccv;
     // Block condition
     if (slcf->block != NULL) {
         return "is duplicate";
     }
     slcf->block = ngx_palloc(cf->pool, sizeof(ngx_http_complex_value_t));
     if (slcf->block == NULL) {
         return NGX_CONF_ERROR;
     }
     ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
     ccv.cf = cf;
     ccv.value = &value[1];
     ccv.complex_value = slcf->block;
     if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
         return NGX_CONF_ERROR;
     }
     // Optional status code
     if (cf->args->nelts > 2) {
         slcf->block_status = ngx_palloc(cf->pool, sizeof(ngx_http_complex_value_t));
         if (slcf->block_status == NULL) {
             return NGX_CONF_ERROR;
         }
         ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
         ccv.cf = cf;
         ccv.value = &value[2];
         ccv.complex_value = slcf->block_status;
         if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
             return NGX_CONF_ERROR;
         }
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
     ngx_http_handler_pt        *h; // Pointer to handler array element
     ngx_http_core_main_conf_t  *cmcf; // Pointer to main HTTP config

     /* Get the main HTTP configuration */
     cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module); // Get main conf

     /* Register our handler in the REWRITE phase (before rewrite directives) */
     h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers); // Add handler to phase
     if (h == NULL) {
         return NGX_ERROR; // Error if push fails
     }

     *h = ngx_http_sleep_handler; // Set our handler function

     return NGX_OK; // Success
 }

 /**
  * Main Request Handler
  *
  * This function is called for each HTTP request during the REWRITE phase.
  * It checks if block is configured and true, blocks if so. Otherwise, checks if sleep is configured, and sleeps if needed.
  */
 static ngx_int_t
 ngx_http_sleep_handler(ngx_http_request_t *r)
 {
     ngx_http_sleep_loc_conf_t  *slcf;
     ngx_str_t                   val;
     ngx_int_t                   sleep_time;
     ngx_http_sleep_ctx_t       *ctx;
     ngx_str_t                   block_val;
     ngx_str_t                   block_status_val;
     ngx_int_t                   block_status = 503; // Default status
     ngx_flag_t                  should_block = 0;

     slcf = ngx_http_get_module_loc_conf(r, ngx_steadybit_sleep_block_module);

     // Evaluate block condition if set
     if (slcf->block != NULL) {
         if (ngx_http_complex_value(r, slcf->block, &block_val) != NGX_OK) {
             return NGX_ERROR;
         }
         if (block_val.len > 0) {
             ngx_int_t block = ngx_atoi(block_val.data, block_val.len);
             if (block != NGX_ERROR && block != 0) {
                 should_block = 1;
                 // Evaluate status code if provided
                 if (slcf->block_status != NULL) {
                     if (ngx_http_complex_value(r, slcf->block_status, &block_status_val) == NGX_OK && block_status_val.len > 0) {
                         ngx_int_t status = ngx_atoi(block_status_val.data, block_status_val.len);
                         if (status > 0) {
                             block_status = status;
                         }
                     }
                 }
             }
         }
     }

     // If we should block and sleep_ms is also set, perform sleep first, then block
     if (should_block && slcf->sleep_ms != NULL) {
         ctx = ngx_http_get_module_ctx(r, ngx_steadybit_sleep_block_module);
         if (ctx != NULL) {
             // After sleep, block
             ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                           "finished sleeping, now blocking with status %i due to sb_block", block_status);
             r->headers_out.status = block_status;
             return block_status;
         }
         // Evaluate sleep time
         if (ngx_http_complex_value(r, slcf->sleep_ms, &val) != NGX_OK) {
             return NGX_ERROR;
         }
         if (val.len == 0) {
             // No sleep, block immediately
             ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                           "no sleep_ms value, blocking with status %i due to sb_block", block_status);
             r->headers_out.status = block_status;
             return block_status;
         }
         sleep_time = ngx_atoi(val.data, val.len);
         if (sleep_time == NGX_ERROR || sleep_time < 0) {
             ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                           "invalid sb_sleep_ms value \"%V\"", &val);
             r->headers_out.status = block_status;
             return block_status;
         }
         if (sleep_time == 0) {
             // No sleep, block immediately
             ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                           "sleep_ms is 0, blocking with status %i due to sb_block", block_status);
             r->headers_out.status = block_status;
             return block_status;
         }
         // Start async sleep, then block
         ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_sleep_ctx_t));
         if (ctx == NULL) {
             return NGX_ERROR;
         }
         ctx->request = r;
         ctx->cleaned_up = 0;
         ctx->saved_phase_handler = r->phase_handler;
         ngx_http_set_ctx(r, ctx, ngx_steadybit_sleep_block_module);
         ngx_pool_cleanup_t *cln = ngx_pool_cleanup_add(r->pool, 0);
         if (cln == NULL) {
             return NGX_ERROR;
         }
         cln->handler = ngx_http_sleep_cleanup_handler;
         cln->data = ctx;
         ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                       "sleeping (async) for %i ms before blocking", sleep_time);
         ngx_memzero(&ctx->sleep_event, sizeof(ngx_event_t));
         ctx->sleep_event.handler = ngx_http_sleep_wake_handler;
         ctx->sleep_event.data = ctx;
         ctx->sleep_event.log = r->connection->log;
         ngx_add_timer(&ctx->sleep_event, (ngx_msec_t)sleep_time);
         r->main->count++;
         return NGX_DONE;
     }

     // If we should block and no sleep, block immediately
     if (should_block) {
         ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                       "blocking request with status %i due to sb_block", block_status);
         r->headers_out.status = block_status;
         return block_status;
     }

     // Sleep logic (as before)
     if (slcf->sleep_ms == NULL) {
         return NGX_DECLINED;
     }
     ctx = ngx_http_get_module_ctx(r, ngx_steadybit_sleep_block_module);
     if (ctx != NULL) {
         return NGX_DECLINED;
     }
     if (ngx_http_complex_value(r, slcf->sleep_ms, &val) != NGX_OK) {
         return NGX_ERROR;
     }
     if (val.len == 0) {
         return NGX_DECLINED;
     }
     sleep_time = ngx_atoi(val.data, val.len);
     if (sleep_time == NGX_ERROR || sleep_time < 0) {
         ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                       "invalid sb_sleep_ms value \"%V\"", &val);
         return NGX_DECLINED;
     }
     if (sleep_time == 0) {
         return NGX_DECLINED;
     }
     ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_sleep_ctx_t));
     if (ctx == NULL) {
         return NGX_ERROR;
     }
     ctx->request = r;
     ctx->cleaned_up = 0;
     ctx->saved_phase_handler = r->phase_handler;
     ngx_http_set_ctx(r, ctx, ngx_steadybit_sleep_block_module);
     ngx_pool_cleanup_t *cln = ngx_pool_cleanup_add(r->pool, 0);
     if (cln == NULL) {
         return NGX_ERROR;
     }
     cln->handler = ngx_http_sleep_cleanup_handler;
     cln->data = ctx;
     ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                   "sleeping (async) for %i ms", sleep_time);
     ngx_memzero(&ctx->sleep_event, sizeof(ngx_event_t));
     ctx->sleep_event.handler = ngx_http_sleep_wake_handler;
     ctx->sleep_event.data = ctx;
     ctx->sleep_event.log = r->connection->log;
     ngx_add_timer(&ctx->sleep_event, (ngx_msec_t)sleep_time);
     r->main->count++;
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
     ngx_http_sleep_ctx_t *ctx = ev->data;
     ngx_http_request_t *r;
     ngx_http_sleep_loc_conf_t *slcf;
     ngx_str_t block_val;
     ngx_str_t block_status_val;
     ngx_int_t block_status = 503;

     if (ctx == NULL) {
         return;
     }
     r = ctx->request;
     if (r == NULL) {
         return;
     }
     slcf = ngx_http_get_module_loc_conf(r, ngx_steadybit_sleep_block_module);
     // After sleep, check if we should block
     if (slcf->block != NULL) {
         if (ngx_http_complex_value(r, slcf->block, &block_val) == NGX_OK && block_val.len > 0) {
             ngx_int_t block = ngx_atoi(block_val.data, block_val.len);
             if (block != NGX_ERROR && block != 0) {
                 if (slcf->block_status != NULL) {
                     if (ngx_http_complex_value(r, slcf->block_status, &block_status_val) == NGX_OK && block_status_val.len > 0) {
                         ngx_int_t status = ngx_atoi(block_status_val.data, block_status_val.len);
                         if (status > 0) {
                             block_status = status;
                         }
                     }
                 }
                 ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                               "finished sleeping (async), now blocking with status %i due to sb_block", block_status);
                 r->headers_out.status = block_status;
                 ngx_http_finalize_request(r, block_status);
                 r->main->count--;
                 return;
             }
         }
     }
     ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                   "finished sleeping (async)");
     r->phase_handler = ctx->saved_phase_handler + 1;
     ngx_http_core_run_phases(r);
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
     ngx_http_sleep_ctx_t *ctx = data; // Cast data to context

     /* Prevent race conditions with double cleanup */
     if (ctx->cleaned_up) {
         return; // Already cleaned up, avoid double cleanup
     }
     ctx->cleaned_up = 1; // Mark as cleaned up

     ngx_log_error(NGX_LOG_NOTICE, ctx->request->connection->log, 0,
                   "request terminated, cleaning up sleep context"); // Log cleanup

     /* If the timer is still set, cancel it */
     if (ctx->sleep_event.timer_set) {
         ngx_del_timer(&ctx->sleep_event); // Remove timer
     }

     /* Decrement the request's reference count */
     ctx->request->main->count--; // Balance reference count

     /* Note: Context memory is automatically freed when request pool is destroyed */
 }

 // Handler for sb_sleep_ms supporting both config and if block usage
static char *
ngx_http_sleep_ms_handler(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t *value = cf->args->elts;
    if (cf->cmd_type & NGX_HTTP_SIF_CONF) {
        // In if block: register a per-request handler
        ngx_http_core_loc_conf_t *clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
        clcf->handler = ngx_http_sleep_ms_if_request_handler;
        // Save the value for use at request time
        clcf->name = value[1];
        return NGX_CONF_OK;
    }
    // Fallback to config setter for non-if usage
    return ngx_http_sleep_set(cf, cmd, conf);
}

// Handler for sb_block supporting both config and if block usage
static char *
ngx_http_block_handler(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t *value = cf->args->elts;
    if (cf->cmd_type & NGX_HTTP_SIF_CONF) {
        // In if block: register a per-request handler
        ngx_http_core_loc_conf_t *clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
        clcf->handler = ngx_http_block_if_request_handler;
        // Save the value for use at request time
        clcf->name = value[1];
        return NGX_CONF_OK;
    }
    // Fallback to config setter for non-if usage
    return ngx_http_block_set(cf, cmd, conf);
}

// Per-request handler for sb_sleep_ms in if block
static ngx_int_t ngx_http_sleep_ms_if_request_handler(ngx_http_request_t *r)
{
    ngx_http_core_loc_conf_t *clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    ngx_str_t val = clcf->name;
    ngx_int_t sleep_time = ngx_atoi(val.data, val.len);
    if (sleep_time == NGX_ERROR || sleep_time < 0) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "invalid sb_sleep_ms value '%V' in if block", &val);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    if (sleep_time == 0) {
        return NGX_OK;
    }
    ngx_http_sleep_ctx_t *ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_sleep_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ctx->request = r;
    ctx->cleaned_up = 0;
    ctx->saved_phase_handler = r->phase_handler;
    ngx_http_set_ctx(r, ctx, ngx_steadybit_sleep_block_module);
    ngx_pool_cleanup_t *cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    cln->handler = ngx_http_sleep_cleanup_handler;
    cln->data = ctx;
    ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                  "sleeping (async) for %i ms (if block)", sleep_time);
    ngx_memzero(&ctx->sleep_event, sizeof(ngx_event_t));
    ctx->sleep_event.handler = ngx_http_sleep_wake_handler;
    ctx->sleep_event.data = ctx;
    ctx->sleep_event.log = r->connection->log;
    ngx_add_timer(&ctx->sleep_event, (ngx_msec_t)sleep_time);
    r->main->count++;
    return NGX_DONE;
}

// Per-request handler for sb_block in if block
static ngx_int_t ngx_http_block_if_request_handler(ngx_http_request_t *r)
{
    ngx_http_core_loc_conf_t *clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    ngx_str_t block_val = clcf->name;
    ngx_int_t block = ngx_atoi(block_val.data, block_val.len);
    ngx_int_t block_status = 503;
    if (block != NGX_ERROR && block != 0) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "blocking request with status %i due to sb_block in if block", block_status);
        r->headers_out.status = block_status;
        ngx_http_finalize_request(r, block_status);
        return NGX_DONE;
    }
    return NGX_OK;
}
