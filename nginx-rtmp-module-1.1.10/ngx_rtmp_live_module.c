
/*
 * Copyright (C) Roman Arutyunyan
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_cmd_module.h"
//#include "ngx_rtmp_codec_module.h"
#include <ngx_time.h>


static ngx_rtmp_publish_pt              next_publish;
static ngx_rtmp_play_pt                 next_play;
static ngx_rtmp_close_stream_pt         next_close_stream;
static ngx_rtmp_pause_pt                next_pause;
static ngx_rtmp_stream_begin_pt         next_stream_begin;
static ngx_rtmp_stream_eof_pt           next_stream_eof;


static ngx_int_t ngx_rtmp_live_postconfiguration(ngx_conf_t *cf);
static void * ngx_rtmp_live_create_app_conf(ngx_conf_t *cf);
static char * ngx_rtmp_live_merge_app_conf(ngx_conf_t *cf,
       void *parent, void *child);
static char *ngx_rtmp_live_set_msec_slot(ngx_conf_t *cf, ngx_command_t *cmd,
       void *conf);
static void ngx_rtmp_live_start(ngx_rtmp_session_t *s);
static void ngx_rtmp_live_stop(ngx_rtmp_session_t *s);
static void 
ngx_rtmp_free_push_cache_frame(ngx_rtmp_live_stream_t *stream, ngx_rtmp_live_push_cache_t *pc);
static ngx_rtmp_live_push_cache_t *ngx_rtmp_alloc_push_cache_frame(ngx_rtmp_live_stream_t *s);
static ngx_uint_t ngx_rtmp_get_current_time();

ngx_chain_t *
ngx_rtmp_append_data_to_push_cache(size_t chunk_size, ngx_rtmp_live_stream_t *stream, ngx_chain_t *head, ngx_chain_t *in);
void 
ngx_rtmp_free_frame_buffer(ngx_rtmp_live_stream_t *stream, ngx_chain_t *in);
static void
nxg_rtmp_live_av_dump_cache(ngx_event_t *ev);


typedef struct {
    ngx_rtmp_conf_ctx_t         cctx;
    ngx_rtmp_relay_target_t    *target;
} ngx_rtmp_relay_static_t;

static char *
ngx_rtmp_stream_relay_push_pull(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void
ngx_rtmp_stream_relay_publish(ngx_rtmp_live_stream_t *stream, ngx_rtmp_publish_t *v);
static void 
ngx_rtmp_stream_relay_close(ngx_rtmp_live_stream_t *stream);
static void 
nxg_rtmp_live_relay_cache_poll(ngx_event_t *ev);
static void 
ngx_rtmp_stream_codec_ctx_copy(ngx_rtmp_codec_ctx_t *codec_ctx, ngx_rtmp_live_stream_t *stream);



static ngx_command_t  ngx_rtmp_live_commands[] = {

    { ngx_string("live"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, live),
      NULL },

    { ngx_string("stream_buckets"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, nbuckets),
      NULL },

    { ngx_string("buffer"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, buflen),
      NULL },

    { ngx_string("sync"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_rtmp_live_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, sync),
      NULL },

    { ngx_string("interleave"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, interleave),
      NULL },

    { ngx_string("wait_key"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, wait_key),
      NULL },

    { ngx_string("wait_video"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, wait_video),
      NULL },

    { ngx_string("publish_notify"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, publish_notify),
      NULL },

    { ngx_string("play_restart"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, play_restart),
      NULL },

    { ngx_string("idle_streams"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, idle_streams),
      NULL },

    { ngx_string("drop_idle_publisher"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_rtmp_live_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, idle_timeout),
      NULL },

    { ngx_string("push_cache"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, push_cache),
      NULL },
        
    { ngx_string("push_cache_time_len"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, push_cache_time_len),
      NULL },
        
    { ngx_string("push_cache_frame_num"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, push_cache_frame_num),
      NULL },
        
    { ngx_string("publish_delay_close"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, publish_delay_close),
      NULL },
    { ngx_string("publish_delay_close_len"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, publish_delay_close_len),
      NULL },
    { ngx_string("stream_push_reconnect"),
        NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_msec_slot,
        NGX_RTMP_APP_CONF_OFFSET,
        offsetof(ngx_rtmp_live_app_conf_t, stream_push_reconnect),
        NULL },
    { ngx_string("stream_push"),
        NGX_RTMP_APP_CONF|NGX_CONF_1MORE,
        ngx_rtmp_stream_relay_push_pull,
        NGX_RTMP_APP_CONF_OFFSET,
        0,
        NULL },

    { ngx_string("relay_cache"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_live_app_conf_t, relay_cache),
      NULL },
    { ngx_string("relay_cache_poll_len"),
        NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_msec_slot,
        NGX_RTMP_APP_CONF_OFFSET,
        offsetof(ngx_rtmp_live_app_conf_t, relay_cache_poll_len),
        NULL },
    { ngx_string("relay_cache_file"),
        NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_RTMP_APP_CONF_OFFSET,
        offsetof(ngx_rtmp_live_app_conf_t, relay_cache_file),
        NULL  },
    
      ngx_null_command
};


static ngx_rtmp_module_t  ngx_rtmp_live_module_ctx = {
    NULL,                                   /* preconfiguration */
    ngx_rtmp_live_postconfiguration,        /* postconfiguration */
    NULL,                                   /* create main configuration */
    NULL,                                   /* init main configuration */
    NULL,                                   /* create server configuration */
    NULL,                                   /* merge server configuration */
    ngx_rtmp_live_create_app_conf,          /* create app configuration */
    ngx_rtmp_live_merge_app_conf            /* merge app configuration */
};


ngx_module_t  ngx_rtmp_live_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_live_module_ctx,              /* module context */
    ngx_rtmp_live_commands,                 /* module directives */
    NGX_RTMP_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    NULL,                                   /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_rtmp_live_create_app_conf(ngx_conf_t *cf)
{
    ngx_rtmp_live_app_conf_t      *lacf;

    lacf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_live_app_conf_t));
    if (lacf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&lacf->pushes, cf->pool, 1, sizeof(void *)) != NGX_OK) {
        return NULL;
     }
    lacf->live = NGX_CONF_UNSET;
    lacf->nbuckets = NGX_CONF_UNSET;
    lacf->buflen = NGX_CONF_UNSET_MSEC;
    lacf->sync = NGX_CONF_UNSET_MSEC;
    lacf->idle_timeout = NGX_CONF_UNSET_MSEC;
    lacf->interleave = NGX_CONF_UNSET;
    lacf->wait_key = NGX_CONF_UNSET;
    lacf->wait_video = NGX_CONF_UNSET;
    lacf->publish_notify = NGX_CONF_UNSET;
    lacf->play_restart = NGX_CONF_UNSET;
    lacf->idle_streams = NGX_CONF_UNSET;
    
    // 缓冲初始化
    lacf->push_cache  = NGX_CONF_UNSET;
    lacf->push_cache_time_len = NGX_CONF_UNSET;
    lacf->push_cache_frame_num = NGX_CONF_UNSET;
    lacf->publish_delay_close = NGX_CONF_UNSET;
    lacf->publish_delay_close_len = NGX_CONF_UNSET;
    

    lacf->push_reconnect = NGX_CONF_UNSET_MSEC;
    lacf->stream_push_reconnect = NGX_CONF_UNSET_MSEC;

    // 手动开启转推是否开启
    lacf->relay_cache  = NGX_CONF_UNSET;
    lacf->relay_cache_poll_len = NGX_CONF_UNSET_MSEC;
    

    return lacf;
}


static char *
ngx_rtmp_live_merge_app_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_rtmp_live_app_conf_t *prev = parent;
    ngx_rtmp_live_app_conf_t *conf = child;

    ngx_conf_merge_value(conf->live, prev->live, 0);
    ngx_conf_merge_value(conf->nbuckets, prev->nbuckets, 1024);
    ngx_conf_merge_msec_value(conf->buflen, prev->buflen, 0);
    ngx_conf_merge_msec_value(conf->sync, prev->sync, 300);
    ngx_conf_merge_msec_value(conf->idle_timeout, prev->idle_timeout, 0);
    ngx_conf_merge_value(conf->interleave, prev->interleave, 0);
    ngx_conf_merge_value(conf->wait_key, prev->wait_key, 1);
    ngx_conf_merge_value(conf->wait_video, prev->wait_video, 0);
    ngx_conf_merge_value(conf->publish_notify, prev->publish_notify, 0);
    ngx_conf_merge_value(conf->play_restart, prev->play_restart, 0);
    ngx_conf_merge_value(conf->idle_streams, prev->idle_streams, 1);
    
    // 缓冲参数    
    ngx_conf_merge_value(conf->push_cache, prev->push_cache, 0);
    ngx_conf_merge_value(conf->push_cache_time_len, prev->push_cache_time_len, 10000);
    ngx_conf_merge_value(conf->push_cache_frame_num, prev->push_cache_frame_num, 250);
    ngx_conf_merge_value(conf->publish_delay_close, prev->publish_delay_close, 0);
    ngx_conf_merge_msec_value(conf->publish_delay_close_len, prev->publish_delay_close_len, 3000);
    
    ngx_conf_merge_msec_value(conf->stream_push_reconnect, prev->stream_push_reconnect, 3000);

    // 手动开启转推
    ngx_conf_merge_value(conf->push_cache, prev->push_cache, 0);
    ngx_conf_merge_msec_value(conf->relay_cache_poll_len, prev->relay_cache_poll_len, 2000);

    
    conf->pool = ngx_create_pool(4096, &cf->cycle->new_log);
    if (conf->pool == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->streams = ngx_pcalloc(cf->pool,
            sizeof(ngx_rtmp_live_stream_t *) * conf->nbuckets);

    
    return NGX_CONF_OK;
}


static char *
ngx_rtmp_live_set_msec_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                       *p = conf;
    ngx_str_t                  *value;
    ngx_msec_t                 *msp;

    msp = (ngx_msec_t *) (p + cmd->offset);

    value = cf->args->elts;

    if (value[1].len == sizeof("off") - 1 &&
        ngx_strncasecmp(value[1].data, (u_char *) "off", value[1].len) == 0)
    {
        *msp = 0;
        return NGX_CONF_OK;
    }

    return ngx_conf_set_msec_slot(cf, cmd, conf);
}


static ngx_rtmp_live_stream_t **
ngx_rtmp_live_get_stream(ngx_rtmp_session_t *s, u_char *name, int create)
{
    //printf("LLLLL ngx_rtmp_live_get_stream\n");
    ngx_rtmp_live_app_conf_t   *lacf;
    ngx_rtmp_live_stream_t    **stream;
    size_t                      len;

    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);
    if (lacf == NULL) {
        return NULL;
    }

    len = ngx_strlen(name);
    stream = &lacf->streams[ngx_hash_key(name, len) % lacf->nbuckets];

    for (; *stream; stream = &(*stream)->next) {
        if (ngx_strcmp(name, (*stream)->name) == 0) {
            return stream;
        }
    }

    if (!create) {
        return NULL;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
            "live: create stream '%s'", name);

    if (lacf->free_streams) {
        *stream = lacf->free_streams;
        lacf->free_streams = lacf->free_streams->next;
    } else {
        *stream = ngx_palloc(lacf->pool, sizeof(ngx_rtmp_live_stream_t));
    }
    
    ngx_memzero(*stream, sizeof(ngx_rtmp_live_stream_t));
    ngx_memcpy((*stream)->name, name,
            ngx_min(sizeof((*stream)->name) - 1, len));
    (*stream)->epoch = ngx_current_msec;
    (*stream)->is_publish_closed = 0;
     
    if( lacf->push_cache ){
        (*stream)->pool = ngx_create_pool(4096, NULL);
    }
    
    // 有队列情况下 修改内存池
    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0, "push_cache:%d push_cache_time_len:%L push_cache_frame_num:%d", lacf->push_cache, lacf->push_cache_time_len, lacf->push_cache_frame_num);

    return stream;
}


static void
ngx_rtmp_live_idle(ngx_event_t *pev)
{
    //printf("TTTTT ngx_rtmp_live_idle\n");
    ngx_connection_t           *c;
    ngx_rtmp_session_t         *s;

    c = pev->data;
    s = c->data;

    ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                  "live: drop idle publisher");

    ngx_rtmp_finalize_session(s);
}


static void
ngx_rtmp_live_set_status(ngx_rtmp_session_t *s, ngx_chain_t *control,
                         ngx_chain_t **status, size_t nstatus,
                         unsigned active)
{
    //printf("TTTTT ngx_rtmp_live_set_status\n");
    ngx_rtmp_live_app_conf_t   *lacf;
    ngx_rtmp_live_ctx_t        *ctx, *pctx;
    ngx_chain_t               **cl;
    ngx_event_t                *e;
    size_t                      n;

    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "live: set active=%ui", active);

    if (ctx->active == active) {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "live: unchanged active=%ui", active);
        return;
    }
    
    ctx->active = active;
    
    if (ctx->publishing) {

        /* publisher */

        if (lacf->idle_timeout) {
            e = &ctx->idle_evt;

            if (active && !ctx->idle_evt.timer_set) {
                e->data = s->connection;
                e->log = s->connection->log;
                e->handler = ngx_rtmp_live_idle;

                ngx_add_timer(e, lacf->idle_timeout);

            } else if (!active && ctx->idle_evt.timer_set) {
                ngx_del_timer(e);
            }
        }

        ctx->stream->active = active;

        for (pctx = ctx->stream->ctx; pctx; pctx = pctx->next) {
            if (pctx->publishing == 0) {
                ngx_rtmp_live_set_status(pctx->session, control, status,
                        nstatus, active);
            }
        }

        return;
    }

    /* subscriber */

    if( ctx->newflag == 1 
            || (ctx->stream && ctx->stream->push_cache_head == NULL ) 
            || !(lacf->idle_streams && lacf->push_cache && lacf->publish_delay_close) 
            )
    {
        if (control && ngx_rtmp_send_message(s, control, 0) != NGX_OK) {
            ngx_rtmp_finalize_session(s);
            return;
        }
    }

    
    if (!ctx->silent) {
        cl = status;

        for (n = 0; n < nstatus; ++n, ++cl) {
            if (*cl && ngx_rtmp_send_message(s, *cl, 0) != NGX_OK) {
                ngx_rtmp_finalize_session(s);
                return;
            }
        }
    }
    
    if( ctx->newflag == 1 
            || (ctx->stream && ctx->stream->push_cache_head == NULL) 
            || !(lacf->idle_streams && lacf->push_cache && lacf->publish_delay_close) 
            )
    {
        ctx->cs[0].active = 0;
        ctx->cs[0].dropped = 0;

        ctx->cs[1].active = 0;
        ctx->cs[1].dropped = 0;

        ctx->newflag  = 2;
    }
}


static void
ngx_rtmp_live_start(ngx_rtmp_session_t *s)
{
    //printf("TTTTT ngx_rtmp_live_start\n");
    ngx_rtmp_core_srv_conf_t   *cscf;
    ngx_rtmp_live_app_conf_t   *lacf;
    ngx_chain_t                *control;
    ngx_chain_t                *status[3];
    size_t                      n, nstatus;

    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);

    control = ngx_rtmp_create_stream_begin(s, NGX_RTMP_MSID);

    nstatus = 0;

    if (lacf->play_restart) {
        status[nstatus++] = ngx_rtmp_create_status(s, "NetStream.Play.Start",
                                                   "status", "Start live");
        status[nstatus++] = ngx_rtmp_create_sample_access(s);
    }

    if (lacf->publish_notify) {
        status[nstatus++] = ngx_rtmp_create_status(s,
                                                 "NetStream.Play.PublishNotify",
                                                 "status", "Start publishing");
    }

    ngx_rtmp_live_set_status(s, control, status, nstatus, 1);

    if (control) {
        ngx_rtmp_free_shared_chain(cscf, control);
    }

    for (n = 0; n < nstatus; ++n) {
        ngx_rtmp_free_shared_chain(cscf, status[n]);
    }
}


static void
ngx_rtmp_live_stop(ngx_rtmp_session_t *s)
{
    //printf("TTTTT ngx_rtmp_live_stop\n");
    ngx_rtmp_core_srv_conf_t   *cscf;
    ngx_rtmp_live_app_conf_t   *lacf;
    ngx_chain_t                *control;
    ngx_chain_t                *status[3];
    size_t                      n, nstatus;

    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);

    control = ngx_rtmp_create_stream_eof(s, NGX_RTMP_MSID);

    nstatus = 0;

    if (lacf->play_restart) {
        status[nstatus++] = ngx_rtmp_create_status(s, "NetStream.Play.Stop",
                                                   "status", "Stop live");
    }

    if (lacf->publish_notify) {
        status[nstatus++] = ngx_rtmp_create_status(s,
                                               "NetStream.Play.UnpublishNotify",
                                               "status", "Stop publishing");
    }

    ngx_rtmp_live_set_status(s, control, status, nstatus, 0);

    if (control) {
        ngx_rtmp_free_shared_chain(cscf, control);
    }

    for (n = 0; n < nstatus; ++n) {
        ngx_rtmp_free_shared_chain(cscf, status[n]);
    }
}


static ngx_int_t
ngx_rtmp_live_stream_begin(ngx_rtmp_session_t *s, ngx_rtmp_stream_begin_t *v)
{
    //printf("TTTTT ngx_rtmp_live_stream_begin\n");
    ngx_rtmp_live_ctx_t    *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);

    if (ctx == NULL || ctx->stream == NULL || !ctx->publishing) {
        goto next;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "live: stream_begin");

    ngx_rtmp_live_start(s);

next:
    return next_stream_begin(s, v);
}


static ngx_int_t
ngx_rtmp_live_stream_eof(ngx_rtmp_session_t *s, ngx_rtmp_stream_eof_t *v)
{
    //printf("TTTTT ngx_rtmp_live_stream_eof\n");
    ngx_rtmp_live_ctx_t    *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);

    if (ctx == NULL || ctx->stream == NULL || !ctx->publishing) {
        goto next;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "live: stream_eof");

    ngx_rtmp_live_stop(s);

next:
    return next_stream_eof(s, v);
}


static void
ngx_rtmp_live_join(ngx_rtmp_session_t *s, u_char *name, unsigned publisher)
{
    //printf("TTTTT ngx_rtmp_live_join\n");
    ngx_rtmp_live_ctx_t            *ctx;
    ngx_rtmp_live_stream_t        **stream;
    ngx_rtmp_live_app_conf_t       *lacf;

    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);
    if (lacf == NULL) {
        return;
    }
    
    //  #define ngx_rtmp_get_module_ctx(s, module)     (s)->ctx[module.ctx_index]
    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);
    if (ctx && ctx->stream) {
        ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "live: already joined");
        return;
    }

    if (ctx == NULL) {
        ctx = ngx_palloc(s->connection->pool, sizeof(ngx_rtmp_live_ctx_t));
        ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_live_module);
    }

    ngx_memzero(ctx, sizeof(*ctx));

    ctx->session = s;

    ctx->newflag = 1;

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "live: join '%s'", name);
    
    stream = ngx_rtmp_live_get_stream(s, name, publisher || lacf->idle_streams);

    if (stream == NULL ||
        !(publisher || (*stream)->publishing || lacf->idle_streams))
    {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "live: stream not found");

        ngx_rtmp_send_status(s, "NetStream.Play.StreamNotFound", "error",
                             "No such stream");

        ngx_rtmp_finalize_session(s);

        return;
    }

    if (publisher) {
        if ((*stream)->publishing) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "live: already publishing");

            ngx_rtmp_send_status(s, "NetStream.Publish.BadName", "error",
                                 "Already publishing");

            return;
        }

        (*stream)->publishing = 1;
    }
    
    ctx->stream = *stream;
    ctx->publishing = publisher;
    ctx->next = (*stream)->ctx;

    (*stream)->ctx = ctx;

    if (lacf->buflen) {
        s->out_buffer = 1;
    }

    ctx->cs[0].csid = NGX_RTMP_CSID_VIDEO;
    ctx->cs[1].csid = NGX_RTMP_CSID_AUDIO;
    if ( lacf->push_cache ){
         
        ctx->stream->session = s;
        ctx->stream->lacf = lacf;
        ctx->stream->cscf  = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);
        
        ctx->stream->cs[0].csid = NGX_RTMP_CSID_VIDEO;
        ctx->stream->cs[1].csid = NGX_RTMP_CSID_AUDIO;
    }
    
    if (!ctx->publishing && (ctx->stream->active || ctx->stream->push_cache_head ) ) {
        ngx_rtmp_live_start(s);
    }
}

static void 
ngx_rtmp_live_close_plays(ngx_rtmp_live_stream_t *stream)
{
    printf("LLLLL ngx_rtmp_live_close_plays\n");
    ngx_rtmp_session_t             *ss;
    ngx_rtmp_live_ctx_t            *pctx;
    if( !stream )
        return;
    
    for (pctx = stream->ctx; pctx; pctx = pctx->next) {
        if (pctx->publishing == 0) {
            ss = pctx->session;
            ngx_log_debug0(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                    "live: no publisher");
            ngx_rtmp_finalize_session(ss);
        }
    }
}

static void  
ngx_rtmp_live_clear_push_cache(ngx_rtmp_live_stream_t *stream)
{
    if( !stream )
        return;
    ngx_rtmp_live_push_cache_t     *pch;
    
    // 回收ngx_rtmp_live_push_cache_t中buffer的内存
    pch = stream->push_cache_head;
    // 如果队列头 不为空则取队头返回
    while( pch != NULL ){
        if ( pch->next != NULL ){  // 移动头到下一个
            stream->push_cache_head = pch->next;
            stream->push_cache_count -= 1;
        } else {  // 如果队列已经为空，需要把对头队尾指针也置为空
            stream->push_cache_head = NULL;
            stream->push_cache_tail = NULL;
            stream->push_cache_count = 0;
        }
        ngx_rtmp_free_frame_buffer(stream, pch->frame_buf);
        pch = stream->push_cache_head;
    }
    printf("LLLLL ngx_rtmp_live_clear_push_cache head:%p count:%ld\n", stream->push_cache_head, stream->push_cache_count);
    
    // 空闲队列置空
    stream->idle_cache_head = NULL;
    stream->idle_cache_tail = NULL;
    stream->idle_cache_count = 0;
}
static void  
ngx_rtmp_live_free_push_cache(ngx_rtmp_live_stream_t *stream)
{
    if ( !stream )
        return;
    printf("LLLLL ngx_rtmp_live_free_push_cache stream:%p lacf:%p ctx:%p\n", stream, stream->lacf, stream->ctx);

    ngx_rtmp_live_stream_t         **pstream;
    ngx_rtmp_live_app_conf_t       *lacf;
    size_t                      len;

    lacf = stream->lacf;
    // ############## 重置缓存   
    // 清空codec_ctx
    stream->codec_ctx.is_init = 0; 
    // 时间戳单调递增控制 
    stream->push_cache_lts   = 0;  
    stream->push_cache_delta = 0;
     
    // ############## 回收内存 
    if( stream->ctx && stream->lacf )
        return;
    
    // 删除定时器 
    ngx_event_t *ev = &stream->push_cache_event;
    if( ev && ev->timer_set ){
        ngx_del_timer(ev);
    }
    ev = &stream->delay_close_event;
    if( ev && ev->timer_set ){
        ngx_del_timer(ev);
    }
    
    // 回收内存池 
    if( stream->pool ) {
        ngx_destroy_pool(stream->pool);
        stream->pool = NULL;
    }
    
    // 释放stream结构体 
    // 1.没开延时关闭，立即删除
    // 2.缓存没数据，立即删除
    //if (!lacf->publish_delay_close || !stream->push_cache_head){
    len = ngx_strlen(stream->name);
    pstream = &lacf->streams[ngx_hash_key(stream->name, len) % lacf->nbuckets];
    for (; *pstream; pstream = &(*pstream)->next) {
        if (ngx_strcmp(stream->name, (*pstream)->name) == 0) {
            break;
        }
    }
    if ( !pstream || !(*pstream) )
        goto next;
    //    剔除当前stream
    *pstream = (*pstream)->next;
    //     把stream 归还到free_streams
    stream->next = lacf->free_streams;
    lacf->free_streams = stream;
            
next:
    // 置空指针    
    stream->session = NULL;    
    stream->lacf = NULL;
    stream->cscf = NULL;
    
    stream->relay_ctx = NULL;
    stream->main_conf = NULL;
    stream->srv_conf = NULL;
    stream->app_conf = NULL;
    stream->relay_reconnects = NULL;
}

static void
nxg_rtmp_live_delay_close_stream(ngx_event_t *ev)
{
    printf("LLLLL nxg_rtmp_live_delay_close_stream\n");
    if( !ev )
        return;

    ngx_rtmp_live_stream_t        *stream;
    stream = ev->data;
    // stream->active=1时：需要等混存充满然后再释放
    // stream->active=0时：已经超时并没有新publish到来，需要释放。
    //            当超时时间(publish_delay_close_len)大于缓存dump时间(push_cache_time_len)的时候
    //            会出现缓存已经dump完毕，超时事件触发，此时缓存为空，不需要再dump操作。 
    if( !stream || !stream->lacf || stream->active )
        return;
    // 释放队列
     
    ngx_event_t *dev = &stream->push_cache_event;
    if( dev && !dev->timer_set ){
        dev->handler = nxg_rtmp_live_av_dump_cache;
        dev->log = NULL;
        dev->data = stream;

        ngx_uint_t relts = ngx_rtmp_get_current_time();
        stream->push_cache_expts = relts;
        //ngx_add_timer(dev, 10);
        
        nxg_rtmp_live_av_dump_cache(dev);
    }
}

static ngx_int_t
ngx_rtmp_live_close_stream(ngx_rtmp_session_t *s, ngx_rtmp_close_stream_t *v)
{
    //printf("LLLLL ngx_rtmp_live_close_stream\n");
    //ngx_rtmp_session_t             *ss;
    ngx_rtmp_live_ctx_t            *ctx, **cctx;//, *pctx;
    ngx_rtmp_live_stream_t        **stream;
    ngx_rtmp_live_app_conf_t       *lacf;

    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);
    if (lacf == NULL) {
        goto next;
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);
    if (ctx == NULL) {
        goto next;
    }

    if (ctx->stream == NULL) {
        ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "live: not joined");
        goto next;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
            "live: leave '%s'", ctx->stream->name);
    
    if (ctx->stream->publishing && ctx->publishing) {
        ctx->stream->publishing = 0;
    }
    // 找到 ctx 对应的地址  并且从队列中删除之
    for (cctx = &ctx->stream->ctx; *cctx; cctx = &(*cctx)->next) {
        if (*cctx == ctx) {
            *cctx = ctx->next;
            break;
        }
    }
    
    // stream 状态置空 
    if (ctx->publishing || ctx->stream->active) {
        ngx_rtmp_live_stop(s);
    }
    
    if ( ctx->publishing ) {
        ctx->stream->codec_ctx.is_init = 0;

        ngx_rtmp_send_status(s, "NetStream.Unpublish.Success",
                    "status", "Stop publishing");
    }
    
    // 避免play未断，publish断开重连出现乱码
    if ( lacf->push_cache ) {
        if ( ctx->publishing ) {
            ctx->stream->session = NULL;
            ctx->stream->is_publish_closed = 1;
            ctx->stream->publish_closed_count += 1;   

            ngx_rtmp_live_push_cache_t *pct = ctx->stream->push_cache_tail;
            if(pct != NULL && pct->has_closed != 1){
                pct->has_closed = 1;
            } 
                      
            printf("publish close idle_streams:%ld delay_close:%ld ctx:%p cache_head:%p\n", lacf->idle_streams, lacf->publish_delay_close, ctx->stream->ctx, ctx->stream->push_cache_head);
            // 开启缓存：
            // 0.关闭清空延时： 
            //     0. 没有dump缓存时，清空混存，并且无ctx时，释放stream结构体。
            //     1. 正在dump缓存时，会在缓存dump空之后，调用一次释放stream的操作。goto
            // 1.开启清空延时： 
            //     0. 没有dump缓存时，进入等待(如果在等待期内，又有流到来，停止等待。否则超时之后，自动dump混存) 不能立即释放stream。
            //     1. 正在dump混存时，释放完毕之后，释放stream。
            ngx_event_t *ev = &ctx->stream->push_cache_event;
            if( !lacf->publish_delay_close ){ 
                ngx_rtmp_live_clear_push_cache(ctx->stream); 
                    
                if( ev && !ev->timer_set ){
                    ngx_rtmp_stream_relay_close(ctx->stream);
                    // 关闭所有play
                    // 关闭play也会调用释放stream函数。因为只用ctx->stream->ctx为空时，才会释放stream。否则不能释放。
                    if( !lacf->idle_streams ){
                        ngx_rtmp_live_close_plays(ctx->stream);
                    }
                    
                    ngx_rtmp_live_free_push_cache(ctx->stream); 
                } else {
                    ;; // 需要等dump之后。
                }
            } else {
                if( ev && !ev->timer_set ){
                    ev = &ctx->stream->delay_close_event;
                    if( ev && !ev->timer_set ){
                        ev->handler = nxg_rtmp_live_delay_close_stream;
                        ev->log = NULL;
                        ev->data = ctx->stream;

                        ngx_add_timer(ev, lacf->publish_delay_close_len);
                    } else {
                        ;; // 已经开始等待
                    }
                } else {
                    ;; // 需要等dump之后。
                }
            }
        } else {
            printf("play close idle_streams:%ld delay_close:%ld ctx:%p cache_head:%p\n", lacf->idle_streams, lacf->publish_delay_close, ctx->stream->ctx, ctx->stream->push_cache_head);
            // 当publish不存在，play链接进来之后，又关闭。也需要清空stream
            if( !ctx->stream->ctx ){
                // 当publish_delay_close＝true 此时有两种情况不能清空：
                // 1. 正在等待publish_delay_close超时，需要dump完毕之后才能释放stream
                // 2. 正在dump时，需要等待dump完毕。
                if( lacf->publish_delay_close && ctx->stream->push_cache_head ){
                    ;; // 需要等dump之后。
                } else {
                    ngx_rtmp_live_free_push_cache(ctx->stream); 
                }
                 
                // 发送一个状态  只有publishing=0的时候才有用
                if (!ctx->silent && !ctx->publishing && !lacf->play_restart) {
                    ngx_rtmp_send_status(s, "NetStream.Play.Stop", "status", "Stop live");
                }
            }
        }
         
        ctx->stream = NULL;
        // 走cache专有通道 所有操作都走goto
        goto next;
    } else {
        if ( ctx->publishing ) {
            // 1. 立即清空stream。lacf->idle_streams:1:不立即清楚，0:立即清除 
            if ( !lacf->idle_streams ) {
                ngx_rtmp_live_close_plays(ctx->stream);
            }
        }
           
        if ( ctx->stream->ctx ) {
            ctx->stream = NULL;
            goto next;
        }
    }
    printf("------------------- close all plays\n");
    
    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
            "live: delete empty stream '%s'",
            ctx->stream->name);
    
    stream = ngx_rtmp_live_get_stream(s, ctx->stream->name, 0);
    if (stream == NULL) {
        goto next;
    }
    
    // 剔除当前stream
    *stream = (*stream)->next;
    // 把stream 归还到free_streams
    ctx->stream->next = lacf->free_streams;
    lacf->free_streams = ctx->stream;
    ctx->stream = NULL;
     
    if (!ctx->silent && !ctx->publishing && !lacf->play_restart) {
        ngx_rtmp_send_status(s, "NetStream.Play.Stop", "status", "Stop live");
    }
    
next:
    return next_close_stream(s, v);
}

static ngx_int_t
ngx_rtmp_live_pause(ngx_rtmp_session_t *s, ngx_rtmp_pause_t *v)
{
    //printf("TTTTT ngx_rtmp_live_pause\n");
    ngx_rtmp_live_ctx_t            *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);

    if (ctx == NULL || ctx->stream == NULL) {
        goto next;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "live: pause=%i timestamp=%f",
                   (ngx_int_t) v->pause, v->position);

    if (v->pause) {
        if (ngx_rtmp_send_status(s, "NetStream.Pause.Notify", "status",
                                 "Paused live")
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        ctx->paused = 1;

        ngx_rtmp_live_stop(s);

    } else {
        if (ngx_rtmp_send_status(s, "NetStream.Unpause.Notify", "status",
                                 "Unpaused live")
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        ctx->paused = 0;

        ngx_rtmp_live_start(s);
    }

next:
    return next_pause(s, v);
}

// 走原有的通路上传到网上
static ngx_int_t 
ngx_rtmp_live_av_to_net(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
                 ngx_chain_t *in)
{
    ngx_rtmp_live_ctx_t            *ctx, *pctx;
    ngx_rtmp_codec_ctx_t           *codec_ctx;
    ngx_chain_t                    *header, *coheader, *meta,
                                   *apkt, *aapkt, *acopkt, *rpkt;
    ngx_rtmp_core_srv_conf_t       *cscf;
    ngx_rtmp_live_app_conf_t       *lacf;
    ngx_rtmp_session_t             *ss;
    ngx_rtmp_header_t               ch, lh, clh;
    ngx_int_t                       rc, mandatory, dummy_audio;
    ngx_uint_t                      prio;
    ngx_uint_t                      peers, apeers, vpeers;
    ngx_uint_t                      meta_version;
    ngx_uint_t                      csidx;
    uint32_t                        delta;
    ngx_rtmp_live_chunk_stream_t   *cs;
    ngx_int_t                       vasync;
#ifdef NGX_DEBUG
    const char                     *type_s;

    type_s = (h->type == NGX_RTMP_MSG_VIDEO ? "video" : "audio");
#endif

    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);
    if (lacf == NULL) {
        return NGX_ERROR;
    }

    if (!lacf->live || in == NULL  || in->buf == NULL) {
        return NGX_OK;
    }
    
    //  获取上下文
    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);
    if (ctx == NULL || ctx->stream == NULL) {
        return NGX_OK;
    }
    // 非主播模式退出
    if (ctx->publishing == 0) {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "live: %s from non-publisher", type_s);
        return NGX_OK;
    }
    // 数据流是否激活 如果没有则激活 
    if (!ctx->stream->active) {
        ngx_rtmp_live_start(s);
    }

     
    // 视音频不同步的厉害就断链
    vasync = ((ngx_int_t)ctx->cs[1].timestamp - (ngx_int_t)ctx->cs[0].timestamp);
    if( vasync >= 100 || vasync <= -100 ){
        printf("LLLLL ngx_rtmp_live_av audio:%d video:%d sync:%ld\n", ctx->cs[1].timestamp, ctx->cs[0].timestamp, vasync);
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                  "live: video audio sync:%i", vasync);
        
        ngx_rtmp_finalize_session(s);
        return NGX_ERROR;
    }
    
    
    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "live: %s packet timestamp=%uD",
                   type_s, h->timestamp);
    // 更新session时间 
    s->current_time = h->timestamp;

    peers = 0;
    apeers = 0;
    vpeers = 0;
    apkt = NULL;
    aapkt = NULL;
    acopkt = NULL;
    header = NULL;
    coheader = NULL;
    meta = NULL;
    meta_version = 0;
    mandatory = 0;
    
    prio = (h->type == NGX_RTMP_MSG_VIDEO ?
            ngx_rtmp_get_video_frame_type(in) : 0);

    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);
    
    // 音视频分离 0 ／1不分开 
    csidx = !(lacf->interleave || h->type == NGX_RTMP_MSG_VIDEO);

    cs  = &ctx->cs[csidx];
    
    // 初始化 ngx_rtmp_header_t  三类
    ngx_memzero(&ch, sizeof(ch));
    
    // current header
    ch.timestamp = h->timestamp;
    ch.msid = NGX_RTMP_MSID;
    ch.csid = cs->csid;
    ch.type = h->type;
    // last header  current header
    lh = ch;
    
    // last header 
    if (cs->active) {
        lh.timestamp = cs->timestamp;
    }

    clh = lh;
    clh.type = (h->type == NGX_RTMP_MSG_AUDIO ? NGX_RTMP_MSG_VIDEO :
                                                NGX_RTMP_MSG_AUDIO);
    
    // chunk stream 的时间戳
    cs->active = 1;
    cs->timestamp = ch.timestamp;
    // 时间差
    delta = ch.timestamp - lh.timestamp;
    
     
    rpkt = ngx_rtmp_append_shared_bufs(cscf, NULL, in);

    // 准备包
    ngx_rtmp_prepare_message(s, &ch, &lh, rpkt);
    // 上下文   数据头 流的基本信息  视频头音频头帧吕
    
    codec_ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);
    
    if (codec_ctx) {
        // 音频头 
        if (h->type == NGX_RTMP_MSG_AUDIO) {
        //printf("LLLLL NGX_RTMP_MSG_AUDIO interleave:%ld csidx:%ld, type:%d\n", lacf->interleave, csidx, h->type);
            header = codec_ctx->aac_header;
            // 是分离的 
            if (lacf->interleave) {
                // 视频头
                coheader = codec_ctx->avc_header;
            }
            
            if (codec_ctx->audio_codec_id == NGX_RTMP_AUDIO_AAC &&
                ngx_rtmp_is_codec_header(in))
            {
                prio = 0;
                mandatory = 1;
            }
                     
            // STREAM_STATE 
            ngx_rtmp_stream_codec_ctx_copy(codec_ctx, ctx->stream);
        // 视频头
        } else {
        //printf("LLLLL NGX_RTMP_MSG_VIDEO interleave:%ld csidx:%ld, type:%d\n", lacf->interleave, csidx, h->type);
            header = codec_ctx->avc_header;

            if (lacf->interleave) {
                coheader = codec_ctx->aac_header;
            }
            
            if (codec_ctx->video_codec_id == NGX_RTMP_VIDEO_H264 &&
                ngx_rtmp_is_codec_header(in))
            {
                prio = 0;
                mandatory = 1;
            }
               
            // STREAM_STATE 
            ngx_rtmp_stream_codec_ctx_copy(codec_ctx, ctx->stream);
        }
        
        // 第一个发送的包  数据头 
        if ( codec_ctx->meta ) {
            meta = codec_ctx->meta;
            // 版本信息 初始化0
            meta_version = codec_ctx->meta_version;
            
            // STREAM_STATE
            ctx->stream->codec_ctx.meta_version = codec_ctx->meta_version;
        }
    }
    

    /* broadcast to all subscribers */
    // 下一个流的上下文 
    for (pctx = ctx->stream->ctx; pctx; pctx = pctx->next) {
        if (pctx == ctx || pctx->paused) {
            continue;
        }
            
        ss = pctx->session;
        cs = &pctx->cs[csidx];

        /* send metadata */
        if (meta && meta_version != pctx->meta_version) {
            ngx_log_debug0(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                           "live: meta");
        
             if (ngx_rtmp_send_message(ss, meta, 0) == NGX_OK) {
                // 改变状态
                pctx->meta_version = meta_version;
            }
        }
        
        /* sync stream */
        if (cs->active && (lacf->sync && cs->dropped > lacf->sync)) {
            ngx_log_debug2(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                           "live: sync %s dropped=%uD", type_s, cs->dropped);
            
            cs->active = 0;
            cs->dropped = 0;
        }
        
        /* absolute packet */
        if (!cs->active) {
            // 如果头没到来
            if (mandatory) {
                ngx_log_debug0(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                        "live: skipping header");
                continue;
            }
            // 等视频帧到来：如果收到音频 并且 cs[0]应该是视频流 
            if (lacf->wait_video && h->type == NGX_RTMP_MSG_AUDIO &&
                    !pctx->cs[0].active)
            {
                ngx_log_debug0(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                        "live: waiting for video");
                continue;
            }
            // 如果关键帧没到来 等关键帧
            if (lacf->wait_key && prio != NGX_RTMP_VIDEO_KEY_FRAME &&
                    (lacf->interleave || h->type == NGX_RTMP_MSG_VIDEO))
            {
                ngx_log_debug0(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                        "live: skip non-key");
                continue;
            }
            // 丢包过后逻辑  
            dummy_audio = 0;
            if (lacf->wait_video && h->type == NGX_RTMP_MSG_VIDEO &&
                    !pctx->cs[1].active)
            {
                dummy_audio = 1;
                if (aapkt == NULL) {
                    aapkt = ngx_rtmp_alloc_shared_buf(cscf);
                    // 发送 自己初始化的最新的头 
                    ngx_rtmp_prepare_message(s, &clh, NULL, aapkt);
                }
            }
           
            // 发送 AV header 
            if (header || coheader) {
                /* send absolute codec header */
                // 发送绝对头
                ngx_log_debug2(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                               "live: abs %s header timestamp=%uD",
                               type_s, lh.timestamp);

                if (header) {
                    if (apkt == NULL) {
                        apkt = ngx_rtmp_append_shared_bufs(cscf, NULL, header);
                        ngx_rtmp_prepare_message(s, &lh, NULL, apkt);
                    }

                    rc = ngx_rtmp_send_message(ss, apkt, 0);
                    if (rc != NGX_OK) {
                        continue;
                    }
                }

                if (coheader) {
                    if (acopkt == NULL) {
                        acopkt = ngx_rtmp_append_shared_bufs(cscf, NULL, coheader);
                        ngx_rtmp_prepare_message(s, &clh, NULL, acopkt);
                    }

                    rc = ngx_rtmp_send_message(ss, acopkt, 0);
                    if (rc != NGX_OK) {
                        continue;
                    }

                } else if (dummy_audio) {
                    ngx_rtmp_send_message(ss, aapkt, 0);
                }

                cs->timestamp = lh.timestamp;
                cs->active = 1;
                ss->current_time = cs->timestamp;
            } else {
                // 发包绝对包 
                /* send absolute packet */
                ngx_log_debug2(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                               "live: abs %s packet timestamp=%uD",
                               type_s, ch.timestamp);

                if (apkt == NULL) {
                    apkt = ngx_rtmp_append_shared_bufs(cscf, NULL, in);
                    ngx_rtmp_prepare_message(s, &ch, NULL, apkt);
                }

                rc = ngx_rtmp_send_message(ss, apkt, prio);
                if (rc != NGX_OK) {
                    continue;
                }

                cs->timestamp = ch.timestamp;
                cs->active = 1;
                ss->current_time = cs->timestamp;

                ++peers;
                if (h->type == NGX_RTMP_MSG_AUDIO) {
                    ++apeers;
                } else {
                    ++vpeers;
                }

                if (dummy_audio) {
                    ngx_rtmp_send_message(ss, aapkt, 0);
                }

                continue;
            }
        }
        // end cs->active 

        /* send relative packet */
        ngx_log_debug2(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                       "live: rel %s packet delta=%uD",
                       type_s, delta);
        // 发送数据 payload
        if (ngx_rtmp_send_message(ss, rpkt, prio) != NGX_OK) {
            ++pctx->ndropped;
            ctx->stream->ndropped += 1;

            cs->dropped += delta;

            if (mandatory) {
                ngx_log_debug0(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                               "live: mandatory packet failed");
                ngx_rtmp_finalize_session(ss);
            }

            continue;
        }

        cs->timestamp += delta;
        ++peers;
        if (h->type == NGX_RTMP_MSG_AUDIO) {
            ++apeers;
        } else {
            ++vpeers;
        }
        ss->current_time = cs->timestamp;
    }

    if (rpkt) {
        ngx_rtmp_free_shared_chain(cscf, rpkt);
    }

    if (apkt) {
        ngx_rtmp_free_shared_chain(cscf, apkt);
    }

    if (aapkt) {
        ngx_rtmp_free_shared_chain(cscf, aapkt);
    }

    if (acopkt) {
        ngx_rtmp_free_shared_chain(cscf, acopkt);
    }

    ngx_rtmp_update_bandwidth(&ctx->stream->bw_in, h->mlen);
    ngx_rtmp_update_bandwidth(h->type == NGX_RTMP_MSG_AUDIO ?
                              &ctx->stream->bw_in_audio :
                              &ctx->stream->bw_in_video,
                              h->mlen);

    ngx_rtmp_update_bandwidth(&ctx->stream->bw_out, h->mlen * peers);
    ngx_rtmp_update_bandwidth(h->type == NGX_RTMP_MSG_AUDIO ?
                              &ctx->stream->bw_out_audio :
                              &ctx->stream->bw_out_video,
                              h->type == NGX_RTMP_MSG_AUDIO ? 
                              h->mlen * apeers :
                              h->mlen * vpeers);

    return NGX_OK;
} // end ngx_rtmp_live_av_to_net

void
ngx_rtmp_prepare_message_with_cache(ngx_rtmp_live_stream_t *stream, ngx_rtmp_header_t *h,
        ngx_rtmp_header_t *lh, ngx_chain_t *out)
{
    ngx_chain_t                *l;
    u_char                     *p, *pp;
    ngx_int_t                   hsize, thsize, nbufs;
    uint32_t                    mlen, timestamp, ext_timestamp;
    static uint8_t              hdrsize[] = { 12, 8, 4, 1 };
    u_char                      th[7];
    ngx_rtmp_core_srv_conf_t   *cscf;
    uint8_t                     fmt;
    
    cscf = stream->cscf;
    /*
    ngx_connection_t           *c;
    c = s->connection;
    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    if (h->csid >= (uint32_t)cscf->max_streams) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                "RTMP out chunk stream too big: %D >= %D",
                h->csid, cscf->max_streams);
        ngx_rtmp_finalize_session(s);
        return;
    }
    */

    /* detect packet size */
    mlen = 0;
    nbufs = 0;
    for(l = out; l; l = l->next) {
        mlen += (l->buf->last - l->buf->pos);
        ++nbufs;
    }

    fmt = 0;
    if (lh && lh->csid && h->msid == lh->msid) {
        ++fmt;
        if (h->type == lh->type && mlen && mlen == lh->mlen) {
            ++fmt;
            if (h->timestamp == lh->timestamp) {
                ++fmt;
            }
        }
        timestamp = h->timestamp - lh->timestamp;
    } else {
        timestamp = h->timestamp;
    }

    /*if (lh) {
     *lh = *h;
     lh->mlen = mlen;
     }*/

    hsize = hdrsize[fmt];
    /*  //////////////////// +
    ngx_log_debug8(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
            "RTMP prep %s (%d) fmt=%d csid=%uD timestamp=%uD "
            "mlen=%uD msid=%uD nbufs=%d",
            ngx_rtmp_message_type(h->type), (int)h->type, (int)fmt,
            h->csid, timestamp, mlen, h->msid, nbufs);
    */
    ext_timestamp = 0;
    if (timestamp >= 0x00ffffff) {
        ext_timestamp = timestamp;
        timestamp = 0x00ffffff;
        hsize += 4;
    }

    if (h->csid >= 64) {
        ++hsize;
        if (h->csid >= 320) {
            ++hsize;
        }
    }

    /* fill initial header */
    out->buf->pos -= hsize;
    p = out->buf->pos;

    /* basic header */
    *p = (fmt << 6);
    if (h->csid >= 2 && h->csid <= 63) {
        *p++ |= (((uint8_t)h->csid) & 0x3f);
    } else if (h->csid >= 64 && h->csid < 320) {
        ++p;
        *p++ = (uint8_t)(h->csid - 64);
    } else {
        *p++ |= 1;
        *p++ = (uint8_t)(h->csid - 64);
        *p++ = (uint8_t)((h->csid - 64) >> 8);
    }

    /* create fmt3 header for successive fragments */
    thsize = p - out->buf->pos;
    ngx_memcpy(th, out->buf->pos, thsize);
    th[0] |= 0xc0;
    /* message header */
    if (fmt <= 2) {
        pp = (u_char*)&timestamp;
        *p++ = pp[2];
        *p++ = pp[1];
        *p++ = pp[0];
        if (fmt <= 1) {
            pp = (u_char*)&mlen;
            *p++ = pp[2];
            *p++ = pp[1];
            *p++ = pp[0];
            *p++ = h->type;
            if (fmt == 0) {
                pp = (u_char*)&h->msid;
                *p++ = pp[0];
                *p++ = pp[1];
                *p++ = pp[2];
                *p++ = pp[3];
            }
        }
    }

    /* extended header */
    if (ext_timestamp) {
        pp = (u_char*)&ext_timestamp;
        *p++ = pp[3];
        *p++ = pp[2];
        *p++ = pp[1];
        *p++ = pp[0];

        /* This CONTRADICTS the standard
         * but that's the way flash client
         * wants data to be encoded;
         * ffmpeg complains */
        if (cscf->play_time_fix) {
            ngx_memcpy(&th[thsize], p - 4, 4);
            thsize += 4;
        }
    }

    /* append headers to successive fragments */
    for(out = out->next; out; out = out->next) {
        out->buf->pos -= thsize;
        ngx_memcpy(out->buf->pos, th, thsize);
    }
}

static ngx_int_t 
ngx_rtmp_live_av_to_play(ngx_rtmp_live_stream_t *stream, ngx_rtmp_header_t *h,
                 ngx_chain_t *in, ngx_int_t mandatory)
{
    ngx_rtmp_live_ctx_t            *pctx;
    ngx_rtmp_session_t             *ss;
    ngx_rtmp_live_chunk_stream_t   *cs;
    ngx_rtmp_header_t               ch, lh;//, clh;
    ngx_chain_t                    *rpkt;
    uint32_t                        delta;
    ngx_uint_t                      csidx;
    
    ngx_rtmp_live_app_conf_t       *lacf;
    ngx_rtmp_core_srv_conf_t       *cscf;
    cscf = stream->cscf;
    lacf = stream->lacf;   /////////////////////////+
    
    ngx_chain_t                    *apkt;//, *acopkt;
    ngx_uint_t                      prio;
    ngx_uint_t                      peers, apeers, vpeers;
    ngx_int_t                       rc;
    
    apkt = NULL;
    //acopkt = NULL;
    peers = 0;
    apeers = 0;
    vpeers = 0;
#ifdef NGX_DEBUG
    const char                     *type_s;
    type_s = (h->type == NGX_RTMP_MSG_VIDEO ? "video" : "audio");
#endif
    // 好像是优先级
    prio = (h->type == NGX_RTMP_MSG_VIDEO ?
            ngx_rtmp_get_video_frame_type(in) : 0); 

    // 音视频分离 0 ／1不分开
    csidx = !(lacf->interleave || h->type == NGX_RTMP_MSG_VIDEO); 
    cs  = &(stream->cs[csidx]); 
    // 初始化 ch ,lh 
    ngx_memzero(&ch, sizeof(ch));
     
    ch.timestamp = h->timestamp;
    ch.msid = NGX_RTMP_MSID;
    ch.csid = cs->csid;
    ch.type = h->type;
    // last header  current header
    lh = ch;
    
    /*
    clh = lh;
    clh.type = (h->type == NGX_RTMP_MSG_AUDIO ? NGX_RTMP_MSG_VIDEO :
                                                NGX_RTMP_MSG_AUDIO);
    */
    //  不知道干啥用 
    if ( cs->active ) {
        lh.timestamp = cs->timestamp;
    } 
    

    cs->active = 1;
    cs->timestamp = ch.timestamp;
    // 时间差
    delta = ch.timestamp - lh.timestamp;  
    

    rpkt = ngx_rtmp_append_shared_bufs(cscf, NULL, in);
    // 准备包
    ngx_rtmp_prepare_message_with_cache(stream, &ch, &lh, rpkt);
         
    /* broadcast to all subscribers */
    // 下一个流的上下文 
    for (pctx = stream->ctx; pctx; pctx = pctx->next) {
        //if (pctx == ctx || pctx->paused) {
        if ( pctx->paused ) {
            continue;
        }
    
        if ( pctx->publishing ){
            continue;
        }
           
        ss = pctx->session;
        cs = &pctx->cs[csidx];
        
        if (stream->codec_ctx.meta && stream->codec_ctx.meta_version != pctx->meta_version) {
            if (ngx_rtmp_send_message(ss, stream->codec_ctx.meta, 0) == NGX_OK) {
                // 改变状态
                pctx->meta_version = stream->codec_ctx.meta_version;
            }
        }

        if (!cs->active) {
            if( mandatory ) {
                continue;
            }

            if( stream->codec_ctx.aac_header && stream->codec_ctx.avc_header){
                if ( h->type == NGX_RTMP_MSG_AUDIO ) {
                    if (apkt == NULL) {
                        apkt = ngx_rtmp_append_shared_bufs(cscf, NULL, stream->codec_ctx.aac_header);
                        ngx_rtmp_prepare_message_with_cache(stream, &lh, NULL, apkt);
                    }
                    
                    rc = ngx_rtmp_send_message(ss, apkt, 0);
                    if (rc != NGX_OK) {
                        printf("aac send fail\n");
                        continue;
                    }
                } else {
                    if (apkt == NULL) {
                        apkt = ngx_rtmp_append_shared_bufs(cscf, NULL, stream->codec_ctx.avc_header);
                        ngx_rtmp_prepare_message_with_cache(stream, &lh, NULL, apkt);
                    }
                    
                    rc = ngx_rtmp_send_message(ss, apkt, 0);
                    if (rc != NGX_OK) {
                        printf("avc send fail\n");
                        continue;
                    }
                }
            }
            
            cs->timestamp = lh.timestamp;
            cs->active = 1;
            ss->current_time = cs->timestamp;
        }
        
        // send relative packet 
        // 发送相对包
        ngx_log_debug2(NGX_LOG_DEBUG_RTMP, ss->connection->log, 0,
                       "live: rel %s packet delta=%uD",
                       type_s, delta);
        // 发送数据 payload
        if (ngx_rtmp_send_message(ss, rpkt, prio) != NGX_OK) {
            ++pctx->ndropped;
            stream->ndropped += 1;
            
            cs->dropped += delta;
            continue;
        }
        
        cs->timestamp += delta;
        // STREAM_STATE   监控输出的数据量
        ++peers;
        if ( h->type == NGX_RTMP_MSG_AUDIO ) {
            ++apeers;
        } else {
            ++vpeers;
        } 
        ss->current_time = cs->timestamp;
    }
    
    if (rpkt) {
        ngx_rtmp_free_shared_chain(cscf, rpkt);
    }
    
    if (apkt) {
        ngx_rtmp_free_shared_chain(cscf, apkt);
    }

    ngx_rtmp_update_bandwidth(&stream->bw_in, h->mlen);
    ngx_rtmp_update_bandwidth(h->type == NGX_RTMP_MSG_AUDIO ?
                              &stream->bw_in_audio :
                              &stream->bw_in_video,
                              h->mlen);

    ngx_rtmp_update_bandwidth(&stream->bw_out, h->mlen * peers);
    ngx_rtmp_update_bandwidth(h->type == NGX_RTMP_MSG_AUDIO ?
                              &stream->bw_out_audio :
                              &stream->bw_out_video,
                              h->type == NGX_RTMP_MSG_AUDIO ? 
                              h->mlen * apeers :
                              h->mlen * vpeers);
    return NGX_OK;
}

static int64_t 
ngx_rtmp_live_av_dump_cache_frame(ngx_rtmp_live_stream_t *stream)
{
    int64_t delta = 0;
    // 缓存队列
    ngx_rtmp_live_push_cache_t  *pc, *tpc;
    pc = stream->push_cache_head;
    if( pc != NULL ){
        tpc = pc->next;  
        if(tpc != NULL){
            if(pc->mandatory == 0 && tpc->mandatory == 0){
                delta = tpc->frame_pts - pc->frame_pts; 
            }
            stream->push_cache_head = pc->next;
            stream->push_cache_count -= 1;
        } else {
            stream->push_cache_head = NULL;
            stream->push_cache_tail = NULL;
            
            stream->push_cache_count = 0;
        }
        
        // 发送数据
        if ( stream->publish_closed_count == 0 && stream->session != NULL ) {
            ngx_rtmp_live_av_to_net(stream->session, &pc->frame_header, pc->frame_buf);

            // 音视频分离 0/1不分开 
            ngx_uint_t csidx = !(stream->lacf->interleave || pc->frame_header.type == NGX_RTMP_MSG_VIDEO);
            stream->cs[csidx] = stream->ctx->cs[csidx];
        } else {
            ngx_rtmp_live_av_to_play(stream, &pc->frame_header, pc->frame_buf, pc->mandatory);
                 
            if ( pc->has_closed == 1 ){
                pc->has_closed = 0;

                stream->cs[0].timestamp = 0;
                stream->cs[1].timestamp = 0;
                stream->publish_closed_count -= 1;
                // 当最后一次publish关闭之前的数据释放完毕（此时缓存应该为空）。需要清空内存
                if( stream->publish_closed_count == 0 ){
                    if( stream->codec_ctx.aac_header )
                        ngx_rtmp_free_frame_buffer(stream, stream->codec_ctx.aac_header);
                    if ( stream->codec_ctx.avc_header )
                        ngx_rtmp_free_frame_buffer(stream, stream->codec_ctx.avc_header);
                    stream->codec_ctx.aac_header = NULL;
                    stream->codec_ctx.avc_header = NULL;
                }

                delta = 0;
            }
        } 

        // 把内存归还给空闲队列
        ngx_rtmp_free_push_cache_frame(stream, pc);
    }
    return delta;
}

// RELAY_CACHE
static void 
ngx_rtmp_live_relay_cache_check(ngx_rtmp_live_stream_t *stream)
{
    if( stream->lacf->relay_cache_ctrl == 1 ){
        // 首次开始转推
        if( !stream->is_relay_start ){
            ngx_rtmp_stream_relay_publish(stream, &stream->publish);
        } 
    } else {
        // 关闭转推(已经开启转推时，关闭)
        if( stream->is_relay_start ){
            ngx_rtmp_stream_relay_close(stream);             
        }
    }
}

// DUMP_CACHE 从缓存中释放数据
static void
nxg_rtmp_live_av_dump_cache(ngx_event_t *ev)
{
    if( !ev )
        return;
    // 开始转推
    // start real ts, end expect ts 
    ngx_uint_t relts ,expts;
    relts = ngx_rtmp_get_current_time();
    ngx_rtmp_live_stream_t *stream = ev->data;
    if( !stream || !stream->lacf )
        return;
     
    // RELAY_CACHE             
    ngx_rtmp_live_relay_cache_check(stream);
     
    // 期望到达时间
    expts = stream->push_cache_expts;
    // 误差时间 (当前时间 － 预计到达时间)
    // zeta累计误差 iota上次误差
    int64_t delay = 0, delta = 0, zeta = relts - expts;
     
    do{
        // 下一帧发送时间
        delta = ngx_rtmp_live_av_dump_cache_frame(stream);
        
        if( delta > 0 )
            zeta -= delta;
    } while (zeta >= 0 && stream->push_cache_head != NULL);
     
    // 如果对列为空  
    if ( stream->push_cache_head ){
        // 等待下一帧发送时间
        delay -= zeta;
        if( delay < 0 || delay > 50 ) {
            printf("####dump_cache delta:%ld zeta:%ld delay:%ld\n", delta, zeta, delay);
        }

        // 容错操作
        if(delay > 200) {
            delay = 200;
        }
        // 修改预计到达时间
        expts = relts + delay; 
        stream->push_cache_expts = expts;
        
        ngx_add_timer(ev, delay); 
    }
    else
    {
        printf("LLLLL nxg_rtmp_live_av_dump_cache cache is null\n");
        // 清空所有的play
        if ( stream->lacf && !stream->lacf->idle_streams ) {
            ngx_rtmp_live_close_plays(stream);
        }
        
        ngx_rtmp_stream_relay_close(stream);             
        ngx_rtmp_live_free_push_cache(stream); 
    }
}

// TO_PLAY  STREAM_STATE
static void 
ngx_rtmp_stream_codec_ctx_copy(ngx_rtmp_codec_ctx_t *codec_ctx, ngx_rtmp_live_stream_t *stream)
{
    stream->codec_ctx.width           = codec_ctx->width;
    stream->codec_ctx.height          = codec_ctx->height;
    stream->codec_ctx.duration        = codec_ctx->duration;
    stream->codec_ctx.frame_rate      = codec_ctx->frame_rate;
    stream->codec_ctx.video_data_rate = codec_ctx->video_data_rate;
    stream->codec_ctx.video_codec_id  = codec_ctx->video_codec_id;
    stream->codec_ctx.audio_data_rate = codec_ctx->audio_data_rate;
    stream->codec_ctx.audio_codec_id  = codec_ctx->audio_codec_id;
    stream->codec_ctx.aac_profile     = codec_ctx->aac_profile;
    stream->codec_ctx.aac_chan_conf   = codec_ctx->aac_chan_conf;
    stream->codec_ctx.aac_sbr         = codec_ctx->aac_sbr;
    stream->codec_ctx.aac_ps          = codec_ctx->aac_ps;
    stream->codec_ctx.avc_profile     = codec_ctx->avc_profile;
    stream->codec_ctx.avc_compat      = codec_ctx->avc_compat;
    stream->codec_ctx.avc_level       = codec_ctx->avc_level;
    stream->codec_ctx.avc_nal_bytes   = codec_ctx->avc_nal_bytes;
    stream->codec_ctx.avc_ref_frames  = codec_ctx->avc_ref_frames;
    stream->codec_ctx.sample_rate     = codec_ctx->sample_rate;    // 5512, 11025, 22050, 44100 //
    stream->codec_ctx.sample_size     = codec_ctx->sample_size;    // 1=8bit, 2=16bit //
    stream->codec_ctx.audio_channels  = codec_ctx->audio_channels; // 1, 2 //
   // 
   // u_char                      profile[32];
   // u_char                      level[32];

   // ngx_uint_t                  meta_version;
   // 
}

// STREAM_STATE
static void 
ngx_rtmp_stream_publish_header_copy(ngx_rtmp_session_t *s, ngx_rtmp_stream_codec_ctx_t *codec_ctx, ngx_rtmp_live_stream_t *stream, ngx_flag_t publishing)
{
    u_char *p;
    ngx_rtmp_codec_ctx_t                *codec;
    ngx_pool_t                          *pool;
    codec = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);
    
    codec_ctx->is_init         = 1;
    codec_ctx->width           = codec->width;
    codec_ctx->height          = codec->height;
    codec_ctx->duration        = codec->duration;
    codec_ctx->frame_rate      = codec->frame_rate;
    codec_ctx->video_data_rate = codec->video_data_rate;
    codec_ctx->video_codec_id  = codec->video_codec_id;
    codec_ctx->audio_data_rate = codec->audio_data_rate;
    codec_ctx->audio_codec_id  = codec->audio_codec_id;
    codec_ctx->aac_profile     = codec->aac_profile;
    codec_ctx->aac_chan_conf   = codec->aac_chan_conf;
    codec_ctx->aac_sbr         = codec->aac_sbr;
    codec_ctx->aac_ps          = codec->aac_ps;
    codec_ctx->avc_profile     = codec->avc_profile;
    codec_ctx->avc_compat      = codec->avc_compat;
    codec_ctx->avc_level       = codec->avc_level;
    codec_ctx->avc_nal_bytes   = codec->avc_nal_bytes;
    codec_ctx->avc_ref_frames  = codec->avc_ref_frames;
    codec_ctx->sample_rate     = codec->sample_rate; 
    codec_ctx->sample_size     = codec->sample_size;
    codec_ctx->audio_channels  = codec->audio_channels;
    
    codec_ctx->number          = s->connection->number;
    
    if( publishing ) 
        pool = stream->pool;  
    else 
        pool = s->connection->pool;
     
    if( s->connection->addr_text.len ){
        p = ngx_pnalloc(pool, s->connection->addr_text.len); 
        ngx_memcpy(p, 
                s->connection->addr_text.data, 
                s->connection->addr_text.len);
        codec_ctx->addr_text.data = p;
        codec_ctx->addr_text.len = s->connection->addr_text.len;
    }
    
    codec_ctx->epoch = s->epoch;
    
    if( s->flashver.len ) {
        p = ngx_pnalloc(pool, s->flashver.len); 
        ngx_memcpy(p, 
                s->flashver.data, 
                s->flashver.len);
        codec_ctx->flashver.data = p;
        codec_ctx->flashver.len = s->flashver.len; 
    }
    
    if( s->page_url.len ){
        p = ngx_pnalloc(pool, s->page_url.len); 
        ngx_memcpy(p, 
                s->page_url.data, 
                s->page_url.len);
        codec_ctx->page_url.data = p;
        codec_ctx->page_url.len = s->page_url.len;
    }
    
    if( s->swf_url.len ){
        p = ngx_pnalloc(pool, s->swf_url.len); 
        ngx_memcpy(p, 
                s->swf_url.data, 
                s->swf_url.len);
        codec_ctx->swf_url.data = p;
        codec_ctx->swf_url.len = s->swf_url.len; 
    }
    
    if( s->tc_url.len ){
        p = ngx_pnalloc(pool, s->tc_url.len); 
        ngx_memcpy(p, 
                s->tc_url.data, 
                s->tc_url.len);
        codec_ctx->tc_url.data = p;
        codec_ctx->tc_url.len = s->tc_url.len; 
    }
}

// PUSH_CACHE 缓存接收到的数据
static ngx_int_t 
ngx_rtmp_live_av_to_cache(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
                 ngx_chain_t *in)
{
    ngx_rtmp_core_srv_conf_t       *cscf;
    ngx_rtmp_live_ctx_t            *ctx;
    ngx_rtmp_live_app_conf_t       *lacf;
    ngx_rtmp_live_push_cache_t  *pc, *pch, *pct, *pcp;
    ngx_uint_t                      prio;
    ngx_rtmp_codec_ctx_t           *codec_ctx;
    ngx_int_t                       mandatory;
    ngx_msec_t                      timestamp, last ,cur;
    ngx_chain_t                    *header;
    header = NULL;
    
#ifdef NGX_DEBUG
    const char                     *type_s;

    type_s = (h->type == NGX_RTMP_MSG_VIDEO ? "video" : "audio");
#endif

    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);
    if (lacf == NULL) {
        return NGX_ERROR;
    }
    
    if (!lacf->live || in == NULL  || in->buf == NULL) {
        return NGX_OK;
    }
    
    //  获取上下文
    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);
    if (ctx == NULL || ctx->stream == NULL) {
        return NGX_OK;
    }
     
    // 非主播模式退出
    if (ctx->publishing == 0) {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "live: %s from non-publisher", type_s);
        return NGX_OK;
    }
     
    // 数据流是否激活 如果没有则激活 
    if (!ctx->stream->active) {
        ngx_rtmp_live_start(s);
    }
    
    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                    "live: %s packet timestamp=%uD",
                    type_s, h->timestamp);
    
    // ############### 缓存数据
    // 优先从空闲队列中获取
    pc = ngx_rtmp_alloc_push_cache_frame(ctx->stream); 
    if(pc == NULL)
        return NGX_ERROR; 
     
    mandatory = 0;
    
    prio = (h->type == NGX_RTMP_MSG_VIDEO ?
            ngx_rtmp_get_video_frame_type(in) : 0);
    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);
     
    ctx->stream->session      = s;
    ctx->stream->current_time = s->current_time;
    ctx->stream->interleave   = lacf->interleave;
    
    codec_ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);
    if( codec_ctx ){
        if (h->type == NGX_RTMP_MSG_AUDIO) {
            if (codec_ctx->audio_codec_id == NGX_RTMP_AUDIO_AAC &&
                    ngx_rtmp_is_codec_header(in))
            {
                mandatory = 1;
            }
            ngx_rtmp_stream_codec_ctx_copy(codec_ctx, ctx->stream);

            // ADDD 
            header = codec_ctx->aac_header;
            if ( !ctx->stream->codec_ctx.aac_header && header ) {
                ctx->stream->codec_ctx.aac_header = ngx_rtmp_append_data_to_push_cache(cscf->chunk_size, ctx->stream, NULL, header);
            }
        } else {
            if (codec_ctx->video_codec_id == NGX_RTMP_VIDEO_H264 &&
                    ngx_rtmp_is_codec_header(in))
            {
                mandatory = 1;
            }
            ngx_rtmp_stream_codec_ctx_copy(codec_ctx, ctx->stream);
               
            // ADDD
            header = codec_ctx->avc_header;
            if ( !ctx->stream->codec_ctx.avc_header && header ){
                ctx->stream->codec_ctx.avc_header = ngx_rtmp_append_data_to_push_cache(cscf->chunk_size, ctx->stream, NULL, header);
            } 
        }
               
        // ADDD 
        if( codec_ctx->meta && !ctx->stream->codec_ctx.meta ){
            ctx->stream->codec_ctx.meta = ngx_rtmp_append_data_to_push_cache(cscf->chunk_size, ctx->stream, NULL, codec_ctx->meta);
            // 版本信息 初始化0
            ctx->stream->codec_ctx.meta_version = codec_ctx->meta_version;
        }
    }
     
    // 如果推流中间断开过，保证时间戳单调递增
    timestamp = 0;
    if ( mandatory == 0 ) {
        last = ctx->stream->push_cache_delta + ctx->stream->push_cache_lts; 
        cur  = ctx->stream->push_cache_delta + h->timestamp;
        if ( ctx->stream->is_publish_closed ){  
            ctx->stream->is_publish_closed = 0;
            if ( last > cur )
                ctx->stream->push_cache_delta += ctx->stream->push_cache_lts;       
        }
        ctx->stream->push_cache_lts = h->timestamp;
        timestamp = ctx->stream->push_cache_delta + h->timestamp;
        if (h->type == NGX_RTMP_MSG_AUDIO) {
            ctx->stream->push_cache_aets = timestamp;
        } else {
            ctx->stream->push_cache_vets = timestamp;
        }
    } 
    
    pc->mandatory    = mandatory;       // 是否为 音视频pps sps头
    pc->frame_type   = h->type ;
    pc->frame_flag   = prio;           // 是否为关键帧
    pc->frame_pts    = timestamp;    // 时间戳
    pc->frame_buf    = ngx_rtmp_append_data_to_push_cache(cscf->chunk_size, ctx->stream, NULL, in);
    pc->frame_len    = h->mlen;         // 每一帧长度
    pc->frame_header = *h;
    pc->next         = NULL;  
    // ############### 缓存数据
     
    pch = ctx->stream->push_cache_head;
    pct = ctx->stream->push_cache_tail;
    if(pch == NULL){
        ctx->stream->push_cache_head = pc;
        ctx->stream->push_cache_tail = pc;
        ctx->stream->push_cache_count = 1;
    } else {
        if(pct != NULL){
            pct->next = pc;
            ctx->stream->push_cache_tail = pc;
            ctx->stream->push_cache_count += 1;
        } else {
            return NGX_ERROR; 
        }
    }
    
    // DUMP_CACHE start     
    pcp = pch;
    ngx_event_t *ev = &ctx->stream->push_cache_event;
    if( ev && !ev->timer_set && mandatory == 0 && pcp && pc){
	    while( pcp->mandatory == 1 ){
		    pcp = pcp->next;
	    };
        if( !pcp )
            return NGX_OK;
            
	    if( (pcp->frame_type == pc->frame_type) && (pc->frame_pts-pcp->frame_pts >= lacf->push_cache_time_len-10) ){
            ev->handler = nxg_rtmp_live_av_dump_cache;
            ev->log = NULL;
            ev->data = ctx->stream;

            ngx_uint_t relts = ngx_rtmp_get_current_time();
            relts += 10;
            ctx->stream->push_cache_expts = relts;
            
		    ngx_add_timer(ev, 10);
                     
            // RELAY_CACHE
            ngx_rtmp_live_relay_cache_check(ctx->stream);
	    }
    }
	
    return NGX_OK;            
}

// RELAY_CACHE
static void 
nxg_rtmp_live_relay_cache_poll(ngx_event_t *ev)
{
    if( !ev ){
        printf("LLLLL nxg_rtmp_live_relay_cache_poll ev is null\n");
        return;
    }

    ngx_rtmp_live_app_conf_t       *lacf;
    ngx_file_t        file;
    ssize_t           n, tn, cn;
    u_char           tbuf[RELAY_CACHE_FILE_LEN];
    u_char           cbuf[RELAY_CACHE_FILE_LEN];
    
    lacf = ev->data;
    if( !lacf ){
        printf("LLLLL nxg_rtmp_live_relay_cache_poll lacf is null\n");
        return;
    }
    
    // 如果转推关闭 直接退出 
    if( !lacf->relay_cache )
        return;

    // 如果转推开启 则需要判断标志文件
    ngx_memzero(tbuf, sizeof(RELAY_CACHE_FILE_LEN));
    ngx_memzero(cbuf, sizeof(RELAY_CACHE_FILE_LEN));
    ngx_memzero(&file, sizeof(ngx_file_t));
    file.name = lacf->relay_cache_file;
    file.log = NULL;
    if ( file.name.len == 0 )
        return;
    file.fd = ngx_open_file(file.name.data, NGX_FILE_RDONLY,
            NGX_FILE_OPEN, NGX_FILE_DEFAULT_ACCESS);
    if (file.fd == NGX_INVALID_FILE) {
        printf("LLLLL ngx_open_file NGX_INVALID_FILE\n");
        goto next;
    }
    
    n = ngx_read_file(&file, tbuf, RELAY_CACHE_FILE_LEN, 0);
    tn = 0;
    cn = 0;
    // 去除空格
    while( tbuf[tn] != ';' && tn < n ){
        if( tbuf[tn] == '_' || isalpha(tbuf[tn]) )
            cbuf[cn++] = tbuf[tn];
        tn++;
    }
    cbuf[cn] = '\0';
    
    if( cn == 18 && !ngx_strcmp(cbuf, "relay_cache_ctrlon") ){
        lacf->relay_cache_ctrl = 1;
    } else if ( cn == 19 && !ngx_strcmp(cbuf, "relay_cache_ctrloff")) {
        lacf->relay_cache_ctrl = 0;
    }
    
    if (ngx_close_file(file.fd) == NGX_FILE_ERROR) { 
        printf("LLLLL ngx_close_file NGX_FILE_ERROR\n");
        goto next;
    }
next:
    ngx_add_timer(ev, lacf->relay_cache_poll_len);
}

static ngx_int_t
ngx_rtmp_live_av(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
                 ngx_chain_t *in)
{
    ngx_int_t ret = -1;
    
    ngx_rtmp_live_app_conf_t       *lacf;
    ngx_rtmp_live_ctx_t            *ctx;
    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);
    
    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);
    if (ctx == NULL || !ctx->publishing) {
        return ret;   
    }
     
    
    // 推流断开重连事件
    if (ctx->idle_evt.timer_set) {
        ngx_add_timer(&ctx->idle_evt, lacf->idle_timeout);
    }
    
    // RELAY_CACHE timer register 
    if ( lacf->relay_cache ){
        ngx_event_t *ev = &lacf->relay_cache_event;
        if ( !ev->timer_set ){
            //printf("LLLLL init timer set  relay_cache_poll_len:%ld, relay_cache_file:%s\n", lacf->relay_cache_poll_len, lacf->relay_cache_file.data);
            ev->handler = nxg_rtmp_live_relay_cache_poll;
            ev->log = NULL;
            ev->data = lacf;
            ngx_add_timer(ev, lacf->relay_cache_poll_len);
        }
    }
    
     
    if( lacf->push_cache ){
        ret = ngx_rtmp_live_av_to_cache(s, h, in);
    } else {
        ret = ngx_rtmp_live_av_to_net(s, h, in);
    }
    
    
    // STREAM_STATE  publish状态详情（兼容publish断开时显示）
    if( !ctx->stream->codec_ctx.is_init ) {
        ngx_rtmp_stream_publish_header_copy(s, &ctx->stream->codec_ctx, ctx->stream, lacf->push_cache);
    }
    // STREAM_STATE  输入的数据大小    
    ngx_rtmp_update_bandwidth(&ctx->stream->bw_in, h->mlen);
    ngx_rtmp_update_bandwidth(h->type == NGX_RTMP_MSG_AUDIO ?
                              &ctx->stream->bw_in_audio :
                              &ctx->stream->bw_in_video,
                              h->mlen);
    
    return ret;   
}

static ngx_int_t
ngx_rtmp_live_publish(ngx_rtmp_session_t *s, ngx_rtmp_publish_t *v)
{
    //printf("TTTTT ngx_rtmp_live_publish\n");
    ngx_rtmp_live_app_conf_t       *lacf;
    ngx_rtmp_live_ctx_t            *ctx;

    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);

    if (lacf == NULL || !lacf->live) {
        goto next;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "live: publish: name='%s' type='%s'",
                   v->name, v->type);

    /* join stream as publisher */

    ngx_rtmp_live_join(s, v->name, 1);

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);
    if (ctx == NULL || !ctx->publishing) {
        goto next;
    }

    ctx->silent = v->silent;

    if (!ctx->silent) {
        ngx_rtmp_send_status(s, "NetStream.Publish.Start",
                             "status", "Start publishing");
    }
    
    // --------liw
    if( lacf->push_cache ){
        /*
        ctx->stream->session   = s;
        ctx->stream->lacf      = lacf;
        ctx->stream->cscf      = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);
        */
        // 转推相关 
        ctx->stream->app_conf  = s->app_conf; 
        ctx->stream->srv_conf  = s->srv_conf;
        ctx->stream->main_conf = s->main_conf; 
        
        ngx_memcpy(&ctx->stream->publish, v, sizeof(ngx_rtmp_publish_t));
    }
next:
    return next_publish(s, v);
}


static ngx_int_t
ngx_rtmp_live_play(ngx_rtmp_session_t *s, ngx_rtmp_play_t *v)
{
    //printf("TTTTT ngx_rtmp_live_play v->name:%s\n", v->name);
    ngx_rtmp_live_app_conf_t       *lacf;
    ngx_rtmp_live_ctx_t            *ctx;

    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);

    if (lacf == NULL || !lacf->live) {
        goto next;
    }

    ngx_log_debug4(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "live: play: name='%s' start=%uD duration=%uD reset=%d",
                   v->name, (uint32_t) v->start,
                   (uint32_t) v->duration, (uint32_t) v->reset);

    /* join stream as subscriber */

    ngx_rtmp_live_join(s, v->name, 0);

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);
    if (ctx == NULL) {
        goto next;
    }
    
    ctx->silent = v->silent;

    if (!ctx->silent && !lacf->play_restart) {
        ngx_rtmp_send_status(s, "NetStream.Play.Start",
                             "status", "Start live");
        ngx_rtmp_send_sample_access(s);
    }

next:
    return next_play(s, v);
}


static ngx_int_t
ngx_rtmp_live_postconfiguration(ngx_conf_t *cf)
{
    ngx_rtmp_core_main_conf_t          *cmcf;
    ngx_rtmp_handler_pt                *h;

    cmcf = ngx_rtmp_conf_get_module_main_conf(cf, ngx_rtmp_core_module);

    /* register raw event handlers */

    h = ngx_array_push(&cmcf->events[NGX_RTMP_MSG_AUDIO]);
    *h = ngx_rtmp_live_av;

    h = ngx_array_push(&cmcf->events[NGX_RTMP_MSG_VIDEO]);
    *h = ngx_rtmp_live_av;

    /* chain handlers */

    next_publish = ngx_rtmp_publish;
    ngx_rtmp_publish = ngx_rtmp_live_publish;

    next_play = ngx_rtmp_play;
    ngx_rtmp_play = ngx_rtmp_live_play;

    next_close_stream = ngx_rtmp_close_stream;
    ngx_rtmp_close_stream = ngx_rtmp_live_close_stream;

    next_pause = ngx_rtmp_pause;
    ngx_rtmp_pause = ngx_rtmp_live_pause;

    next_stream_begin = ngx_rtmp_stream_begin;
    ngx_rtmp_stream_begin = ngx_rtmp_live_stream_begin;

    next_stream_eof = ngx_rtmp_stream_eof;
    ngx_rtmp_stream_eof = ngx_rtmp_live_stream_eof;

    return NGX_OK;
}

// 申请帧内存空间 (先从空闲队列中申请，没有才能从内存中申请)
static ngx_rtmp_live_push_cache_t *
ngx_rtmp_alloc_push_cache_frame(ngx_rtmp_live_stream_t *stream)
{
    //printf("LLLLL ngx_rtmp_alloc_push_cache_frame\n");
    //if ( !stream || !stream->pool )
    if ( !stream )
        return NULL;
      
    ngx_rtmp_live_push_cache_t  *out, *pch;
    out = pch = stream->idle_cache_head;
    // 如果队列头 不为空则取队头返回, 否则就申请内存
    if( pch != NULL ){
        // 移动头到下一个
        if(pch->next != NULL){
            stream->idle_cache_head = pch->next;
            stream->idle_cache_count -= 1;
        }
        // 如果队列已经为空，需要把对头队尾指针也置为空
        else{
            stream->idle_cache_head = NULL;
            stream->idle_cache_tail = NULL;
            stream->idle_cache_count = 0;
        }
    } else {
        out =  ngx_pcalloc(stream->pool, sizeof(ngx_rtmp_live_push_cache_t));
        if( out != NULL ) {
            out->frame_type = -1;
            out->frame_len  = 0;
            out->frame_pts  = 0;
            out->frame_flag = 0;
            out->frame_buf  = NULL;
            //out->frame_header = NULL;
            out->next = NULL;
        }
    }
    return out;
}
// 归还帧内存空间 （帧内存归还给空闲队列， buffer内存归还给connection的pool）
static void 
ngx_rtmp_free_push_cache_frame(ngx_rtmp_live_stream_t *stream, ngx_rtmp_live_push_cache_t *pc)
{
    // 追加空闲队列
    ngx_rtmp_live_push_cache_t  *pch;
    pch = stream->idle_cache_head;
    pc->next = NULL;
    if( pch == NULL ){
        stream->idle_cache_head = pc;
        stream->idle_cache_tail = pc;
        stream->idle_cache_count = 1;
    } else {
        stream->idle_cache_tail->next = pc;
        stream->idle_cache_tail = pc;
        stream->idle_cache_count += 1;
    }
    
    ngx_rtmp_free_frame_buffer(stream, pc->frame_buf);

    return;
}

// 从connection->pool 中申请内存
//pc->frame_buf = ngx_rtmp_append_shared_bufs(cscf, NULL, in); // 缓冲数据 
ngx_chain_t *
ngx_rtmp_alloc_frame_buffer(size_t chunk_size, ngx_rtmp_live_stream_t *stream){
    u_char                     *p;
    ngx_chain_t                *out;
    ngx_buf_t                  *b;
    size_t                      size;
    
    if(stream->free){
        out = stream->free;
        stream->free = out->next;
    } else {
        size = chunk_size + NGX_RTMP_MAX_CHUNK_HEADER;
        p = ngx_pcalloc(stream->pool, NGX_RTMP_REFCOUNT_BYTES
                + sizeof(ngx_chain_t)
                + sizeof(ngx_buf_t)
                + size);
        if(p == NULL)
            return NULL;

        // 是个什么东西？
        p += NGX_RTMP_REFCOUNT_BYTES;
        out = (ngx_chain_t *)p;

        p += sizeof(ngx_chain_t);
        out->buf = (ngx_buf_t *)p;

        p += sizeof(ngx_buf_t);
        out->buf->start = p;
        out->buf->end = p+size;
    }
    
    out->next = NULL;
    b = out->buf;
    b->pos = b->last = b->start + NGX_RTMP_MAX_CHUNK_HEADER;
    b->memory = 1;
    
    // 不知道什么作用  貌似是一个宏
    ngx_rtmp_ref_set(out, 1);
    return out;
}

// 第一个参数申请内存空间使用
// 第三个参数为以后兼容
// 第三个参数为数据
ngx_chain_t *
ngx_rtmp_append_data_to_push_cache(size_t chunk_size, ngx_rtmp_live_stream_t *stream, ngx_chain_t *head, ngx_chain_t *in){
    ngx_chain_t                    *l, **ll;
    u_char                         *p;
    size_t                          size;
    
    ll = &head;
    l = head;
    p = in->buf->pos;
    for(;;){
        if(l == NULL || l->buf->last == l->buf->end){
            l = ngx_rtmp_alloc_frame_buffer(chunk_size, stream);
            if(l == NULL || l->buf == NULL)
                break;
            
            *ll = l;
            ll = &l->next;
        }   
        
        while( (l->buf->end - l->buf->last) >= (in->buf->last -p) ){
            l->buf->last = ngx_cpymem(l->buf->last, p, in->buf->last - p);
            in = in->next;
            if(in == NULL){
                goto done;
            }
            p = in->buf->pos;
        }    
        
        size = l->buf->end - l->buf->last;
        l->buf->last = ngx_cpymem(l->buf->last, p, size);
        p += size;
    }
done:
    ll = NULL;
    
    return head;
}

void 
ngx_rtmp_free_frame_buffer(ngx_rtmp_live_stream_t *stream, ngx_chain_t *in){
    ngx_chain_t        *cl;

    if (ngx_rtmp_ref_put(in)) {
        return;
    }
    
    for (cl = in; ; cl = cl->next) {
        if (cl->next == NULL) {
            cl->next = stream->free;
            stream->free = in;
            return;
        }
    }
}




// RELAY_CACHE
static char *
ngx_rtmp_stream_relay_push_pull(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                          *value, v, n;
    //-- ngx_rtmp_relay_target_t            *target, **t;
    ngx_rtmp_live_app_conf_t          *racf;
    ngx_rtmp_relay_target_t            *target, **t;
    ngx_url_t                          *u;
    ngx_uint_t                          i;
    ngx_int_t                           is_pull, is_static;
    ngx_event_t                       **ee, *e;
    ngx_rtmp_relay_static_t            *rs;
    u_char                             *p;

    value = cf->args->elts;

    racf = ngx_rtmp_conf_get_module_app_conf(cf, ngx_rtmp_live_module);

    is_pull = (value[0].data[3] == 'l');
    is_static = 0;

    target = ngx_pcalloc(cf->pool, sizeof(*target));
    if (target == NULL) {
        return NGX_CONF_ERROR;
    }

    target->tag = &ngx_rtmp_relay_module;
    target->data = target;

    u = &target->url;
    u->default_port = 1935;
    u->uri_part = 1;
    u->url = value[1];

    if (ngx_strncasecmp(u->url.data, (u_char *) "rtmp://", 7) == 0) {
        u->url.data += 7;
        u->url.len  -= 7;
    }

    if (ngx_parse_url(cf->pool, u) != NGX_OK) {
        if (u->err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "%s in url \"%V\"", u->err, &u->url);
        }
        return NGX_CONF_ERROR;
    }
    
    value += 2;
    for (i = 2; i < cf->args->nelts; ++i, ++value) {
        p = ngx_strlchr(value->data, value->data + value->len, '=');

        if (p == NULL) {
            n = *value;
            ngx_str_set(&v, "1");

        } else {
            n.data = value->data;
            n.len  = p - value->data;

            v.data = p + 1;
            v.len  = value->data + value->len - p - 1;
        }

#define NGX_RTMP_RELAY_STR_PAR(name, var)                                     \
        if (n.len == sizeof(name) - 1                                         \
                && ngx_strncasecmp(n.data, (u_char *) name, n.len) == 0)          \
        {                                                                     \
            target->var = v;                                                  \
            continue;                                                         \
        }

#define NGX_RTMP_RELAY_NUM_PAR(name, var)                                     \
        if (n.len == sizeof(name) - 1                                         \
                && ngx_strncasecmp(n.data, (u_char *) name, n.len) == 0)          \
        {                                                                     \
            target->var = ngx_atoi(v.data, v.len);                            \
            continue;                                                         \
        }

        NGX_RTMP_RELAY_STR_PAR("app",         app);
        NGX_RTMP_RELAY_STR_PAR("name",        name);
        NGX_RTMP_RELAY_STR_PAR("tcUrl",       tc_url);
        NGX_RTMP_RELAY_STR_PAR("pageUrl",     page_url);
        NGX_RTMP_RELAY_STR_PAR("swfUrl",      swf_url);
        NGX_RTMP_RELAY_STR_PAR("flashVer",    flash_ver);
        NGX_RTMP_RELAY_STR_PAR("playPath",    play_path);
        NGX_RTMP_RELAY_NUM_PAR("live",        live);
        NGX_RTMP_RELAY_NUM_PAR("start",       start);
        NGX_RTMP_RELAY_NUM_PAR("stop",        stop);

#undef NGX_RTMP_RELAY_STR_PAR
#undef NGX_RTMP_RELAY_NUM_PAR

        if (n.len == sizeof("static") - 1 &&
                ngx_strncasecmp(n.data, (u_char *) "static", n.len) == 0 &&
                ngx_atoi(v.data, v.len))
        {
            is_static = 1;
            continue;
        }
        return "unsuppored parameter";
    }

    if (is_static) {

        if (!is_pull) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "static push is not allowed");
            return NGX_CONF_ERROR;
        }

        if (target->name.len == 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "stream name missing in static pull "
                    "declaration");
            return NGX_CONF_ERROR;
        }

        ee = ngx_array_push(&racf->static_events);
        if (ee == NULL) {
            return NGX_CONF_ERROR;
        }

        e = ngx_pcalloc(cf->pool, sizeof(ngx_event_t));
        if (e == NULL) {
            return NGX_CONF_ERROR;
        }

        *ee = e;

        rs = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_relay_static_t));
        if (rs == NULL) {
            return NGX_CONF_ERROR;
        }

        rs->target = target;

        e->data = rs;
        e->log = &cf->cycle->new_log;
        e->handler = ngx_rtmp_relay_static_pull_reconnect;

        t = ngx_array_push(&racf->static_pulls);

    } else if (is_pull) {
        t = ngx_array_push(&racf->pulls);

    } else {
        t = ngx_array_push(&racf->pushes);
    }
    if (t == NULL) {
        return NGX_CONF_ERROR;
    }

    *t = target;

    return NGX_CONF_OK;
}
        
        
        
static ngx_int_t
ngx_rtmp_stream_relay_copy_str(ngx_pool_t *pool, ngx_str_t *dst, ngx_str_t *src)
{
    if (src->len == 0) {
        return NGX_OK;
    }
    dst->len = src->len;
    dst->data = ngx_palloc(pool, src->len);
    if (dst->data == NULL) {
        return NGX_ERROR;
    }
    ngx_memcpy(dst->data, src->data, src->len);
    return NGX_OK;
}


typedef ngx_rtmp_relay_ctx_t * (* ngx_rtmp_stream_relay_create_ctx_pt)
        (ngx_rtmp_live_stream_t *stream, ngx_rtmp_session_t *s, ngx_str_t *name, ngx_rtmp_relay_target_t *target);

static ngx_int_t
ngx_rtmp_stream_relay_create(ngx_rtmp_live_stream_t *stream, ngx_str_t *name,
        ngx_rtmp_relay_target_t *target,
        ngx_rtmp_stream_relay_create_ctx_pt create_publish_ctx,
        ngx_rtmp_stream_relay_create_ctx_pt create_play_ctx)
{
    ngx_rtmp_relay_ctx_t            *play_ctx, *publish_ctx;

    play_ctx = create_play_ctx(stream, stream->session, name, target);
    if (play_ctx == NULL) {
        return NGX_ERROR;
    }
    
    if ( stream->relay_ctx ) {
        play_ctx->publish       = stream->relay_ctx->publish;
        play_ctx->next          = stream->relay_ctx->play;
        stream->relay_ctx->play = play_ctx;
        return NGX_OK;
    }
    
    publish_ctx = create_publish_ctx(stream, stream->session, name, target);
    if ( publish_ctx == NULL ) {
        ngx_rtmp_finalize_session(play_ctx->session);
        return NGX_ERROR;
    }
    
    publish_ctx->publish = publish_ctx;
    publish_ctx->play    = play_ctx;
    play_ctx->publish    = publish_ctx;
    stream->relay_ctx    = publish_ctx;
    return NGX_OK;
}

static void
ngx_rtmp_stream_relay_push_reconnect(ngx_event_t *ev);

static ngx_rtmp_relay_ctx_t *
ngx_rtmp_stream_relay_create_local_ctx(ngx_rtmp_live_stream_t *stream,ngx_rtmp_session_t *s, ngx_str_t *name,
        ngx_rtmp_relay_target_t *target)
{
    ngx_rtmp_relay_ctx_t           *ctx;

    ctx = stream->relay_ctx;
     
    if (ctx == NULL) {
        ctx = ngx_pcalloc(stream->pool, sizeof(ngx_rtmp_relay_ctx_t));
        if (ctx == NULL) {
            return NULL;
        }
        //ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_relay_module);
    }

    ctx->session = NULL;
    ctx->push_evt.data = stream;
    ctx->push_evt.log = NULL;
    ctx->push_evt.handler = ngx_rtmp_stream_relay_push_reconnect;

    if (ctx->publish) {
        return NULL;
    }

    if (ngx_rtmp_stream_relay_copy_str(stream->pool, &ctx->name, name)
            != NGX_OK)
    {
        return NULL;
    }

    return ctx;
}

static ngx_rtmp_relay_ctx_t *
ngx_rtmp_stream_relay_create_remote_ctx(ngx_rtmp_live_stream_t *stream, ngx_rtmp_session_t *s, ngx_str_t* name, ngx_rtmp_relay_target_t *target)
{
    ngx_rtmp_conf_ctx_t         cctx;
    /*
    cctx.app_conf = s->app_conf;
    cctx.srv_conf = s->srv_conf;
    cctx.main_conf = s->main_conf;
    */
    cctx.app_conf = stream->app_conf;
    cctx.srv_conf = stream->srv_conf;
    cctx.main_conf = stream->main_conf;
    return ngx_rtmp_relay_create_connection(&cctx, name, target);
}

static ngx_int_t 
ngx_rtmp_stream_relay_push(ngx_rtmp_live_stream_t *stream, ngx_str_t *name, ngx_rtmp_relay_target_t *target)
{
    return ngx_rtmp_stream_relay_create(stream, name, target,
            ngx_rtmp_stream_relay_create_local_ctx,
            ngx_rtmp_stream_relay_create_remote_ctx);
}

static void
ngx_rtmp_stream_relay_push_reconnect(ngx_event_t *ev)
{
    //printf("LLLLL ngx_rtmp_stream_relay_push_reconnect\n");
    
    ngx_rtmp_live_app_conf_t      *lacf;
    ngx_rtmp_relay_ctx_t           *ctx, *pctx;
    ngx_uint_t                      n;
    ngx_rtmp_relay_target_t        *target, **t;
    ngx_rtmp_relay_reconnect_t     *rrs, **prrs;  

    ngx_rtmp_live_stream_t *stream = ev->data;
    
    lacf = stream->lacf;
    
    ctx = stream->relay_ctx;
    if (ctx == NULL) {
        return;
    }

    t = lacf->pushes.elts;
    // 多个 app 
    for (n = 0; n < lacf->pushes.nelts; ++n, ++t) {
        target = *t;
        if (target->name.len && (ctx->name.len != target->name.len ||
                    ngx_memcmp(ctx->name.data, target->name.data, ctx->name.len)))
        {
            continue;
        }

        // is connecting 
        for (pctx = ctx->play; pctx; pctx = pctx->next) {
             
            if (pctx->tag == &ngx_rtmp_relay_module &&
                    pctx->data == target)
            {
                break;
            }
        }

        if (pctx) {
            continue;
        }
        
        prrs = &stream->relay_reconnects;  
        for( ; *prrs ; prrs = &(*prrs)->next ){
            if ( (*prrs)->target == (ngx_uint_t)target ){
                (*prrs)->nreconnects += 1;
                break;
            }
        }
        if( !(*prrs) ){
            rrs = ngx_pnalloc(stream->pool, sizeof(ngx_rtmp_relay_reconnect_t)); 
            if( rrs ){
                rrs->target      = (ngx_uint_t)target;
                rrs->nreconnects = 1;
                rrs->next        = NULL;
                *prrs            = rrs;
            }
        }
            
        //在这里找到对应记录链接的转发次数
        if (ngx_rtmp_stream_relay_push(stream, &ctx->name, target) == NGX_OK) {
            continue;
        }

        if (!ctx->push_evt.timer_set) {
            ngx_add_timer(&ctx->push_evt, lacf->stream_push_reconnect);
        }
    }
}

//-- static ngx_int_t 
static void
ngx_rtmp_stream_relay_publish(ngx_rtmp_live_stream_t *stream, ngx_rtmp_publish_t *v)
{
    //printf("LLLLL ngx_rtmp_stream_relay_publish\n");
    ngx_rtmp_live_app_conf_t      *lacf;
    ngx_rtmp_relay_target_t        *target, **t;
    ngx_str_t                       name;
    size_t                          n;

    ngx_rtmp_relay_ctx_t     *ctx;

    if( !stream )
        return;
    
    // RELAY_CACHE 改变缓存转推的状态  
    stream->is_relay_start = 1;
    lacf =  stream->lacf;
    
    if (lacf == NULL || lacf->pushes.nelts == 0) {
        goto next;
    }
    
    if( stream->relay_ctx )
        goto next;
    
    name.len = ngx_strlen(v->name);
    name.data = v->name;
    t = lacf->pushes.elts;
    for (n = 0; n < lacf->pushes.nelts; ++n, ++t) {
        target = *t;
        
        if (target->name.len && (name.len != target->name.len ||
                    ngx_memcmp(name.data, target->name.data, name.len)))
        {
            continue;
        }
        
        if (ngx_rtmp_stream_relay_push(stream, &name, target) == NGX_OK) {
            continue;
        }

        ctx = stream->relay_ctx;
        if (ctx && !ctx->push_evt.timer_set) {
            ngx_add_timer(&ctx->push_evt, lacf->stream_push_reconnect);
        }
    }

next:
    return;
}

static void ngx_rtmp_stream_relay_close(ngx_rtmp_live_stream_t *stream)
{
    ngx_rtmp_relay_ctx_t              *ctx, **cctx;
    if( !stream )
        return;
    // RELAY_CACHE 改变缓存转推的状态  
    stream->is_relay_start = 0;

    ctx = stream->relay_ctx;
    if (ctx == NULL) {
        return;
    }
    
    if (ctx->publish == NULL) {
        return;
    }
    
    if (ctx->push_evt.timer_set) {
        ngx_del_timer(&ctx->push_evt);
    }

    for (cctx = &ctx->play; *cctx; cctx = &(*cctx)->next) {
        (*cctx)->publish = NULL;
        if((*cctx)->session){
            ngx_rtmp_finalize_session((*cctx)->session);
            (*cctx)->session = NULL;
        }
    }
    ctx->publish = NULL;
    stream->relay_ctx = NULL;
}

// RELAY_CACHE #######


// 获取当前时间戳
static ngx_uint_t 
ngx_rtmp_get_current_time(){
    time_t sec;
    ngx_uint_t msec;
    ngx_uint_t curt;
    struct timeval tv;
    ngx_gettimeofday(&tv);
    
    sec = tv.tv_sec;
    msec = tv.tv_usec/1000;
    curt = (ngx_msec_t)sec*1000 + msec;
    //printf("####sec:%ld msec:%ld curt:%ld\n", sec, msec, curt);
    
    return curt;
}




