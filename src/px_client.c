#include "px_client.h"
#include <http_log.h>
#include <apr_strings.h>
#include <util_cookies.h>
#include <apr_base64.h>

#include "curl_pool.h"
#include "px_utils.h"
#include "px_types.h"

#ifdef APLOG_USE_MODULE
APLOG_USE_MODULE(perimeterx);
#endif

static const char *CLIENT_CONTENT_TYPE = "application/javascript";
static const char *XHR_CONTENT_TYPE = "application/json";
static const char *GIF_CONTENT_TYPE = "image/gif";
static const char *VID_OPT1 = "_pxvid";
static const char *VID_OPT2 = "vid";
static const char *CLIENT_URI = "/%s/main.min.js";
static const char *EMPTY_GIF = "R0lGODlhAQABAIAAAAAAAAAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==";

CURLcode post_request(const char *url, const char *payload, long timeout, px_config *conf, const request_context *ctx, char **response_data, double *request_rtt) {
    CURL *curl = curl_pool_get_wait(conf->curl_pool);
    if (curl == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, ctx->r->server, "[%s]: post_req_request: could not obtain curl handle", ctx->app_id);
        return CURLE_FAILED_INIT;
    }
    CURLcode status = post_request_helper(curl, url, payload, timeout, conf, ctx->r->server, response_data);
    if (request_rtt && (CURLE_OK != curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, request_rtt))) {
        *request_rtt = 0;
    }
    curl_pool_put(conf->curl_pool, curl);

    ap_log_error(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, ctx->r->server, "[%s]: post_req_request: post request payload  %s", ctx->app_id, payload);
    return status;
}

CURLcode forward_to_perimeterx(request_rec *r, px_config *conf, redirect_response *res, const char *base_url, const char *uri, const char *vid) {
    CURL *curl = curl_pool_get_wait(conf->redirect_curl_pool);
    if (curl == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, "[%s]: forward_to_perimeterx: could not obtain curl handle", conf->app_id);
        return CURLE_FAILED_INIT;
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r->server, "[%s]: forward_to_perimeterx: redirecting request", conf->app_id);
    CURLcode status = redirect_helper(curl, base_url, uri, vid, conf, r, &res->content, &res->response_headers, &res->content_size);
     // Return curl to pool
    curl_pool_put(conf->redirect_curl_pool, curl);
    return status;
}

const redirect_response *redirect_client(request_rec *r, px_config *conf) {
    redirect_response *res = apr_palloc(r->pool, sizeof(redirect_response));
    res->response_content_type = apr_pstrdup(r->pool, CLIENT_CONTENT_TYPE); 
    // Default content in case of failuer
    if (!conf->first_party_enabled) {
handle_default_client_response:
        res->content =  ""; 
        res->response_headers = NULL;
        res->content_size = 0;
        return res;    
    }

    const char *client_uri = apr_psprintf(r->pool, CLIENT_URI, conf->app_id);
    ap_log_error(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r->server, "[%s]:  redirect_client: forwarding request from %s to %s", conf->app_id,r->parsed_uri.path, client_uri);
    CURLcode status = forward_to_perimeterx(r, conf, res, conf->client_base_uri, client_uri, NULL);
    if (status != CURLE_OK) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r->server, "[%s]: redirect_client: response returned none 200 response, CURLcode[%d]", conf->app_id, status);
        goto handle_default_client_response; 
    }
    return res;    
};

const redirect_response *redirect_xhr(request_rec *r, px_config *conf) {
    redirect_response *res = apr_palloc(r->pool, sizeof(redirect_response));
 
    // Handle xhr/client featrue turned off
    if (!conf->first_party_enabled || !conf->first_party_enabled ) {
handle_default_xhr_response: 
        // Default values for xhr
        res->content = "{}"; 
        res->response_content_type = apr_pstrdup(r->pool,XHR_CONTENT_TYPE ); 
        res->content_size = 2;

        // Check if its a gif
        const char *file_ending = strrchr(r->uri, '.');
        if (file_ending && strcmp(file_ending, ".gif") == 0) {
            res->response_content_type = apr_pstrdup(r->pool, GIF_CONTENT_TYPE); 
            int gif_len = apr_base64_decode_len(EMPTY_GIF);
            char *decbuf = apr_palloc(r->pool, gif_len);
            // Verify that decoded b64 ok
            if (apr_base64_decode(decbuf, EMPTY_GIF) > 0) {
                res->content = decbuf;
                res->content_size = gif_len;
            } else {
                ap_log_error(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r->server, "[%s] Failed to decode b64 empty gif", conf->app_id);
                res->content = "";
                res->content_size = 1;
            }

        }
        return res;
    }

    int cut_prefix_size = strlen(conf->xhr_path_prefix);
    const char *xhr_url = apr_pstrdup(r->pool, &r->uri[cut_prefix_size]);
    ap_log_error(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r->server, "[%s] redirect_xhr: forwarding request from %s to %s", conf->app_id, r->parsed_uri.path, xhr_url);

    // Copy VID
    const char *vid = NULL;
    ap_cookie_read(r, VID_OPT1, &vid, 0);
    if (!vid) {
      ap_cookie_read(r, VID_OPT2, &vid, 0);
    }

    // Attach VID to request as cookie
    CURLcode status = forward_to_perimeterx(r, conf, res, conf->collector_base_uri, xhr_url, vid);
    if (status != CURLE_OK) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r->server, "[%s]: redirect_xhr: response returned none 200 response, CURLcode[%d]", conf->app_id, status);
        goto handle_default_xhr_response;
    }
    return res;
};
