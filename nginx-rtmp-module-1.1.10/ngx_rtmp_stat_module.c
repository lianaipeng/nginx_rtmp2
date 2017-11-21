
/*
 * Copyright (C) Roman Arutyunyan
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>
#include "ngx_rtmp.h"
#include "ngx_rtmp_version.h"
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_play_module.h"
#include "ngx_rtmp_codec_module.h"
#include "ngx_rtmp_bitop.h"


static ngx_int_t ngx_rtmp_stat_init_process(ngx_cycle_t *cycle);
static char *ngx_rtmp_stat(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_rtmp_stat_postconfiguration(ngx_conf_t *cf);
static void * ngx_rtmp_stat_create_loc_conf(ngx_conf_t *cf);
static char * ngx_rtmp_stat_merge_loc_conf(ngx_conf_t *cf,
        void *parent, void *child);


static time_t                       start_time;


#define NGX_RTMP_STAT_ALL           0xff
#define NGX_RTMP_STAT_GLOBAL        0x01
#define NGX_RTMP_STAT_LIVE          0x02
#define NGX_RTMP_STAT_CLIENTS       0x04
#define NGX_RTMP_STAT_PLAY          0x08

/*
 * global: stat-{bufs-{total,free,used}, total bytes in/out, bw in/out} - cscf
*/


typedef struct {
    ngx_uint_t                      stat;
    ngx_str_t                       stylesheet;
} ngx_rtmp_stat_loc_conf_t;


static ngx_conf_bitmask_t           ngx_rtmp_stat_masks[] = {
    { ngx_string("all"),            NGX_RTMP_STAT_ALL           },
    { ngx_string("global"),         NGX_RTMP_STAT_GLOBAL        },
    { ngx_string("live"),           NGX_RTMP_STAT_LIVE          },
    { ngx_string("clients"),        NGX_RTMP_STAT_CLIENTS       },
    { ngx_null_string,              0 }
};


static ngx_command_t  ngx_rtmp_stat_commands[] = {

    { ngx_string("rtmp_stat"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
        ngx_rtmp_stat,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_rtmp_stat_loc_conf_t, stat),
        ngx_rtmp_stat_masks },

    { ngx_string("rtmp_stat_stylesheet"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_rtmp_stat_loc_conf_t, stylesheet),
        NULL },

    ngx_null_command
};


static ngx_http_module_t  ngx_rtmp_stat_module_ctx = {
    NULL,                               /* preconfiguration */
    ngx_rtmp_stat_postconfiguration,    /* postconfiguration */

    NULL,                               /* create main configuration */
    NULL,                               /* init main configuration */

    NULL,                               /* create server configuration */
    NULL,                               /* merge server configuration */

    ngx_rtmp_stat_create_loc_conf,      /* create location configuration */
    ngx_rtmp_stat_merge_loc_conf,       /* merge location configuration */
};


ngx_module_t  ngx_rtmp_stat_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_stat_module_ctx,          /* module context */
    ngx_rtmp_stat_commands,             /* module directives */
    NGX_HTTP_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    ngx_rtmp_stat_init_process,         /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};


#define NGX_RTMP_STAT_BUFSIZE           256


static ngx_int_t
ngx_rtmp_stat_init_process(ngx_cycle_t *cycle)
{
    //printf("SSSSS ngx_rtmp_stat_init_process\n");
    /*
     * HTTP process initializer is called
     * after event module initializer
     * so we can run posted events here
     */

    ngx_event_process_posted(cycle, &ngx_rtmp_init_queue);

    return NGX_OK;
}


/* ngx_escape_html does not escape characters out of ASCII range
 * which are bad for xslt */

static void *
ngx_rtmp_stat_escape(ngx_http_request_t *r, void *data, size_t len)
{
    //printf("SSSSS ngx_rtmp_stat_escape\n");
    u_char *p, *np;
    void   *new_data;
    size_t  n;

    p = data;

    for (n = 0; n < len; ++n, ++p) {
        if (*p < 0x20 || *p >= 0x7f) {
            break;
        }
    }

    if (n == len) {
        return data;
    }

    new_data = ngx_palloc(r->pool, len);
    if (new_data == NULL) {
        return NULL;
    }

    p  = data;
    np = new_data;

    for (n = 0; n < len; ++n, ++p, ++np) {
        *np = (*p < 0x20 || *p >= 0x7f) ? (u_char) ' ' : *p;
    }

    return new_data;
}

#if (NGX_WIN32)
/*
 * Fix broken MSVC memcpy optimization for 4-byte data
 * when this function is inlined
 */
__declspec(noinline)
#endif

static void
ngx_rtmp_stat_output(ngx_http_request_t *r, ngx_chain_t ***lll,
        void *data, size_t len, ngx_uint_t escape)
{
    //printf("SSSSS ngx_rtmp_stat_output\n");
    ngx_chain_t        *cl;
    ngx_buf_t          *b;
    size_t              real_len;

    if (len == 0) {
        return;
    }

    if (escape) {
        data = ngx_rtmp_stat_escape(r, data, len);
        if (data == NULL) {
            return;
        }
    }

    real_len = escape
        ? len + ngx_escape_html(NULL, data, len)
        : len;

    cl = **lll;
    if (cl && cl->buf->last + real_len > cl->buf->end) {
        *lll = &cl->next;
    }

    if (**lll == NULL) {
        cl = ngx_alloc_chain_link(r->pool);
        if (cl == NULL) {
            return;
        }
        b = ngx_create_temp_buf(r->pool,
                ngx_max(NGX_RTMP_STAT_BUFSIZE, real_len));
        if (b == NULL || b->pos == NULL) {
            return;
        }
        cl->next = NULL;
        cl->buf = b;
        **lll = cl;
    }

    b = (**lll)->buf;

    if (escape) {
        b->last = (u_char *)ngx_escape_html(b->last, data, len);
    } else {
        b->last = ngx_cpymem(b->last, data, len);
    }
}
/*
static void
ngx_rtmp_stat_output_del(ngx_http_request_t *r, ngx_chain_t ***lll)
{
    // 需要删除一个 ","
    ngx_buf_t *b = (**lll)->buf;
    b->last -= 1; 
}
*/


/* These shortcuts assume 2 variables exist in current context:
 *   ngx_http_request_t    *r
 *   ngx_chain_t         ***lll */

/* plain data */
#define NGX_RTMP_STAT(data, len)    ngx_rtmp_stat_output(r, lll, data, len, 0)

/* escaped data */
#define NGX_RTMP_STAT_E(data, len)  ngx_rtmp_stat_output(r, lll, data, len, 1)

/* literal */
#define NGX_RTMP_STAT_L(s)          NGX_RTMP_STAT((s), sizeof(s) - 1)

/* ngx_str_t */
#define NGX_RTMP_STAT_S(s)          NGX_RTMP_STAT((s)->data, (s)->len)

/* escaped ngx_str_t */
#define NGX_RTMP_STAT_ES(s)         NGX_RTMP_STAT_E((s)->data, (s)->len)

/* C string */
#define NGX_RTMP_STAT_CS(s)         NGX_RTMP_STAT((s), ngx_strlen(s))

/* escaped C string */
#define NGX_RTMP_STAT_ECS(s)        NGX_RTMP_STAT_E((s), ngx_strlen(s))

/* delete the last ","*/
//#define NGX_RTMP_STAT_DEL()       ngx_rtmp_stat_output_del(r, lll) 


#define NGX_RTMP_STAT_BW            0x01
#define NGX_RTMP_STAT_BYTES         0x02
#define NGX_RTMP_STAT_BW_BYTES      0x03


static void
ngx_rtmp_stat_bw(ngx_http_request_t *r, ngx_chain_t ***lll,
                 ngx_rtmp_bandwidth_t *bw, char *name,
                 ngx_uint_t flags)
{
    //printf("SSSSS ngx_rtmp_stat_bw name:%s\n", name);
    u_char  buf[NGX_INT64_LEN + 9];

    ngx_rtmp_update_bandwidth(bw, 0);

    if (flags & NGX_RTMP_STAT_BW) {
        NGX_RTMP_STAT_L("<bw_");
        NGX_RTMP_STAT_CS(name);
        NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), ">%uL</bw_",
                                        bw->bandwidth * 8)
                           - buf);
        NGX_RTMP_STAT_CS(name);
        NGX_RTMP_STAT_L(">\r\n");
    }

    if (flags & NGX_RTMP_STAT_BYTES) {
        NGX_RTMP_STAT_L("<bytes_");
        NGX_RTMP_STAT_CS(name);
        NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), ">%uL</bytes_",
                                        bw->bytes)
                           - buf);
        NGX_RTMP_STAT_CS(name);
        NGX_RTMP_STAT_L(">\r\n");
    }
}


#ifdef NGX_RTMP_POOL_DEBUG
static void
ngx_rtmp_stat_get_pool_size(ngx_pool_t *pool, ngx_uint_t *nlarge,
        ngx_uint_t *size)
{
    //printf("SSSSS ngx_rtmp_stat_get_pool_size\n");
    ngx_pool_large_t       *l;
    ngx_pool_t             *p, *n;

    *nlarge = 0;
    for (l = pool->large; l; l = l->next) {
        ++*nlarge;
    }

    *size = 0;
    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
        *size += (p->d.last - (u_char *)p);
        if (n == NULL) {
            break;
        }
    }
}


static void
ngx_rtmp_stat_dump_pool(ngx_http_request_t *r, ngx_chain_t ***lll,
        ngx_pool_t *pool)
{
    //printf("SSSSS ngx_rtmp_stat_dump_pool\n");
    ngx_uint_t  nlarge, size;
    u_char      buf[NGX_INT_T_LEN];

    size = 0;
    nlarge = 0;
    ngx_rtmp_stat_get_pool_size(pool, &nlarge, &size);
    NGX_RTMP_STAT_L("<pool><nlarge>");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui", nlarge) - buf);
    NGX_RTMP_STAT_L("</nlarge><size>");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui", size) - buf);
    NGX_RTMP_STAT_L("</size></pool>\r\n");
}
#endif



static void
ngx_rtmp_stat_client(ngx_http_request_t *r, ngx_chain_t ***lll,
    ngx_rtmp_session_t *s)
{
    //printf("SSSSS ngx_rtmp_stat_client\n");
    u_char  buf[NGX_INT_T_LEN];

#ifdef NGX_RTMP_POOL_DEBUG
    ngx_rtmp_stat_dump_pool(r, lll, s->connection->pool);
#endif
    NGX_RTMP_STAT_L("<id>");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui",
                  (ngx_uint_t) s->connection->number) - buf);
    NGX_RTMP_STAT_L("</id>");

    NGX_RTMP_STAT_L("<address>");
    NGX_RTMP_STAT_ES(&s->connection->addr_text);
    NGX_RTMP_STAT_L("</address>");

    NGX_RTMP_STAT_L("<time>");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%i",
                  (ngx_int_t) (ngx_current_msec - s->epoch)) - buf);
    NGX_RTMP_STAT_L("</time>");

    if (s->flashver.len) {
        NGX_RTMP_STAT_L("<flashver>");
        NGX_RTMP_STAT_ES(&s->flashver);
        NGX_RTMP_STAT_L("</flashver>");
    }

    if (s->page_url.len) {
        NGX_RTMP_STAT_L("<pageurl>");
        NGX_RTMP_STAT_ES(&s->page_url);
        NGX_RTMP_STAT_L("</pageurl>");
    }

    if (s->swf_url.len) {
        NGX_RTMP_STAT_L("<swfurl>");
        NGX_RTMP_STAT_ES(&s->swf_url);
        NGX_RTMP_STAT_L("</swfurl>");
    }
    /*
    if (s->tc_url.len) {
        NGX_RTMP_STAT_L("<tcurl>");
        NGX_RTMP_STAT_ES(&s->tc_url);
        NGX_RTMP_STAT_L("</tcurl>");
    }
    */


}
static void
ngx_rtmp_stat_publish(ngx_http_request_t *r, ngx_chain_t ***lll,
    ngx_rtmp_stream_codec_ctx_t *codec_ctx)
{
    u_char  buf[NGX_INT_T_LEN];

    NGX_RTMP_STAT_L("<id>");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui",
                  (ngx_uint_t) codec_ctx->number) - buf);
    NGX_RTMP_STAT_L("</id>");

    NGX_RTMP_STAT_L("<address>");
    NGX_RTMP_STAT_ES(&codec_ctx->addr_text);
    NGX_RTMP_STAT_L("</address>");

    NGX_RTMP_STAT_L("<time>");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%i",
                  (ngx_int_t) (ngx_current_msec - codec_ctx->epoch)) - buf);
    NGX_RTMP_STAT_L("</time>");

    if (codec_ctx->flashver.len) {
        NGX_RTMP_STAT_L("<flashver>");
        NGX_RTMP_STAT_ES(&codec_ctx->flashver);
        NGX_RTMP_STAT_L("</flashver>");
    }

    if (codec_ctx->page_url.len) {
        NGX_RTMP_STAT_L("<pageurl>");
        NGX_RTMP_STAT_ES(&codec_ctx->page_url);
        NGX_RTMP_STAT_L("</pageurl>");
    }

    if (codec_ctx->swf_url.len) {
        NGX_RTMP_STAT_L("<swfurl>");
        NGX_RTMP_STAT_ES(&codec_ctx->swf_url);
        NGX_RTMP_STAT_L("</swfurl>");
    }
    if (codec_ctx->tc_url.len) {
        NGX_RTMP_STAT_L("<tcurl>");
        NGX_RTMP_STAT_ES(&codec_ctx->tc_url);
        NGX_RTMP_STAT_L("</tcurl>");
    }
}

static void
ngx_rtmp_stat_publish_json(ngx_http_request_t *r, ngx_chain_t ***lll,
    ngx_rtmp_stream_codec_ctx_t *codec_ctx)
{
    u_char  buf[NGX_INT_T_LEN];

    NGX_RTMP_STAT_L("\"id\":\"");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui",
                  (ngx_uint_t) codec_ctx->number) - buf);
    NGX_RTMP_STAT_L("\"");

    NGX_RTMP_STAT_L(",\"address\":\"");
    NGX_RTMP_STAT_ES(&codec_ctx->addr_text);
    NGX_RTMP_STAT_L("\"");

    NGX_RTMP_STAT_L(",\"time\":");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%i",
                  (ngx_int_t) (ngx_current_msec - codec_ctx->epoch)) - buf);

    if (codec_ctx->flashver.len) {
        NGX_RTMP_STAT_L(",\"flashver\":\"");
        NGX_RTMP_STAT_ES(&codec_ctx->flashver);
        NGX_RTMP_STAT_L("\"");
    }

    if (codec_ctx->page_url.len) {
        NGX_RTMP_STAT_L(",\"pageurl\":\"");
        NGX_RTMP_STAT_ES(&codec_ctx->page_url);
        NGX_RTMP_STAT_L("\"");
    }

    if (codec_ctx->swf_url.len) {
        NGX_RTMP_STAT_L(",\"swfurl\":\"");
        NGX_RTMP_STAT_ES(&codec_ctx->swf_url);
        NGX_RTMP_STAT_L("\"");
    }
    if (codec_ctx->tc_url.len) {
        NGX_RTMP_STAT_L(",\"tcurl\":\"");
        NGX_RTMP_STAT_ES(&codec_ctx->tc_url);
        NGX_RTMP_STAT_L("\"");
    }
}
static void
ngx_rtmp_stat_client_json(ngx_http_request_t *r, ngx_chain_t ***lll,
    ngx_rtmp_session_t *s)
{
    u_char  buf[NGX_INT_T_LEN];

#ifdef NGX_RTMP_POOL_DEBUG
    ngx_rtmp_stat_dump_pool(r, lll, s->connection->pool);
#endif
    NGX_RTMP_STAT_L("\"id\":\"");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui",
                  (ngx_uint_t) s->connection->number) - buf);
    NGX_RTMP_STAT_L("\"");

    NGX_RTMP_STAT_L(",\"address\":\"");
    NGX_RTMP_STAT_ES(&s->connection->addr_text);
    NGX_RTMP_STAT_L("\"");

    NGX_RTMP_STAT_L(",\"time\":");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%i",
                  (ngx_int_t) (ngx_current_msec - s->epoch)) - buf);

    if (s->flashver.len) {
        NGX_RTMP_STAT_L(",\"flashver\":\"");
        NGX_RTMP_STAT_ES(&s->flashver);
        NGX_RTMP_STAT_L("\"");
    }

    if (s->page_url.len) {
        NGX_RTMP_STAT_L(",\"pageurl\":\"");
        NGX_RTMP_STAT_ES(&s->page_url);
        NGX_RTMP_STAT_L("\"");
    }

    if (s->swf_url.len) {
        NGX_RTMP_STAT_L(",\"swfurl\":\"");
        NGX_RTMP_STAT_ES(&s->swf_url);
        NGX_RTMP_STAT_L("\"");
    }
    /* 
    if (s->tc_url.len) {
        NGX_RTMP_STAT_L(",\"tcurl\":\"");
        NGX_RTMP_STAT_ES(&s->tc_url);
        NGX_RTMP_STAT_L("\"");
    }
    */
}
static void
ngx_rtmp_stat_push_cache_json(ngx_http_request_t *r, ngx_chain_t ***lll,
        ngx_rtmp_live_stream_t *stream)
{
    u_char  buf[NGX_INT_T_LEN];
    ngx_rtmp_live_push_cache_t  *pch, *pct, *pc; 
    ngx_uint_t          cache_len, cache_alen, cache_vlen;
    
    NGX_RTMP_STAT_L("\"cache_count\":");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui",
                  (ngx_uint_t) stream->push_cache_count) - buf);
    
    pch = stream->push_cache_head;
    pct = stream->push_cache_tail;
    cache_len = 0;
    cache_alen = 0;
    cache_vlen = 0;
    if(pch && pct){
        cache_len =  pct->frame_pts - pch->frame_pts;

        pc = pch;
        while( pc && pc->frame_type != NGX_RTMP_MSG_AUDIO ){
            pc = pc->next;
        }
        cache_alen = stream->push_cache_aets - pc->frame_pts;

        pc = pch;
        while( pc && pc->frame_type != NGX_RTMP_MSG_VIDEO ){
            pc = pc->next;
        }
        cache_vlen = stream->push_cache_vets - pc->frame_pts;
    }

    NGX_RTMP_STAT_L(",\"cache_len\":");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui",
                  (ngx_uint_t) cache_len) - buf);
    
    NGX_RTMP_STAT_L(",\"cache_alen\":");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui",
                  (ngx_uint_t) cache_alen) - buf);
    
    NGX_RTMP_STAT_L(",\"cache_vlen\":");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui",
                  (ngx_uint_t) cache_vlen) - buf);
}
static void
ngx_rtmp_stat_push_cache(ngx_http_request_t *r, ngx_chain_t ***lll, ngx_rtmp_live_stream_t *stream)
{
    u_char  buf[NGX_INT_T_LEN];
    ngx_rtmp_live_push_cache_t  *pch, *pc;
    ngx_uint_t          cache_len, cache_alen, cache_vlen, nrelays;
    ngx_rtmp_relay_ctx_t           *rctx;

    NGX_RTMP_STAT_L("<cache_count>");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui",
                (ngx_uint_t) stream->push_cache_count) - buf);
    NGX_RTMP_STAT_L("</cache_count>");

    pch = stream->push_cache_head;
    cache_len = 0;
    cache_alen = 0;
    cache_vlen = 0;
    nrelays = 0;
    if( pch ){
        pc = pch;
        while( pc && (pc->frame_type != NGX_RTMP_MSG_AUDIO || pc->mandatory == 1)){
            pc = pc->next;
        }
        if(pc)
            cache_alen = stream->push_cache_aets - pc->frame_pts;

        pc = pch;
        while( pc && (pc->frame_type != NGX_RTMP_MSG_VIDEO || pc->mandatory == 1)){
            pc = pc->next;
        }
        if(pc)
            cache_vlen = stream->push_cache_vets - pc->frame_pts;

        cache_len =  cache_alen>=cache_vlen?cache_alen:cache_vlen ;
    }
    NGX_RTMP_STAT_L("<cache_alen>");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui",
                (ngx_uint_t) cache_alen) - buf);
    NGX_RTMP_STAT_L("</cache_alen>");

    NGX_RTMP_STAT_L("<cache_vlen>");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui",
                (ngx_uint_t) cache_vlen) - buf);
    NGX_RTMP_STAT_L("</cache_vlen>");

    NGX_RTMP_STAT_L("<cache_len>");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%ui",
                (ngx_uint_t) cache_len) - buf);
    NGX_RTMP_STAT_L("</cache_len>");

    if( stream && stream->relay_ctx && stream->relay_ctx->play ){
        //for (rctx = stream->relay_ctx->publish->play; rctx; rctx = rctx->next) {
        for (rctx = stream->relay_ctx->play; rctx; rctx = rctx->next) {
            nrelays += 1;
            //printf("SSSSS RRRRR %ld\n", rctx->nreconnects);
        }
    }
    
    NGX_RTMP_STAT_L("<nrelays>");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                "%ui", nrelays) - buf);
    NGX_RTMP_STAT_L("</nrelays>");
}

static char *
ngx_rtmp_stat_get_aac_profile(ngx_uint_t p, ngx_uint_t sbr, ngx_uint_t ps) {
    //printf("SSSSS ngx_rtmp_stat_get_aac_profile\n");
    switch (p) {
        case 1:
            return "Main";
        case 2:
            if (ps) {
                return "HEv2";
            }
            if (sbr) {
                return "HE";
            }
            return "LC";
        case 3:
            return "SSR";
        case 4:
            return "LTP";
        case 5:
            return "SBR";
        default:
            return "";
    }
}


static char *
ngx_rtmp_stat_get_avc_profile(ngx_uint_t p) {
    //printf("SSSSS ngx_rtmp_stat_get_avc_profile\n");
    switch (p) {
        case 66:
            return "Baseline";
        case 77:
            return "Main";
        case 100:
            return "High";
        default:
            return "";
    }
}


static void
ngx_rtmp_stat_live(ngx_http_request_t *r, ngx_chain_t ***lll,
        ngx_rtmp_live_app_conf_t *lacf)
{
    //printf("SSSSS ngx_rtmp_stat_live\n");
    ngx_rtmp_live_stream_t         *stream;
    //ngx_rtmp_codec_ctx_t           *codec;
    ngx_rtmp_stream_codec_ctx_t    *codec;
    ngx_rtmp_live_ctx_t            *ctx;
    ngx_rtmp_session_t             *s;
    ngx_int_t                       n;
    ngx_uint_t                      nclients, total_nclients, nreconnects;
    u_char                          buf[NGX_INT_T_LEN];
    u_char                          bbuf[NGX_INT32_LEN];
    ngx_rtmp_stat_loc_conf_t       *slcf;
    u_char                         *cname;
    ngx_rtmp_relay_reconnect_t     *rrs;
    ngx_rtmp_relay_target_t        *target;

    if (!lacf->live) {
        return;
    }

    slcf = ngx_http_get_module_loc_conf(r, ngx_rtmp_stat_module);

    NGX_RTMP_STAT_L("<live>\r\n");

    total_nclients = 0;
    for (n = 0; n < lacf->nbuckets; ++n) {
        for (stream = lacf->streams[n]; stream; stream = stream->next) {
            NGX_RTMP_STAT_L("<stream>\r\n");

            NGX_RTMP_STAT_L("<name>");
            NGX_RTMP_STAT_ECS(stream->name);
            NGX_RTMP_STAT_L("</name>\r\n");

            NGX_RTMP_STAT_L("<time>");
            NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%i",
                          (ngx_int_t) (ngx_current_msec - stream->epoch))
                          - buf);
            NGX_RTMP_STAT_L("</time>");
            
            ngx_rtmp_stat_bw(r, lll, &stream->bw_in, "in",
                             NGX_RTMP_STAT_BW_BYTES);
            ngx_rtmp_stat_bw(r, lll, &stream->bw_out, "out",
                             NGX_RTMP_STAT_BW_BYTES);
            /*
            ngx_rtmp_stat_bw(r, lll, &stream->bw_in_audio, "in_audio",
                             NGX_RTMP_STAT_BW);
            ngx_rtmp_stat_bw(r, lll, &stream->bw_in_video, "in_video",
                             NGX_RTMP_STAT_BW);
            */
            ngx_rtmp_stat_bw(r, lll, &stream->bw_in_audio, "in_audio",
                             NGX_RTMP_STAT_BW);
            ngx_rtmp_stat_bw(r, lll, &stream->bw_in_video, "in_video",
                             NGX_RTMP_STAT_BW);

            ngx_rtmp_stat_bw(r, lll, &stream->bw_out_audio, "out_audio",
                             NGX_RTMP_STAT_BW);
            ngx_rtmp_stat_bw(r, lll, &stream->bw_out_video, "out_video",
                             NGX_RTMP_STAT_BW);
            
            nclients = 0;
            codec = NULL;
            // 针对publish
            //printf("SSSSS stream:%p stream->ctx:%p active:%d head:%p\n", stream, stream->ctx, stream->active, stream->push_cache_head);
            // 开启缓存的时候监控显示publish（stream->lacf为空,只有在混存开启的时候才会赋值）
            // 关闭缓存，并且有推流的时候显示publish
            if ( (stream && stream->lacf && stream->lacf->push_cache && stream->push_cache_head)
                || (stream && stream->active) ) {
                // 计算缓存的时间
                ngx_rtmp_stat_push_cache(r, lll, stream);
                
                NGX_RTMP_STAT_L("<client>");
                codec = &stream->codec_ctx;
                
                ngx_rtmp_stat_publish(r, lll, codec);
                // 丢包率
                NGX_RTMP_STAT_L("<dropped>");
                NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                            "%ui", stream->ndropped) - buf);
                NGX_RTMP_STAT_L("</dropped>");

                // 音视频同步
                NGX_RTMP_STAT_L("<avsync>");
                if (!stream->interleave) {
                    NGX_RTMP_STAT(bbuf, ngx_snprintf(bbuf, sizeof(bbuf),
                                "%D", stream->cs[1].timestamp -
                                stream->cs[0].timestamp) - bbuf);
                }
                NGX_RTMP_STAT_L("</avsync>");

                NGX_RTMP_STAT_L("<timestamp>");
                NGX_RTMP_STAT(bbuf, ngx_snprintf(bbuf, sizeof(bbuf),
                            "%D", stream->current_time) - bbuf);
                NGX_RTMP_STAT_L("</timestamp>");

                NGX_RTMP_STAT_L("<publishing/>");

                if ( stream->active ) {
                    NGX_RTMP_STAT_L("<active/>");
                }

                NGX_RTMP_STAT_L("</client>\r\n");
                
                ++nclients;
            }
            // 针对play
            for (ctx = stream->ctx; ctx; ctx = ctx->next) {
                if( ctx->publishing )
                    continue;
                ++nclients;

                s = ctx->session;
                 
                if (slcf->stat & NGX_RTMP_STAT_CLIENTS) {
                    NGX_RTMP_STAT_L("<client>");

                    // 添加转推断开次数
                    if( s->relay ) {
                        nreconnects = 0;
                        rrs = stream->relay_reconnects;
                        for( ; rrs ; rrs = rrs->next ){
                            target = (ngx_rtmp_relay_target_t*)rrs->target;
                            if( target && !ngx_strncmp(s->connection->addr_text.data, target->url.url.data, target->url.url.len) ) {
                                nreconnects = rrs->nreconnects; 
                            }
                        }
                        NGX_RTMP_STAT_L("<nreconnects>");
                        NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                                    "%ui", nreconnects) - buf);
                        NGX_RTMP_STAT_L("</nreconnects>");
                    }

                    // 丢包率
                    NGX_RTMP_STAT_L("<dropped>");
                    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                                "%ui", ctx->ndropped) - buf);
                    NGX_RTMP_STAT_L("</dropped>");

                    // 音视频同步
                    NGX_RTMP_STAT_L("<avsync>");
                    if (!lacf->interleave) {
                        NGX_RTMP_STAT(bbuf, ngx_snprintf(bbuf, sizeof(bbuf),
                                    "%D", ctx->cs[1].timestamp -
                                    ctx->cs[0].timestamp) - bbuf);
                    }
                    NGX_RTMP_STAT_L("</avsync>");

                    NGX_RTMP_STAT_L("<timestamp>");
                    NGX_RTMP_STAT(bbuf, ngx_snprintf(bbuf, sizeof(bbuf),
                                "%D", s->current_time) - bbuf);
                    NGX_RTMP_STAT_L("</timestamp>");

                    NGX_RTMP_STAT_L("<timestamp>");
                    NGX_RTMP_STAT(bbuf, ngx_snprintf(bbuf, sizeof(bbuf),
                                "%D", s->current_time) - bbuf);
                    NGX_RTMP_STAT_L("</timestamp>");

                    // play or publish
                    ngx_rtmp_stat_client(r, lll, s);

                    if (ctx->active) {
                        NGX_RTMP_STAT_L("<active/>");
                    }

                    NGX_RTMP_STAT_L("</client>\r\n");
                }
            }
            total_nclients += nclients;

            if (codec) {
                NGX_RTMP_STAT_L("<meta>");

                NGX_RTMP_STAT_L("<video>");
                NGX_RTMP_STAT_L("<width>");
                NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                              "%ui", codec->width) - buf);
                NGX_RTMP_STAT_L("</width><height>");
                NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                              "%ui", codec->height) - buf);
                NGX_RTMP_STAT_L("</height><frame_rate>");
                NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                              "%ui", codec->frame_rate) - buf);
                NGX_RTMP_STAT_L("</frame_rate>");

                cname = ngx_rtmp_get_video_codec_name(codec->video_codec_id);
                if (*cname) {
                    NGX_RTMP_STAT_L("<codec>");
                    NGX_RTMP_STAT_ECS(cname);
                    NGX_RTMP_STAT_L("</codec>");
                }
                if (codec->avc_profile) {
                    NGX_RTMP_STAT_L("<profile>");
                    NGX_RTMP_STAT_CS(
                            ngx_rtmp_stat_get_avc_profile(codec->avc_profile));
                    NGX_RTMP_STAT_L("</profile>");
                }
                if (codec->avc_level) {
                    NGX_RTMP_STAT_L("<compat>");
                    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                                  "%ui", codec->avc_compat) - buf);
                    NGX_RTMP_STAT_L("</compat>");
                }
                if (codec->avc_level) {
                    NGX_RTMP_STAT_L("<level>");
                    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                                  "%.1f", codec->avc_level / 10.) - buf);
                    NGX_RTMP_STAT_L("</level>");
                }
                NGX_RTMP_STAT_L("</video>");

                NGX_RTMP_STAT_L("<audio>");
                cname = ngx_rtmp_get_audio_codec_name(codec->audio_codec_id);
                if (*cname) {
                    NGX_RTMP_STAT_L("<codec>");
                    NGX_RTMP_STAT_ECS(cname);
                    NGX_RTMP_STAT_L("</codec>");
                }
                if (codec->aac_profile) {
                    NGX_RTMP_STAT_L("<profile>");
                    NGX_RTMP_STAT_CS(
                            ngx_rtmp_stat_get_aac_profile(codec->aac_profile,
                                                          codec->aac_sbr,
                                                          codec->aac_ps));
                    NGX_RTMP_STAT_L("</profile>");
                }
                if (codec->aac_chan_conf) {
                    NGX_RTMP_STAT_L("<channels>");
                    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                                  "%ui", codec->aac_chan_conf) - buf);
                    NGX_RTMP_STAT_L("</channels>");
                } else if (codec->audio_channels) {
                    NGX_RTMP_STAT_L("<channels>");
                    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                                  "%ui", codec->audio_channels) - buf);
                    NGX_RTMP_STAT_L("</channels>");
                }
                if (codec->sample_rate) {
                    NGX_RTMP_STAT_L("<sample_rate>");
                    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                                  "%ui", codec->sample_rate) - buf);
                    NGX_RTMP_STAT_L("</sample_rate>");
                }
                NGX_RTMP_STAT_L("</audio>");

                NGX_RTMP_STAT_L("</meta>\r\n");
            }

            NGX_RTMP_STAT_L("<nclients>");
            NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                          "%ui", nclients) - buf);
            NGX_RTMP_STAT_L("</nclients>\r\n");

            if (stream->publishing) {
                NGX_RTMP_STAT_L("<publishing/>\r\n");
            }

            if (stream->active) {
                NGX_RTMP_STAT_L("<active/>\r\n");
            }

            NGX_RTMP_STAT_L("</stream>\r\n");
        }
    }

    NGX_RTMP_STAT_L("<nclients>");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                  "%ui", total_nclients) - buf);
    NGX_RTMP_STAT_L("</nclients>\r\n");

    NGX_RTMP_STAT_L("</live>\r\n");
}


static void
ngx_rtmp_stat_play(ngx_http_request_t *r, ngx_chain_t ***lll,
        ngx_rtmp_play_app_conf_t *pacf)
{
    //printf("SSSSS ngx_rtmp_stat_play\n");
    ngx_rtmp_play_ctx_t            *ctx, *sctx;
    ngx_rtmp_session_t             *s;
    ngx_uint_t                      n, nclients, total_nclients;
    u_char                          buf[NGX_INT_T_LEN];
    u_char                          bbuf[NGX_INT32_LEN];
    ngx_rtmp_stat_loc_conf_t       *slcf;

    if (pacf->entries.nelts == 0) {
        return;
    }

    slcf = ngx_http_get_module_loc_conf(r, ngx_rtmp_stat_module);

    NGX_RTMP_STAT_L("<play>\r\n");

    total_nclients = 0;
    for (n = 0; n < pacf->nbuckets; ++n) {
        for (ctx = pacf->ctx[n]; ctx; ) {
            NGX_RTMP_STAT_L("<stream>\r\n");

            NGX_RTMP_STAT_L("<name>");
            NGX_RTMP_STAT_ECS(ctx->name);
            NGX_RTMP_STAT_L("</name>\r\n");

            nclients = 0;
            sctx = ctx;
            for (; ctx; ctx = ctx->next) {
                if (ngx_strcmp(ctx->name, sctx->name)) {
                    break;
                }

                nclients++;

                s = ctx->session;
                if (slcf->stat & NGX_RTMP_STAT_CLIENTS) {
                    NGX_RTMP_STAT_L("<client>");

                    ngx_rtmp_stat_client(r, lll, s);

                    NGX_RTMP_STAT_L("<timestamp>");
                    NGX_RTMP_STAT(bbuf, ngx_snprintf(bbuf, sizeof(bbuf),
                                  "%D", s->current_time) - bbuf);
                    NGX_RTMP_STAT_L("</timestamp>");

                    NGX_RTMP_STAT_L("</client>\r\n");
                }
            }
            total_nclients += nclients;

            NGX_RTMP_STAT_L("<active/>");
            NGX_RTMP_STAT_L("<nclients>");
            NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                          "%ui", nclients) - buf);
            NGX_RTMP_STAT_L("</nclients>\r\n");

            NGX_RTMP_STAT_L("</stream>\r\n");
        }
    }

    NGX_RTMP_STAT_L("<nclients>");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                  "%ui", total_nclients) - buf);
    NGX_RTMP_STAT_L("</nclients>\r\n");

    NGX_RTMP_STAT_L("</play>\r\n");
}


static void
ngx_rtmp_stat_application(ngx_http_request_t *r, ngx_chain_t ***lll,
        ngx_rtmp_core_app_conf_t *cacf)
{
    //printf("SSSSS ngx_rtmp_stat_application\n");
    ngx_rtmp_stat_loc_conf_t       *slcf;

    NGX_RTMP_STAT_L("<application>\r\n");
    NGX_RTMP_STAT_L("<name>");
    NGX_RTMP_STAT_ES(&cacf->name);
    NGX_RTMP_STAT_L("</name>\r\n");

    slcf = ngx_http_get_module_loc_conf(r, ngx_rtmp_stat_module);

    if (slcf->stat & NGX_RTMP_STAT_LIVE) {
        ngx_rtmp_stat_live(r, lll,
                cacf->app_conf[ngx_rtmp_live_module.ctx_index]);
    }

    if (slcf->stat & NGX_RTMP_STAT_PLAY) {
        ngx_rtmp_stat_play(r, lll,
                cacf->app_conf[ngx_rtmp_play_module.ctx_index]);
    }

    NGX_RTMP_STAT_L("</application>\r\n");
}


static void
ngx_rtmp_stat_server(ngx_http_request_t *r, ngx_chain_t ***lll,
        ngx_rtmp_core_srv_conf_t *cscf)
{
    //printf("SSSSS ngx_rtmp_stat_server\n");
    ngx_rtmp_core_app_conf_t      **cacf;
    size_t                          n;

    NGX_RTMP_STAT_L("<server>\r\n");

#ifdef NGX_RTMP_POOL_DEBUG
    ngx_rtmp_stat_dump_pool(r, lll, cscf->pool);
#endif

    cacf = cscf->applications.elts;
    for (n = 0; n < cscf->applications.nelts; ++n, ++cacf) {
        ngx_rtmp_stat_application(r, lll, *cacf);
    }

    NGX_RTMP_STAT_L("</server>\r\n");
}


//JSON ################################################################
static void
ngx_rtmp_stat_bw_json(ngx_http_request_t *r, ngx_chain_t ***lll,
        ngx_rtmp_bandwidth_t *bw, char *name,
        ngx_uint_t flags)
{
    u_char  buf[NGX_INT64_LEN + 9];
    
    ngx_rtmp_update_bandwidth(bw, 0);

    if (flags & NGX_RTMP_STAT_BW) {
        NGX_RTMP_STAT_L("\"bw_");
        NGX_RTMP_STAT_CS(name);
        NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "\":%uL",
                    bw->bandwidth * 8)
                - buf);
    }

    if (flags & NGX_RTMP_STAT_BYTES) {
        NGX_RTMP_STAT_L(",\"bytes_");
        NGX_RTMP_STAT_CS(name);
        NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "\":%uL",
                    bw->bytes)
                - buf);
    }
}

static void
ngx_rtmp_stat_live_json(ngx_http_request_t *r, ngx_chain_t ***lll,
                ngx_rtmp_live_app_conf_t *lacf)
{
    ngx_rtmp_live_stream_t         *stream;
    ngx_rtmp_stream_codec_ctx_t    *codec;
    ngx_rtmp_live_ctx_t            *ctx;
    ngx_rtmp_session_t             *s;
    ngx_int_t                       n;
    ngx_uint_t                      nstreams, nclients, total_nclients;
    u_char                          buf[NGX_INT_T_LEN];
    u_char                          bbuf[NGX_INT32_LEN];
    //ngx_rtmp_stat_loc_conf_t       *slcf;
    u_char                         *cname;
    ngx_rtmp_relay_ctx_t           *rctx;
    ngx_uint_t                      nrelays;

    if (!lacf->live) {
        return;
    }

    //slcf = ngx_http_get_module_loc_conf(r, ngx_rtmp_stat_module);

    total_nclients = 0;
    nstreams = 0;
    NGX_RTMP_STAT_L("{\r\n\"streams\":\r\n[");
    for (n = 0; n < lacf->nbuckets; ++n) {
        for (stream = lacf->streams[n]; stream; stream = stream->next, ++nstreams) {
            if( nstreams != 0 )
                NGX_RTMP_STAT_L(",");
            
            NGX_RTMP_STAT_L("{\"name\":\"");
            NGX_RTMP_STAT_ECS(stream->name);
            NGX_RTMP_STAT_L("\"");
            
            NGX_RTMP_STAT_L(",\"time\":");
            NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%i",
                        (ngx_int_t) (ngx_current_msec - stream->epoch)) - buf);
            NGX_RTMP_STAT_L(",");
            
            ngx_rtmp_stat_bw_json(r, lll, &stream->bw_in, "in",
                    NGX_RTMP_STAT_BW_BYTES);
            NGX_RTMP_STAT_L(",");
            ngx_rtmp_stat_bw_json(r, lll, &stream->bw_in_audio, "in_audio",
                    NGX_RTMP_STAT_BW);
            NGX_RTMP_STAT_L(",");
            ngx_rtmp_stat_bw_json(r, lll, &stream->bw_in_video, "in_video",
                    NGX_RTMP_STAT_BW);
            NGX_RTMP_STAT_L(",");
            ngx_rtmp_stat_bw_json(r, lll, &stream->bw_out, "out",
                    NGX_RTMP_STAT_BW_BYTES);
            NGX_RTMP_STAT_L(",");
            ngx_rtmp_stat_bw_json(r, lll, &stream->bw_out_audio, "out_audio",
                    NGX_RTMP_STAT_BW);
            NGX_RTMP_STAT_L(",");
            ngx_rtmp_stat_bw_json(r, lll, &stream->bw_out_video, "out_video",
                    NGX_RTMP_STAT_BW);
            
            // cache status  
            NGX_RTMP_STAT_L(",");
            ngx_rtmp_stat_push_cache_json(r, lll, stream);
            
            nclients = 0;
            codec = NULL;
            NGX_RTMP_STAT_L(",\"clients\":\r\n[");
            for (ctx = stream->ctx; ctx; ctx = ctx->next, ++nclients) {
                if( nclients != 0 )
                    NGX_RTMP_STAT_L(",");
                    
                NGX_RTMP_STAT_L("{\r\n\"dropped\":");
                NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                            "%ui", ctx->ndropped) - buf);                  

                NGX_RTMP_STAT_L(",\"avsync\":");
                if (!lacf->interleave) {
                    NGX_RTMP_STAT(bbuf, ngx_snprintf(bbuf, sizeof(bbuf),
                                "%D", ctx->cs[1].timestamp -
                                ctx->cs[0].timestamp) - bbuf);
                }
                if (ctx->publishing) {
                    codec = &stream->codec_ctx;    
                    NGX_RTMP_STAT_L(",");
                    ngx_rtmp_stat_publish_json(r, lll, codec);
                  
                    //不知道是什么 
                    NGX_RTMP_STAT_L(",\"timestamp\":");
                    NGX_RTMP_STAT(bbuf, ngx_snprintf(bbuf, sizeof(bbuf),
                                "%D", stream->current_time) - bbuf); 
                    NGX_RTMP_STAT_L(",\"publishing\":1");
                } else {
                    s = ctx->session;
                    NGX_RTMP_STAT_L(",");
                    ngx_rtmp_stat_client_json(r, lll, s);
                    
                    NGX_RTMP_STAT_L(",\"timestamp\":");
                    NGX_RTMP_STAT(bbuf, ngx_snprintf(bbuf, sizeof(bbuf),
                                "%D", s->current_time) - bbuf);
                    NGX_RTMP_STAT_L(",\"publishing\":0");
                }
                
                if (ctx->active) {
                    NGX_RTMP_STAT_L(",\"active\":1");
                } else {
                    NGX_RTMP_STAT_L(",\"active\":0");
                }
                
                NGX_RTMP_STAT_L("\r\n}");
            }
            total_nclients += nclients;
            NGX_RTMP_STAT_L("]");
            
            NGX_RTMP_STAT_L(",\"nclients\":");
            NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%l",
                        (ngx_int_t) nclients) - buf);
                
            nrelays = 0;
            for (rctx = stream->relay_ctx; rctx; rctx = rctx->next) 
                nrelays += 1;
            NGX_RTMP_STAT_L(",\"nrelays\":");
            NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                        "%ui", nrelays) - buf);                  
            
            NGX_RTMP_STAT_L(",\"meta\":{\r\n");         
            if ( codec ) {
                NGX_RTMP_STAT_L("\"audio\":{");
                cname = ngx_rtmp_get_audio_codec_name(codec->audio_codec_id);
                if (*cname) {
                    NGX_RTMP_STAT_L("\"codec\":\"");
                    NGX_RTMP_STAT_ECS(cname);
                    NGX_RTMP_STAT_L("\"");
                }
                if (codec->aac_profile) {
                    NGX_RTMP_STAT_L(",\"profile\":\"");
                    NGX_RTMP_STAT_CS(
                            ngx_rtmp_stat_get_aac_profile(codec->aac_profile,
                                codec->aac_sbr,
                                codec->aac_ps));
                    NGX_RTMP_STAT_L("\"");
                }
                if (codec->aac_chan_conf) {
                    NGX_RTMP_STAT_L(",\"channels\":");
                    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                                "%ui", codec->aac_chan_conf) - buf);
                } else if (codec->audio_channels) {
                    NGX_RTMP_STAT_L(",\"channels\":");
                    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                                "%ui", codec->audio_channels) - buf);
                }
                if (codec->sample_rate) {
                    NGX_RTMP_STAT_L(",\"sample_rate\":");
                    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                                "%ui", codec->sample_rate) - buf);
                }
                NGX_RTMP_STAT_L("\r\n}");         
                
                
                NGX_RTMP_STAT_L(",\"video\":{");
                NGX_RTMP_STAT_L("\"width\":");
                NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                            "%ui", codec->width) - buf);
                NGX_RTMP_STAT_L(",\"height\":");
                NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                            "%ui", codec->height) - buf);
                NGX_RTMP_STAT_L(",\"frame_rate\":");
                NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                            "%ui", codec->frame_rate) - buf);
                cname = ngx_rtmp_get_video_codec_name(codec->video_codec_id);
                if (*cname) {
                    NGX_RTMP_STAT_L(",\"codec\":\"");
                    NGX_RTMP_STAT_ECS(cname);
                    NGX_RTMP_STAT_L("\"");
                }
                if (codec->avc_profile) {
                    NGX_RTMP_STAT_L(",\"profile\":\"");
                    NGX_RTMP_STAT_CS(
                            ngx_rtmp_stat_get_avc_profile(codec->avc_profile));
                    NGX_RTMP_STAT_L("\"");
                }
                if (codec->avc_level) {
                    NGX_RTMP_STAT_L(",\"compat\":");
                    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                                "%ui", codec->avc_compat) - buf);
                }
                if (codec->avc_level) {
                    NGX_RTMP_STAT_L(",\"level\":");
                    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf),
                                "%.1f", codec->avc_level / 10.) - buf);
                }
                NGX_RTMP_STAT_L("\r\n}");         
            }
            NGX_RTMP_STAT_L("\r\n}");         

            NGX_RTMP_STAT_L("\r\n}");
        }
    } 
    NGX_RTMP_STAT_L("]");

    NGX_RTMP_STAT_L(",\"nclients\":");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%l",
                (ngx_int_t) total_nclients) - buf);

    NGX_RTMP_STAT_L("\r\n}");
}
static void
ngx_rtmp_stat_application_json(ngx_http_request_t *r, ngx_chain_t ***lll,
        ngx_rtmp_core_app_conf_t *cacf)
{
    if(cacf == NULL){
        return;
    }
    
    ngx_rtmp_stat_loc_conf_t       *slcf;

    NGX_RTMP_STAT_L("{\r\n");
    NGX_RTMP_STAT_L("\"name\":\"");
    NGX_RTMP_STAT_ES(&cacf->name);
    NGX_RTMP_STAT_L("\"");
    
    slcf = ngx_http_get_module_loc_conf(r, ngx_rtmp_stat_module);
    

    NGX_RTMP_STAT_L(",\"lives\":\r\n[");
    if (slcf->stat & NGX_RTMP_STAT_LIVE) {
        ngx_rtmp_stat_live_json(r, lll,
                cacf->app_conf[ngx_rtmp_live_module.ctx_index]);
    }

    if (slcf->stat & NGX_RTMP_STAT_PLAY) {
  //      ngx_rtmp_stat_play(r, lll,
  //              cacf->app_conf[ngx_rtmp_play_module.ctx_index]);
    }
    NGX_RTMP_STAT_L("]");
    
    NGX_RTMP_STAT_L("\r\n}");
}
static void
ngx_rtmp_stat_server_json(ngx_http_request_t *r, ngx_chain_t ***lll,
                 ngx_rtmp_core_srv_conf_t *cscf)
{
    ngx_rtmp_core_app_conf_t      **cacf;
    size_t                          n;
    u_char                          buf[NGX_INT_T_LEN];

    NGX_RTMP_STAT_L("{\r\n\"applications\":\r\n[");
    cacf = cscf->applications.elts;
    for (n = 0; n < cscf->applications.nelts; ++n, ++cacf) {
        ngx_rtmp_stat_application_json(r, lll, *cacf);
    }
    NGX_RTMP_STAT_L("]");
    
    NGX_RTMP_STAT_L(",\"nclients\":");
    NGX_RTMP_STAT(buf, ngx_snprintf(buf, sizeof(buf), "%l",
                (ngx_int_t) n) - buf);

    NGX_RTMP_STAT_L("\r\n}");
}
static ngx_int_t
ngx_rtmp_stat_handler_json(ngx_http_request_t *r)
{
    //printf("SSSSS ngx_rtmp_stat_json_handler\n");
    //ngx_rtmp_live_stream_t         *stream;
    ngx_rtmp_stat_loc_conf_t        *slcf;
    ngx_rtmp_core_main_conf_t       *cmcf;
    ngx_rtmp_core_srv_conf_t       **cscf;
    ngx_chain_t                     *cl, *l, **ll, ***lll;
    size_t                          n;
    off_t                           len;
    static u_char                   tbuf[NGX_TIME_T_LEN];
    static u_char                   nbuf[NGX_INT_T_LEN];
    
    slcf = ngx_http_get_module_loc_conf(r, ngx_rtmp_stat_module);
    if( slcf->stat == 0){
        return NGX_DECLINED;
    }

    cmcf = ngx_rtmp_core_main_conf;
    if ( cmcf == NULL ){
        goto error;
    }
    
    cl = NULL;
    ll = &cl;
    lll = &ll;
     
    NGX_RTMP_STAT_L("{\r\n");
    NGX_RTMP_STAT_L("\"pid\":");
    NGX_RTMP_STAT(nbuf, ngx_snprintf(nbuf, sizeof(nbuf),
                "%ui", (ngx_uint_t) ngx_getpid()) - nbuf);
    NGX_RTMP_STAT_L(",\"uptime\":");
    NGX_RTMP_STAT(tbuf, ngx_snprintf(tbuf, sizeof(tbuf),
                "%T", ngx_cached_time->sec - start_time) - tbuf);
    NGX_RTMP_STAT_L(",\"naccepted\":");
    NGX_RTMP_STAT(nbuf, ngx_snprintf(nbuf, sizeof(nbuf),
                "%ui", ngx_rtmp_naccepted) - nbuf);
    
    NGX_RTMP_STAT_L(",\"servers\":\r\n[");
    cscf = cmcf->servers.elts;
    for (n = 0; n < cmcf->servers.nelts; ++n, ++cscf) {
        ngx_rtmp_stat_server_json(r, lll, *cscf);
    }
    NGX_RTMP_STAT_L("]");

    NGX_RTMP_STAT_L("\r\n}");
    
      
    len = 0;
    for (l = cl; l; l = l->next) {
        len += (l->buf->last - l->buf->pos);
    }
    
    ngx_str_set(&r->headers_out.content_type, "application/json");
    r->headers_out.content_length_n = len;
    r->headers_out.status = NGX_HTTP_OK;
    ngx_http_send_header(r);
    if(cl == NULL){
        return NGX_ERROR;
    }
    (*ll)->buf->last_buf = 1;
    return ngx_http_output_filter(r, cl);
    
error:
    r->headers_out.status = NGX_HTTP_INTERNAL_SERVER_ERROR;
    r->headers_out.content_length_n = 0;
    return ngx_http_send_header(r);
}
//JSON ################################################################

static ngx_int_t
ngx_rtmp_stat_handler(ngx_http_request_t *r)
{
    //printf("SSSSS ngx_rtmp_stat_handler\n");
    ngx_rtmp_stat_loc_conf_t       *slcf;
    ngx_rtmp_core_main_conf_t      *cmcf;
    ngx_rtmp_core_srv_conf_t      **cscf;
    ngx_chain_t                    *cl, *l, **ll, ***lll;
    size_t                          n;
    off_t                           len;
    static u_char                   tbuf[NGX_TIME_T_LEN];
    static u_char                   nbuf[NGX_INT_T_LEN];

    slcf = ngx_http_get_module_loc_conf(r, ngx_rtmp_stat_module);
    if (slcf->stat == 0) {
        return NGX_DECLINED;
    }

    cmcf = ngx_rtmp_core_main_conf;
    if (cmcf == NULL) {
        goto error;
    }

    cl = NULL;
    ll = &cl;
    lll = &ll;

    NGX_RTMP_STAT_L("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\r\n");
    if (slcf->stylesheet.len) {
        NGX_RTMP_STAT_L("<?xml-stylesheet type=\"text/xsl\" href=\"");
        NGX_RTMP_STAT_ES(&slcf->stylesheet);
        NGX_RTMP_STAT_L("\" ?>\r\n");
    }

    NGX_RTMP_STAT_L("<rtmp>\r\n");

#ifdef NGINX_VERSION
    NGX_RTMP_STAT_L("<nginx_version>" NGINX_VERSION "</nginx_version>\r\n");
#endif

#ifdef NGINX_RTMP_VERSION
    NGX_RTMP_STAT_L("<nginx_rtmp_version>" NGINX_RTMP_VERSION "</nginx_rtmp_version>\r\n");
#endif

#ifdef NGX_COMPILER
    NGX_RTMP_STAT_L("<compiler>" NGX_COMPILER "</compiler>\r\n");
#endif
    NGX_RTMP_STAT_L("<built>" __DATE__ " " __TIME__ "</built>\r\n");

    NGX_RTMP_STAT_L("<pid>");
    NGX_RTMP_STAT(nbuf, ngx_snprintf(nbuf, sizeof(nbuf),
                  "%ui", (ngx_uint_t) ngx_getpid()) - nbuf);
    NGX_RTMP_STAT_L("</pid>\r\n");

    NGX_RTMP_STAT_L("<uptime>");
    NGX_RTMP_STAT(tbuf, ngx_snprintf(tbuf, sizeof(tbuf),
                  "%T", ngx_cached_time->sec - start_time) - tbuf);
    NGX_RTMP_STAT_L("</uptime>\r\n");

    NGX_RTMP_STAT_L("<naccepted>");
    NGX_RTMP_STAT(nbuf, ngx_snprintf(nbuf, sizeof(nbuf),
                  "%ui", ngx_rtmp_naccepted) - nbuf);
    NGX_RTMP_STAT_L("</naccepted>\r\n");

    ngx_rtmp_stat_bw(r, lll, &ngx_rtmp_bw_in, "in", NGX_RTMP_STAT_BW_BYTES);
    ngx_rtmp_stat_bw(r, lll, &ngx_rtmp_bw_out, "out", NGX_RTMP_STAT_BW_BYTES);

    cscf = cmcf->servers.elts;
    for (n = 0; n < cmcf->servers.nelts; ++n, ++cscf) {
        ngx_rtmp_stat_server(r, lll, *cscf);
    }

    NGX_RTMP_STAT_L("</rtmp>\r\n");

    len = 0;
    for (l = cl; l; l = l->next) {
        len += (l->buf->last - l->buf->pos);
    }
    ngx_str_set(&r->headers_out.content_type, "text/xml");
    r->headers_out.content_length_n = len;
    r->headers_out.status = NGX_HTTP_OK;
    ngx_http_send_header(r);
    (*ll)->buf->last_buf = 1;
    return ngx_http_output_filter(r, cl);

error:
    r->headers_out.status = NGX_HTTP_INTERNAL_SERVER_ERROR;
    r->headers_out.content_length_n = 0;
    return ngx_http_send_header(r);
}


static void *
ngx_rtmp_stat_create_loc_conf(ngx_conf_t *cf)
{
    //printf("SSSSS ngx_rtmp_stat_create_loc_conf\n");
    ngx_rtmp_stat_loc_conf_t       *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_stat_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->stat = 0;

    return conf;
}


static char *
ngx_rtmp_stat_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    //printf("SSSSS ngx_rtmp_stat_merge_loc_conf\n");
    ngx_rtmp_stat_loc_conf_t       *prev = parent;
    ngx_rtmp_stat_loc_conf_t       *conf = child;

    ngx_conf_merge_bitmask_value(conf->stat, prev->stat, 0);
    ngx_conf_merge_str_value(conf->stylesheet, prev->stylesheet, "");

    return NGX_CONF_OK;
}


static char *
ngx_rtmp_stat(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    //printf("SSSSS ngx_rtmp_stat\n");
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    if( 0 ){
        //clcf->handler = ngx_rtmp_stat_handler_cache;
        // json 暂时屏蔽
        clcf->handler = ngx_rtmp_stat_handler_json;
    } else {
        clcf->handler = ngx_rtmp_stat_handler;
    }

    return ngx_conf_set_bitmask_slot(cf, cmd, conf);
}


static ngx_int_t
ngx_rtmp_stat_postconfiguration(ngx_conf_t *cf)
{
    //printf("SSSSS ngx_rtmp_stat_postconfiguration\n");
    start_time = ngx_cached_time->sec;

    return NGX_OK;
}
