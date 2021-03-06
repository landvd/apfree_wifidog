/* vim: set et sw=4 ts=4 sts=4 : */
/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
 *                                                                  *
 \********************************************************************/

/* $Id$ */
/** @file https_server.c
  @brief 
  @author Copyright (C) 2016 Dengfeng Liu <liudengfeng@kunteng.org>
  */

#include "https_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#ifndef S_ISDIR
#define S_ISDIR(x) (((x) & S_IFMT) == S_IFDIR)
#endif
#else
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>

#ifdef EVENT__HAVE_NETINET_IN_H
#include <netinet/in.h>
# ifdef _XOPEN_SOURCE_EXTENDED
#  include <arpa/inet.h>
# endif
#endif

#ifdef _WIN32
#define stat _stat
#define fstat _fstat
#define open _open
#define close _close
#define O_RDONLY _O_RDONLY
#endif

#include <syslog.h>

#include "https_server.h"
#include "debug.h"
#include "conf.h"
#include "gateway.h"
#include "wd_util.h"
#include "util.h"
#include "firewall.h"

// !!!remember to free the return url
char *
evhttp_get_request_url(struct evhttp_request *req) {
	const struct evhttp_uri *uri = evhttp_request_get_evhttp_uri(req);
	char url[256] = {0}; // only get 256 char from request url
	
	snprintf(url, 256, "https://%s%s",
		evhttp_request_get_host(req),
		evhttp_request_get_uri(req));
	
	debug (LOG_INFO, "url is %s", url);
	return evhttp_encode_uri(url);
}

// !!!remember to free the return redir_url
char *
evhttpd_get_full_redir_url(const char *mac, const char *ip, const char *orig_url) {
	struct evbuffer *evb = evbuffer_new();
	s_config *config = config_get_config();
	char *protocol = NULL;
    int port = 80;
    t_auth_serv *auth_server = get_auth_server();

    if (auth_server->authserv_use_ssl) {
        protocol = "https";
        port = auth_server->authserv_ssl_port;
    } else {
        protocol = "http";
        port = auth_server->authserv_http_port;
    }
	
	evbuffer_add_printf(evb, "%s://%s:%d%s%sgw_address=%s&gw_port=%d&gw_id=%s&channel_path=%s&ssid=%s&ip=%s&mac=%s&url=%s",
					protocol, auth_server->authserv_hostname, port, auth_server->authserv_path,
					auth_server->authserv_login_script_path_fragment,
					config->gw_address, config->gw_port, config->gw_id, 
					g_channel_path?g_channel_path:"null",
					g_ssid?g_ssid:"null",
					ip, mac, orig_url);
	

	char *redir_url = evb_2_string(evb);
	evbuffer_free(evb);
	
	return redir_url;
}

void
evhttpd_gw_reply(struct evhttp_request *req, struct evbuffer *evb) {
	evhttp_add_header(evhttp_request_get_output_headers(req),
		    "Content-Type", "text/html");
	evhttp_send_reply (req, 200, "OK", evb); 	
}

void
evhttp_gw_reply_js_redirect(struct evhttp_request *req, const char *peer_addr) {
	char *mac = (char *)arp_get(peer_addr);
	char *req_url = evhttp_get_request_url (req); 
	char *redir_url = evhttpd_get_full_redir_url(mac!=NULL?mac:"ff:ff:ff:ff:ff:ff", peer_addr, req_url);
	struct evbuffer *evb = evbuffer_new();
	struct evbuffer *evb_redir_url = evbuffer_new();
	
	debug (LOG_INFO, "Got a GET request for <%s> from <%s>\n", req_url, peer_addr);
	
		
	evbuffer_add(evb, wifidog_redir_html->front, wifidog_redir_html->front_len);
	evbuffer_add_printf(evb_redir_url, WIFIDOG_REDIR_HTML_CONTENT, redir_url);
	evbuffer_add_buffer(evb, evb_redir_url);
	evbuffer_add(evb, wifidog_redir_html->rear, wifidog_redir_html->rear_len);
	
	
	evhttpd_gw_reply(req, evb);
	
	free(mac);
	free(req_url);
	free(redir_url);
	evbuffer_free(evb);
	evbuffer_free(evb_redir_url);
}

static void
process_https_cb (struct evhttp_request *req, void *arg) {  			
	/* Determine peer */
	char *peer_addr;
	ev_uint16_t peer_port;
	struct evhttp_connection *con = evhttp_request_get_connection (req);
	evhttp_connection_get_peer (con, &peer_addr, &peer_port);
	
	if (!is_online()) {    
        debug(LOG_INFO, "Sent %s an apology since I am not online - no point sending them to auth server",
              peer_addr);
		evhttpd_gw_reply(req, evb_internet_offline_page);
    } else if (!is_auth_online()) {  
        debug(LOG_INFO, "Sent %s an apology since auth server not online - no point sending them to auth server",
              peer_addr);
		evhttpd_gw_reply(req, evb_authserver_offline_page);
    } else {
		evhttp_gw_reply_js_redirect(req, peer_addr);
	}
}

/**
 * This callback is responsible for creating a new SSL connection
 * and wrapping it in an OpenSSL bufferevent.  This is the way
 * we implement an https server instead of a plain old http server.
 */
static struct bufferevent* bevcb (struct event_base *base, void *arg) { 
	struct bufferevent* r;
  	SSL_CTX *ctx = (SSL_CTX *) arg;

  	r = bufferevent_openssl_socket_new (base,
                                      -1,
                                      SSL_new (ctx),
                                      BUFFEREVENT_SSL_ACCEPTING,
                                      BEV_OPT_CLOSE_ON_FREE);
  	return r;
}

static void server_setup_certs (SSL_CTX *ctx,
                                const char *certificate_chain,
                                const char *private_key) { 
  	if (1 != SSL_CTX_use_certificate_chain_file (ctx, certificate_chain))
    	die_most_horribly_from_openssl_error ("SSL_CTX_use_certificate_chain_file");

  	if (1 != SSL_CTX_use_PrivateKey_file (ctx, private_key, SSL_FILETYPE_PEM))
    	die_most_horribly_from_openssl_error ("SSL_CTX_use_PrivateKey_file");

  	if (1 != SSL_CTX_check_private_key (ctx))
    	die_most_horribly_from_openssl_error ("SSL_CTX_check_private_key");
}

static int serve_some_http (char *gw_ip,  t_https_server *https_server) { 
	struct event_base *base;
  	struct evhttp *http;
  	struct evhttp_bound_socket *handle;

  	base = event_base_new ();
  	if (! base) { 
		debug (LOG_ERR, "Couldn't create an event_base: exiting\n");
      	return 1;
    }

  	/* Create a new evhttp object to handle requests. */
  	http = evhttp_new (base);
  	if (! http) { 
		debug (LOG_ERR, "couldn't create evhttp. Exiting.\n");
      	return 1;
    }
 
 	SSL_CTX *ctx = SSL_CTX_new (SSLv23_server_method ());
  	SSL_CTX_set_options (ctx,
                       SSL_OP_SINGLE_DH_USE |
                       SSL_OP_SINGLE_ECDH_USE |
                       SSL_OP_NO_SSLv2);

	/* Cheesily pick an elliptic curve to use with elliptic curve ciphersuites.
	* We just hardcode a single curve which is reasonably decent.
	* See http://www.mail-archive.com/openssl-dev@openssl.org/msg30957.html */
	EC_KEY *ecdh = EC_KEY_new_by_curve_name (NID_X9_62_prime256v1);
	if (! ecdh)
    	die_most_horribly_from_openssl_error ("EC_KEY_new_by_curve_name");
  	if (1 != SSL_CTX_set_tmp_ecdh (ctx, ecdh))
    	die_most_horribly_from_openssl_error ("SSL_CTX_set_tmp_ecdh");

	server_setup_certs (ctx, https_server->svr_crt_file, https_server->svr_key_file);

	/* This is the magic that lets evhttp use SSL. */
	evhttp_set_bevcb (http, bevcb, ctx);
 
	/* This is the callback that gets called when a request comes in. */
	evhttp_set_gencb (http, process_https_cb, NULL);

	/* Now we tell the evhttp what port to listen on */
	handle = evhttp_bind_socket_with_handle (http, gw_ip, https_server->gw_https_port);
	if (! handle) { 
		debug (LOG_ERR, "couldn't bind to port %d. Exiting.\n",
               (int) https_server->gw_https_port);
		return 1;
    }
    
    event_base_dispatch (base);

  	/* not reached; runs forever */
  	return 0;
}

void thread_https_server(void *args) {
	s_config *config = config_get_config();
	common_setup ();              /* Initialize OpenSSL */
   	serve_some_http (config->gw_address, config->https_server);
}
