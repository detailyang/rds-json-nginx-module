
/*
 * Copyright (C) agentzh
 */


#define DDEBUG 0
#include "ddebug.h"

#include "ngx_http_rds_json_filter_module.h"
#include "ngx_http_rds_json_util.h"
#include "ngx_http_rds_json_processor.h"

#include <ngx_config.h>


#define ngx_http_rds_json_content_type  "application/json"

static ngx_conf_enum_t  ngx_http_rds_json_formats[] = {
    { ngx_string("compact"), json_format_compact },
    { ngx_string("pretty"),  json_format_pretty },
    { ngx_null_string, 0 }
};


ngx_http_output_header_filter_pt  ngx_http_rds_json_next_header_filter;
ngx_http_output_body_filter_pt    ngx_http_rds_json_next_body_filter;


static ngx_int_t ngx_http_rds_json_ret_handler(ngx_http_request_t *r);

static char *ngx_http_rds_json_ret(ngx_conf_t *cf, ngx_command_t *cmd,
        void* conf);

static void *ngx_http_rds_json_create_conf(ngx_conf_t *cf);
static char *ngx_http_rds_json_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_rds_json_filter_init(ngx_conf_t *cf);


static ngx_command_t  ngx_http_rds_json_commands[] = {

    { ngx_string("rds_json"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF
          |NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
          |NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_rds_json_conf_t, enabled),
      NULL },

    { ngx_string("rds_json_format"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF
          |NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
          |NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_rds_json_conf_t, format),
      &ngx_http_rds_json_formats },

    { ngx_string("rds_json_content_type"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF
          |NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
          |NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_rds_json_conf_t, content_type),
      NULL },

    { ngx_string("rds_json_ret"),
      NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_2MORE,
      ngx_http_rds_json_ret,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_rds_json_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_rds_json_filter_init,         /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_rds_json_create_conf,         /* create location configuration */
    ngx_http_rds_json_merge_conf           /* merge location configuration */
};


ngx_module_t  ngx_http_rds_json_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_rds_json_filter_module_ctx,  /* module context */
    ngx_http_rds_json_commands,            /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_rds_json_header_filter(ngx_http_request_t *r)
{
    ngx_http_rds_json_ctx_t   *ctx;
    ngx_http_rds_json_conf_t  *conf;

    /* XXX maybe we can generate stub JSON strings like
     * {"errcode":403,"error":"Permission denied"}
     * for HTTP error pages? */
    if (r->headers_out.status != NGX_HTTP_OK) {
        dd("status is not OK: %d, skipping", r->headers_out.status);

        return ngx_http_rds_json_next_header_filter(r);
    }

    conf = ngx_http_get_module_loc_conf(r, ngx_http_rds_json_filter_module);

    if ( ! conf->enabled) {
        return ngx_http_rds_json_next_header_filter(r);
    }

    if (ngx_http_rds_json_test_content_type(r) != NGX_OK) {
        return ngx_http_rds_json_next_header_filter(r);
    }

    r->headers_out.content_type = conf->content_type;
    r->headers_out.content_type_len = conf->content_type.len;

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_rds_json_ctx_t));

    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ctx->tag = (ngx_buf_tag_t) &ngx_http_rds_json_filter_module;

    ctx->state = state_expect_header;

    /* set by ngx_pcalloc
     *      ctx->busy_bufs = NULL
     *      ctx->free_bufs = NULL
     *      ctx->col_names = NULL
     *      ctx->col_count = 0
     *      ctx->cur_col = 0
     *      ctx->field_offset = 0
     *      ctx->field_total = 0
     *      ctx->field_data_rest = 0
     */

    ngx_http_set_ctx(r, ctx, ngx_http_rds_json_filter_module);

    ngx_http_clear_content_length(r);

    r->filter_need_in_memory = 1;

    /* TODO: only send the header when we actually
     * see the RDS header */
    return ngx_http_rds_json_next_header_filter(r);
}


static ngx_int_t
ngx_http_rds_json_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_rds_json_ctx_t   *ctx;

    if (in == NULL || r->header_only) {
        return ngx_http_rds_json_next_body_filter(r, in);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_rds_json_filter_module);

    if (ctx == NULL) {
        return ngx_http_rds_json_next_body_filter(r, in);
    }

    switch (ctx->state) {
    case state_expect_header:
        return ngx_http_rds_json_process_header(r, in, ctx);
    case state_expect_col:
        return ngx_http_rds_json_process_col(r, in, ctx);
    case state_expect_row:
        return ngx_http_rds_json_process_row(r, in, ctx);
    case state_expect_field:
        return ngx_http_rds_json_process_field(r, in, ctx);
    case state_expect_more_field_data:
        return ngx_http_rds_json_process_more_field_data(r, in, ctx);

    case state_done:

        /* mark the remaining bufs as consumed */

        dd("discarding bufs");

        ngx_http_rds_json_discard_bufs(r->pool, in);

        return NGX_OK;

    default:
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                       "rds_json: invalid internal state: %d",
                       ctx->state);

        return NGX_ERROR;
    }

    /* impossible to reach here */
    return ngx_http_rds_json_next_body_filter(r, in);
}


static ngx_int_t
ngx_http_rds_json_filter_init(ngx_conf_t *cf)
{
    dd("setting next filter");
    ngx_http_rds_json_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_rds_json_header_filter;

    ngx_http_rds_json_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_rds_json_body_filter;

    return NGX_OK;
}


static void *
ngx_http_rds_json_create_conf(ngx_conf_t *cf)
{
    ngx_http_rds_json_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_rds_json_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->content_type = { 0, NULL };
     */

    conf->enabled = NGX_CONF_UNSET;
    conf->format = NGX_CONF_UNSET_UINT;

    return conf;
}


static char *
ngx_http_rds_json_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_rds_json_conf_t *prev = parent;
    ngx_http_rds_json_conf_t *conf = child;

    ngx_conf_merge_value(conf->enabled, prev->enabled, 0);

    ngx_conf_merge_uint_value(conf->format, prev->format, json_format_compact);

    ngx_conf_merge_str_value(conf->content_type, prev->content_type,
            ngx_http_rds_json_content_type);

    return NGX_CONF_OK;
}


static char *
ngx_http_rds_json_ret(ngx_conf_t *cf, ngx_command_t *cmd,
        void* conf)
{
    ngx_http_rds_json_conf_t            *jlcf = conf;
    ngx_http_core_loc_conf_t            *clcf;
    ngx_str_t                           *value;
    ngx_http_compile_complex_value_t     ccv;
    ngx_uint_t                           i;
    u_char                               c;

    value = cf->args->elts;

    jlcf->errcode = value[1];

    if (jlcf->errcode.len == 0) {
        ngx_log_error(NGX_LOG_ERR, cf->log, 0,
                "rds_json: rds_json_ret: the errcode argument is empty");

        return NGX_CONF_ERROR;
    }

    /* check the validaty of the errcode argument */
    for (i = 0; i < jlcf->errcode.len; i++) {
        c = jlcf->errcode.data[i];

        if (c < '0' || c > '9') {
            ngx_log_error(NGX_LOG_ERR, cf->log, 0,
                    "rds_json: rds_json_ret: invalid errcode argument: "
                    "\"%V\"", &jlcf->errcode);

            return NGX_CONF_ERROR;
        }
    }

    jlcf->errstr = ngx_palloc(cf->pool, sizeof(ngx_http_complex_value_t));
    if (jlcf->errstr == NULL) {
        return NGX_CONF_ERROR;
    }

    if (value[2].len == 0) {
        ngx_memzero(jlcf->errstr, sizeof(ngx_http_complex_value_t));
        return NGX_OK;
    }

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[2];
    ccv.complex_value = jlcf->errstr;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf,
            ngx_http_core_module);
    if (clcf == NULL) {
        return NGX_CONF_ERROR;
    }

    clcf->handler = ngx_http_rds_json_ret_handler;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_rds_json_ret_handler(ngx_http_request_t *r)
{
    ngx_chain_t                     *cl;
    ngx_buf_t                       *b;
    size_t                           len;
    ngx_http_rds_json_conf_t        *conf;
    ngx_str_t                        errstr;
    ngx_int_t                        rc;

    dd("entered ret handler");

    conf = ngx_http_get_module_loc_conf(r,
            ngx_http_rds_json_filter_module);

    /* evaluate the final value of conf->errstr */

    if (ngx_http_complex_value(r, conf->errstr, &errstr) != NGX_OK) {
        return NGX_ERROR;
    }

    /* calculate the buffer size */

    len = sizeof("{\"errcode\":") - 1
        + conf->errcode.len
        + sizeof("}") - 1
        ;

    if (errstr.len) {
        len += sizeof(",\"errstr\":\"") - 1
             + errstr.len
             + sizeof("\"") - 1
             ;
    }

    /* create the buffer */

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        return NGX_ERROR;
    }

    b->last_buf = 1;
    cl->buf = b;

    /* copy data over to the buffer */

    b->last = ngx_copy_const_str(b->last, "{\"errcode\":");

    b->last = ngx_copy(b->last, conf->errcode.data, conf->errcode.len);

    if (errstr.len) {
        b->last = ngx_copy_const_str(b->last, ",\"errstr\":\"");

        b->last = ngx_copy(b->last, errstr.data, errstr.len);

        *b->last++ = '"';
    }

    *b->last++ = '}';

    if (b->last != b->end) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "rds_json: rds_json_ret: buffer error");

        return NGX_ERROR;
    }

    /* send headers */

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_type = conf->content_type;
    r->headers_out.content_type_len = conf->content_type.len;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    dd("output filter...");

    return ngx_http_output_filter(r, cl);
}

