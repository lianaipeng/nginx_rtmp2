
/*
 * Copyright (C) Roman Arutyunyan
 */


#ifndef _NGX_RTMP_LIVE_H_INCLUDED_
#define _NGX_RTMP_LIVE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp.h"
#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_bandwidth.h"
#include "ngx_rtmp_streams.h"

#include "ngx_rtmp_relay_module.h"

#include "ngx_rtmp_codec_module.h"

typedef struct ngx_rtmp_live_ctx_s ngx_rtmp_live_ctx_t;
typedef struct ngx_rtmp_live_stream_s ngx_rtmp_live_stream_t;
typedef struct ngx_rtmp_live_push_cache_s ngx_rtmp_live_push_cache_t;
typedef struct ngx_rtmp_live_app_conf_s ngx_rtmp_live_app_conf_t;


typedef struct {
    unsigned                            active:1;
    uint32_t                            timestamp;
    uint32_t                            csid;
    uint32_t                            dropped;
} ngx_rtmp_live_chunk_stream_t;


struct ngx_rtmp_live_ctx_s {
    ngx_rtmp_session_t                 *session;
    ngx_rtmp_live_stream_t             *stream;
    ngx_rtmp_live_ctx_t                *next;
    ngx_uint_t                          ndropped;
    ngx_rtmp_live_chunk_stream_t        cs[2];
    ngx_uint_t                          meta_version;
    ngx_event_t                         idle_evt;
    unsigned                            active:1;
    unsigned                            publishing:1;
    unsigned                            silent:1;
    unsigned                            paused:1;

    ngx_uint_t                          newflag;

    void*                               target;  // only use in monitor 
};

// 添加缓存结构体
struct ngx_rtmp_live_push_cache_s{
    ngx_int_t                           frame_type;
    ngx_int_t                           frame_len;
    int64_t                             frame_pts;
    ngx_flag_t                          frame_flag;
    ngx_chain_t                         *frame_buf;
    ngx_rtmp_header_t                   frame_header;
    ngx_rtmp_live_push_cache_t          *next;

    ngx_flag_t                          has_closed;  // 流是否被关闭过
    ngx_int_t                           mandatory;   // H264 header type
}; 

typedef struct {
    ngx_flag_t                  is_init;
    ngx_uint_t                  width;
    ngx_uint_t                  height;
    ngx_uint_t                  duration;
    ngx_uint_t                  frame_rate;
    ngx_uint_t                  video_data_rate;
    ngx_uint_t                  video_codec_id;
    ngx_uint_t                  audio_data_rate;
    ngx_uint_t                  audio_codec_id;
    ngx_uint_t                  aac_profile;
    ngx_uint_t                  aac_chan_conf;
    ngx_uint_t                  aac_sbr;
    ngx_uint_t                  aac_ps;
    ngx_uint_t                  avc_profile;
    ngx_uint_t                  avc_compat;
    ngx_uint_t                  avc_level;
    ngx_uint_t                  avc_nal_bytes;
    ngx_uint_t                  avc_ref_frames;
    ngx_uint_t                  sample_rate;    /* 5512, 11025, 22050, 44100 */
    ngx_uint_t                  sample_size;    /* 1=8bit, 2=16bit */
    ngx_uint_t                  audio_channels; /* 1, 2 */
    
    // to_play 使用
    ngx_chain_t                 *aac_header; // 保存第一次的音频头
    ngx_chain_t                 *avc_header; // 保存第一次的视频头
    ngx_chain_t                 *meta;       // 保存视频信息
    ngx_uint_t                  meta_version;
    /*
    u_char                      profile[32];
    u_char                      level[32];
    
    ngx_uint_t                  meta_version;
    */
    
    // publish 
    ngx_uint_t                  number;
    ngx_str_t                   addr_text;
    ngx_uint_t                  epoch;
    ngx_str_t                   flashver;
    ngx_str_t                   page_url;
    ngx_str_t                   swf_url;
    ngx_str_t                   tc_url;
} ngx_rtmp_stream_codec_ctx_t;

typedef struct ngx_rtmp_relay_reconnect_s ngx_rtmp_relay_reconnect_t;

struct ngx_rtmp_relay_reconnect_s {
    ngx_uint_t                   target;
    ngx_uint_t                   ntargets;
    ngx_uint_t                   nreconnects;
    ngx_rtmp_relay_reconnect_t*  next;
};

struct ngx_rtmp_live_stream_s {
    u_char                              name[NGX_RTMP_MAX_NAME];
    ngx_rtmp_live_stream_t             *next;
    ngx_rtmp_live_ctx_t                *ctx;
    ngx_rtmp_bandwidth_t                bw_in;
    ngx_rtmp_bandwidth_t                bw_in_audio;
    ngx_rtmp_bandwidth_t                bw_in_video;
    ngx_rtmp_bandwidth_t                bw_out;
    ngx_rtmp_bandwidth_t                bw_out_audio;
    ngx_rtmp_bandwidth_t                bw_out_video;
    ngx_msec_t                          epoch;
    unsigned                            active:1;
    unsigned                            publishing:1;
    
    // 缓存    
    ngx_rtmp_live_push_cache_t          *push_cache_head;
    ngx_rtmp_live_push_cache_t          *push_cache_tail;
    ngx_int_t                            push_cache_count;

    //   空闲队列 
    ngx_rtmp_live_push_cache_t          *idle_cache_head;  
    ngx_rtmp_live_push_cache_t          *idle_cache_tail;
    ngx_int_t                            idle_cache_count;
    
    ngx_pool_t                          *pool;  // 内存池
    ngx_chain_t                         *free;  // 空闲内存队列
    
    //   时间校验　
    ngx_uint_t                          push_cache_expts; //end expect time
    //   释放缓存时间事件 
    ngx_event_t                         push_cache_event;
    //   延时销毁stream事件  
    ngx_event_t                         delay_close_event;
    
    //   当publish断开之后 才启用这三个参数 否则默认为空
    ngx_rtmp_session_t                  *session;    
    ngx_rtmp_live_app_conf_t            *lacf;
    ngx_rtmp_core_srv_conf_t            *cscf;
    ngx_rtmp_live_chunk_stream_t        cs[2];
    ngx_uint_t                          publish_closed_count;  // publish关闭次数
    // 视音频头 meta
    ngx_rtmp_stream_codec_ctx_t         codec_ctx;
    
    
    //push_realy
    ngx_rtmp_relay_ctx_t                *relay_ctx;    
    void                                **main_conf;
    void                                **srv_conf;
    void                                **app_conf;
    ngx_flag_t                          is_relay_start; // 是否已经开始转推
    ngx_rtmp_relay_reconnect_t          *relay_reconnects;     // 
     
    ngx_rtmp_publish_t                  publish;
    

    // 监控状态
    ngx_msec_t                          current_time;
    ngx_msec_t                          push_cache_aets;   // audio end timestamp 
    ngx_msec_t                          push_cache_vets;   // video end timestamp  
    ngx_msec_t                          push_cache_lts;   
    ngx_msec_t                          push_cache_delta;  
    ngx_flag_t                          is_publish_closed;     // publish is closed 
    
    ngx_uint_t                          ndropped;
    ngx_flag_t                          interleave;
};


//typedef struct{
struct ngx_rtmp_live_app_conf_s{
    ngx_int_t                           nbuckets;
    ngx_rtmp_live_stream_t            **streams;
    ngx_flag_t                          live;
    ngx_flag_t                          meta;
    ngx_msec_t                          sync;
    ngx_msec_t                          idle_timeout;
    ngx_flag_t                          atc;
    ngx_flag_t                          interleave;
    ngx_flag_t                          wait_key;
    ngx_flag_t                          wait_video;
    ngx_flag_t                          publish_notify;
    ngx_flag_t                          play_restart;
    ngx_flag_t                          idle_streams;
    ngx_msec_t                          buflen;
    ngx_pool_t                         *pool;
    ngx_rtmp_live_stream_t             *free_streams;
    
    // PUSH_CACHE 推流缓存
    ngx_flag_t                          push_cache;
    ngx_int_t                           push_cache_time_len;
    ngx_int_t                           push_cache_frame_num;
    ngx_flag_t                          publish_delay_close;
    ngx_msec_t                          publish_delay_close_len;
    
    
    ngx_uint_t                          stream_push_reconnect;
    ngx_array_t                 pulls;         /* ngx_rtmp_relay_target_t * */
    ngx_array_t                 pushes;        /* ngx_rtmp_relay_target_t * */
    ngx_array_t                 static_pulls;  /* ngx_rtmp_relay_target_t * */
    ngx_array_t                 static_events; /* ngx_event_t * */
    ngx_log_t                  *log;
    //ngx_uint_t                  nbuckets;
    //ngx_msec_t                  buflen;
    ngx_flag_t                  session_relay;
    ngx_msec_t                  push_reconnect;
    ngx_msec_t                  pull_reconnect;
    
    // RELAY_CACHE  转推缓存
    ngx_flag_t                          relay_cache;   // 是否开启转推 
    ngx_msec_t                          relay_cache_poll_len;   // 手动控制转推 的轮巡时长 
    ngx_str_t                           relay_cache_file;       // 手动控制转推 的标志文件位置
    ngx_event_t                         relay_cache_event;      // 监控手动转推是否开启
    ngx_flag_t                          relay_cache_ctrl;       // 监控到的转推状态
};

#define RELAY_CACHE_FILE_LEN 256


extern ngx_module_t  ngx_rtmp_live_module;


#endif /* _NGX_RTMP_LIVE_H_INCLUDED_ */
