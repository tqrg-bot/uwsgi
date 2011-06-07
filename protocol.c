#include "uwsgi.h"

extern struct uwsgi_server uwsgi;

static size_t get_content_length(char *buf, uint16_t size) {
	int i;
	size_t val = 0;
	for(i=0;i<size;i++) {
		if (buf[i] >= '0' && buf[i] <= '9') {
			val = (val*10) + (buf[i] - '0');
			continue;
		}
		break;
	}

	return val;
}

void set_http_date(time_t t, char *dst) {

	static char  *week[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	static char  *months[] = {
				"Jan", "Feb", "Mar", "Apr",
				"May", "Jun", "Jul", "Aug",
				"Sep", "Oct", "Nov", "Dec"
			};

	struct tm *hdtm = gmtime(&t);
	snprintf(dst, 49, "Last-Modified: %s, %02d %s %4d %02d:%02d:%02d GMT\r\n\r\n",
		week[hdtm->tm_wday], hdtm->tm_mday,
		months[hdtm->tm_mon], hdtm->tm_year+1900,
		hdtm->tm_hour, hdtm->tm_min, hdtm->tm_sec);
}

// only RFC 1123 is supported
time_t parse_http_date(char *date, uint16_t len) {

	struct tm hdtm;

	if (len != 29 && date[3] != ',') return 0;

	hdtm.tm_mday = uwsgi_str2_num(date+5);

	switch(date[8]) {
		case 'J':
			if (date[9] == 'a') {
				hdtm.tm_mon = 0;
				break;
			}

			if (date[9] == 'u') {
				if (date[10] == 'n') {
					hdtm.tm_mon = 5;
					break;
				}

				if (date[10] == 'l') {
					hdtm.tm_mon = 6;
					break;
				}

				return 0;
			}

			return 0;

		case 'F':
			hdtm.tm_mon = 1;
			break;

		case 'M':
			if (date[9] != 'a') return 0;

			if (date[10] == 'r') {
				hdtm.tm_mon = 2;
				break;
			}

			if (date[10] == 'y') {
				hdtm.tm_mon = 4;
				break;
			}

			return 0;

		case 'A':
			if (date[10] == 'r') {
				hdtm.tm_mon = 3;
				break;
			}
			if (date[10] == 'g') {
				hdtm.tm_mon = 7;
				break;
			}
			return 0;

		case 'S':
			hdtm.tm_mon = 8;
			break;

		case 'O':
			hdtm.tm_mon = 9;
			break;

		case 'N':
			hdtm.tm_mon = 10;

		case 'D':
			hdtm.tm_mon = 11;
			break;
		default:
			return 0;
	}

	hdtm.tm_year = uwsgi_str4_num(date+12)-1900;

	hdtm.tm_hour = uwsgi_str2_num(date+17);
	hdtm.tm_min = uwsgi_str2_num(date+20);
	hdtm.tm_sec = uwsgi_str2_num(date+23);

	return timegm(&hdtm);
	
}

#ifdef UWSGI_UDP
ssize_t send_udp_message(uint8_t modifier1, char *host, char *message, uint16_t message_size) {

	int fd;
	struct sockaddr_in udp_addr;
	char *udp_port;
	ssize_t ret;
	char udpbuff[1024];

	if (message_size + 4 > 1024)
		return -1;

	udp_port = strchr(host, ':');
	if (udp_port == NULL) {
		return -1;
	}

	udp_port[0] = 0; 

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		uwsgi_error("socket()");
		return -1;
	}

	memset(&udp_addr, 0, sizeof(struct sockaddr_in));
	udp_addr.sin_family = AF_INET;
	udp_addr.sin_port = htons(atoi(udp_port+1));
	udp_addr.sin_addr.s_addr = inet_addr(host);

	udpbuff[0] = modifier1;
#ifdef __BIG_ENDIAN__
	message_size = uwsgi_swap16(message_size);
#endif

	memcpy(udpbuff+1, &message_size, 2);

	udpbuff[3] = 0;

#ifdef __BIG_ENDIAN__
	message_size = uwsgi_swap16(message_size);
#endif

	memcpy(udpbuff+4, message, message_size);

	ret = sendto(fd, udpbuff, message_size+4, 0, (struct sockaddr *) &udp_addr, sizeof(udp_addr));
	if (ret < 0) {
		uwsgi_error("sendto()");
	}
	close(fd);

	return ret;
	
}
#endif

int uwsgi_enqueue_message(char *host, int port, uint8_t modifier1, uint8_t modifier2, char *message, int size, int timeout) {

	struct pollfd uwsgi_poll;
	struct sockaddr_in uws_addr;
	int cnt;
	struct uwsgi_header uh;

	if (!timeout)
		timeout = 1;

	if (size > 0xffff) {
		uwsgi_log( "invalid object (marshalled) size\n");
		return -1;
	}

	uwsgi_poll.fd = socket(AF_INET, SOCK_STREAM, 0);
	if (uwsgi_poll.fd < 0) {
		uwsgi_error("socket()");
		return -1;
	}

	memset(&uws_addr, 0, sizeof(struct sockaddr_in));
	uws_addr.sin_family = AF_INET;
	uws_addr.sin_port = htons(port);
	uws_addr.sin_addr.s_addr = inet_addr(host);

	uwsgi_poll.events = POLLIN;

	if (timed_connect(&uwsgi_poll, (const struct sockaddr *) &uws_addr, sizeof(struct sockaddr_in), timeout, 0)) {
		uwsgi_error("connect()");
		close(uwsgi_poll.fd);
		return -1;
	}

	uh.modifier1 = modifier1;
	uh.pktsize = (uint16_t) size;
	uh.modifier2 = modifier2;

	cnt = write(uwsgi_poll.fd, &uh, 4);
	if (cnt != 4) {
		uwsgi_error("write()");
		close(uwsgi_poll.fd);
		return -1;
	}

	cnt = write(uwsgi_poll.fd, message, size);
	if (cnt != size) {
		uwsgi_error("write()");
		close(uwsgi_poll.fd);
		return -1;
	}

	return uwsgi_poll.fd;
}



ssize_t uwsgi_send_message(int fd, uint8_t modifier1, uint8_t modifier2, char *message, uint16_t size, int pfd, ssize_t plen, int timeout) {

	struct pollfd uwsgi_mpoll;
	ssize_t cnt;
	struct uwsgi_header uh;
	char buffer[4096];
	ssize_t ret = 0;
	int pret;
	struct msghdr msg;
	struct iovec  iov [1];
	union {
		struct cmsghdr cmsg;
		char control [CMSG_SPACE (sizeof (int))];
	} msg_control;
	struct cmsghdr *cmsg;

	if (!timeout) timeout = uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT];

	uh.modifier1 = modifier1;
	uh.pktsize = size;
	uh.modifier2 = modifier2;

	if (pfd >= 0 && plen == -1) {
		// pass the fd
		iov[0].iov_base = &uh;
		iov[0].iov_len = 4;
		
		msg.msg_name    = NULL;
		msg.msg_namelen = 0;
		msg.msg_iov     = iov;
		msg.msg_iovlen  = 1;
		msg.msg_flags   = 0;

		msg.msg_control    = &msg_control;
		msg.msg_controllen = sizeof (msg_control);

		cmsg = CMSG_FIRSTHDR (&msg);
		cmsg->cmsg_len   = CMSG_LEN (sizeof (int));
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type  = SCM_RIGHTS;

		memcpy(CMSG_DATA(cmsg), &pfd, sizeof(int));
		
		uwsgi_log("passing fd\n");
		cnt = sendmsg(fd, &msg, 0);	
	}
	else {
		cnt = write(fd, &uh, 4);
	}
	if (cnt != 4) {
		uwsgi_error("write()");
		return -1;
	}

	ret += cnt;

	cnt = write(fd, message, size);
	if (cnt != size) {
		uwsgi_error("write()");
		return -1;
	}

	ret += cnt;

	// transfer data from one socket to another
	if (pfd >= 0 && plen > 0) {
		uwsgi_mpoll.fd = pfd;
		uwsgi_mpoll.events = POLLIN;
		
		while(plen > 0) {
			pret = poll(&uwsgi_mpoll, 1, timeout*1000);
			if (pret < 0) {
				uwsgi_error("poll()");
				return -1;
			}
			else if (pret == 0) {
				uwsgi_log("timeout waiting for socket data\n");
				return -1;
			}
			else {
				cnt = read(pfd, buffer, UMIN(4096, plen));
				if (cnt < 0) {
					uwsgi_error("read()");
					return -1;
				}
				else if (cnt == 0) {
					return ret;
				}	
				// send to peer
				if (write(fd, buffer, cnt) != cnt) {
					uwsgi_error("write()");
					return -1;
				}
				ret += cnt;
				plen -= cnt;	
			}
		}
	}


	return ret;
}


int uwsgi_parse_response(struct pollfd *upoll, int timeout, struct uwsgi_header *uh, char *buffer, int (*socket_proto)(struct wsgi_request *)) {
	int rlen;
	int status = UWSGI_AGAIN;

	if (!timeout)
		timeout = 1;

	while(status == UWSGI_AGAIN) {
		rlen = poll(upoll, 1, timeout * 1000);
		if (rlen < 0) {
			uwsgi_error("poll()");
			exit(1);
		}
		else if (rlen == 0) {
			uwsgi_log( "timeout waiting for header. skip request.\n");
			//close(upoll->fd);
			return 0;
		}
		status = socket_proto((struct wsgi_request *) uh);
		if (status < 0) {
			uwsgi_log("error parsing request\n");
			//close(upoll->fd);
			return 0;
		}
	}

	return 1;
}

int uwsgi_parse_array(char *buffer, uint16_t size, char **argv, uint8_t *argc) {

	char *ptrbuf, *bufferend;
	uint16_t strsize = 0;
	
	uint8_t max = *argc;
	*argc = 0;

        ptrbuf = buffer;
        bufferend = ptrbuf + size;

	while (ptrbuf < bufferend && *argc < max) {
                if (ptrbuf + 2 < bufferend) {
                        memcpy(&strsize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
                        strsize = uwsgi_swap16(strsize);
#endif

                        ptrbuf += 2;
                        /* item cannot be null */
                        if (!strsize) continue;

                        if (ptrbuf + strsize <= bufferend) {
                                // item
				argv[*argc] = uwsgi_cheap_string(ptrbuf, strsize);
                                ptrbuf += strsize;
				*argc = *argc + 1;
			}
			else {
				uwsgi_log( "invalid uwsgi array. skip this request.\n");
                        	return -1;
			}
		}
		else {
			uwsgi_log( "invalid uwsgi array. skip this request.\n");
                        return -1;
		}
	}
	

	return 0;
}

int uwsgi_parse_vars(struct wsgi_request *wsgi_req) {

	char *buffer = wsgi_req->buffer;

	char *ptrbuf, *bufferend;

	uint16_t strsize = 0;

	ptrbuf = buffer;
	bufferend = ptrbuf + wsgi_req->uh.pktsize;
	int i, script_name= -1, path_info= -1;

	/* set an HTTP 500 status as default */
	wsgi_req->status = 500;

	while (ptrbuf < bufferend) {
		if (ptrbuf + 2 < bufferend) {
			memcpy(&strsize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
			strsize = uwsgi_swap16(strsize);
#endif
			/* key cannot be null */
                        if (!strsize) {
                                uwsgi_log( "uwsgi key cannot be null. skip this request.\n");
                                return -1;
                        }
			
			ptrbuf += 2;
			if (ptrbuf + strsize < bufferend) {
				// var key
				wsgi_req->hvec[wsgi_req->var_cnt].iov_base = ptrbuf;
				wsgi_req->hvec[wsgi_req->var_cnt].iov_len = strsize;
				ptrbuf += strsize;
				// value can be null (even at the end) so use <=
				if (ptrbuf + 2 <= bufferend) {
					memcpy(&strsize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
					strsize = uwsgi_swap16(strsize);
#endif
					ptrbuf += 2;
					if (ptrbuf + strsize <= bufferend) {
						//uwsgi_log("uwsgi %.*s = %.*s\n", wsgi_req->hvec[wsgi_req->var_cnt].iov_len, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, strsize, ptrbuf);
						if (!uwsgi_strncmp("SCRIPT_NAME", 11, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->script_name = ptrbuf;
							wsgi_req->script_name_len = strsize;
							script_name = wsgi_req->var_cnt + 1;
#ifdef UWSGI_DEBUG
							uwsgi_debug("SCRIPT_NAME=%.*s\n", wsgi_req->script_name_len, wsgi_req->script_name);
#endif
						}
						else if (!uwsgi_strncmp("PATH_INFO", 9, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->path_info = ptrbuf;
							wsgi_req->path_info_len = strsize;
							path_info = wsgi_req->var_cnt + 1;
#ifdef UWSGI_DEBUG
							uwsgi_debug("PATH_INFO=%.*s\n", wsgi_req->path_info_len, wsgi_req->path_info);
#endif
						}
						else if (!uwsgi_strncmp("SERVER_PROTOCOL", 15, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->protocol = ptrbuf;
							wsgi_req->protocol_len = strsize;
						}
						else if (!uwsgi_strncmp("REQUEST_URI", 11, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->uri = ptrbuf;
							wsgi_req->uri_len = strsize;
						}
						else if (!uwsgi_strncmp("QUERY_STRING", 12, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->query_string = ptrbuf;
							wsgi_req->query_string_len = strsize;
						}
						else if (!uwsgi_strncmp("REQUEST_METHOD", 14, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->method = ptrbuf;
							wsgi_req->method_len = strsize;
						}
						else if (!uwsgi_strncmp("REMOTE_ADDR", 11, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->remote_addr = ptrbuf;
							wsgi_req->remote_addr_len = strsize;
						}
						else if (!uwsgi_strncmp("REMOTE_USER", 11, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->remote_user = ptrbuf;
							wsgi_req->remote_user_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_SCHEME", 12, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->scheme = ptrbuf;
							wsgi_req->scheme_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_SCRIPT", 12, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len )) {
							wsgi_req->script = ptrbuf;
							wsgi_req->script_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_MODULE", 12, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->module = ptrbuf;
							wsgi_req->module_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_CALLABLE", 14, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->callable = ptrbuf;
							wsgi_req->callable_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_PYHOME", 12, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->pyhome = ptrbuf;
							wsgi_req->pyhome_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_CHDIR", 11, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->chdir = ptrbuf;
							wsgi_req->chdir_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_FILE", 10, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->file = ptrbuf;
							wsgi_req->file_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_TOUCH_RELOAD", 18, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->touch_reload = ptrbuf;
							wsgi_req->touch_reload_len = strsize;
						}
						else if (uwsgi.cache_max_items > 0 && !uwsgi_strncmp("UWSGI_CACHE_GET", 15, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->cache_get = ptrbuf;
							wsgi_req->cache_get_len = strsize;
						}
						else if (!uwsgi_strncmp("UWSGI_SETENV", 12, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							char *env_value = memchr(ptrbuf, '=', strsize);
							if (env_value) {
								env_value[0] = 0;
								env_value = uwsgi_concat2n(env_value+1, strsize-((env_value+1)-ptrbuf), "", 0);
								if (setenv(ptrbuf, env_value, 1)) {
									uwsgi_error("setenv()");
								}
								free(env_value);
							}
						}
						else if (!uwsgi_strncmp("SERVER_NAME", 11, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len) && !uwsgi.vhost_host) {
							wsgi_req->host = ptrbuf;
							wsgi_req->host_len = strsize;
#ifdef UWSGI_DEBUG
							uwsgi_debug("SERVER_NAME=%.*s\n", wsgi_req->host_len, wsgi_req->host);
#endif
						}
						else if (!uwsgi_strncmp("HTTP_HOST", 9, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len) && uwsgi.vhost_host) {
							wsgi_req->host = ptrbuf;
							wsgi_req->host_len = strsize;
#ifdef UWSGI_DEBUG
							uwsgi_debug("HTTP_HOST=%.*s\n", wsgi_req->host_len, wsgi_req->host);
#endif
						}
						else if (!uwsgi_strncmp("HTTPS", 5, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->https = ptrbuf;
							wsgi_req->https_len = strsize;
						}
						else if (!uwsgi_strncmp("HTTP_IF_MODIFIED_SINCE", 22, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->if_modified_since = ptrbuf;
							wsgi_req->if_modified_since_len = strsize;
						}
						else if (!uwsgi_strncmp("CONTENT_LENGTH", 14, wsgi_req->hvec[wsgi_req->var_cnt].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len)) {
							wsgi_req->post_cl = get_content_length(ptrbuf, strsize);
							if (uwsgi.limit_post) {
                						if (wsgi_req->post_cl > uwsgi.limit_post) {
                        						uwsgi_log("Invalid (too big) CONTENT_LENGTH. skip.\n");
                        						return -1;
                						}
        						}

						}

						if (wsgi_req->var_cnt < uwsgi.vec_size - (4 + 1)) {
							wsgi_req->var_cnt++;
						}
						else {
							uwsgi_log( "max vec size reached. skip this header.\n");
							return -1;
						}
						// var value
						wsgi_req->hvec[wsgi_req->var_cnt].iov_base = ptrbuf;
						wsgi_req->hvec[wsgi_req->var_cnt].iov_len = strsize;
						//uwsgi_log("%.*s = %.*s\n", wsgi_req->hvec[wsgi_req->var_cnt-1].iov_len, wsgi_req->hvec[wsgi_req->var_cnt-1].iov_base, wsgi_req->hvec[wsgi_req->var_cnt].iov_len, wsgi_req->hvec[wsgi_req->var_cnt].iov_base);
						if (wsgi_req->var_cnt < uwsgi.vec_size - (4 + 1)) {
							wsgi_req->var_cnt++;
						}
						else {
							uwsgi_log( "max vec size reached. skip this header.\n");
							return -1;
						}
						ptrbuf += strsize;
					}
					else {
						uwsgi_log("invalid uwsgi request (current strsize: %d). skip.\n", strsize);
						return -1;
					}
				}
				else {
					uwsgi_log("invalid uwsgi request (current strsize: %d). skip.\n", strsize);
					return -1;
				}
			}
		}
		else {
			uwsgi_log("invalid uwsgi request (current strsize: %d). skip.\n", strsize);
			return -1;
		}
	}

	if (uwsgi.post_buffering > 0 && !wsgi_req->body_as_file && !wsgi_req->async_post) {
        	// read to disk if post_cl > post_buffering (it will eventually do upload progress...)
                if (wsgi_req->post_cl >= (size_t) uwsgi.post_buffering) {
                	if (!uwsgi_read_whole_body(wsgi_req, wsgi_req->post_buffering_buf, uwsgi.post_buffering_bufsize)) {
				wsgi_req->status = -1;
				return -1;	
                        }
			wsgi_req->body_as_file = 1;
		}
                // on tiny post use memory
                else {
                	if (!uwsgi_read_whole_body_in_mem(wsgi_req, wsgi_req->post_buffering_buf)) {
				wsgi_req->status = -1;
				return -1;	
                        }
		}
	}


	// check if data are available in the local cache
	if (wsgi_req->cache_get_len > 0) {
		uint64_t cache_value_size;
		char *cache_value = uwsgi_cache_get(wsgi_req->cache_get, wsgi_req->cache_get_len, &cache_value_size);
		if (cache_value && cache_value_size > 0) {
			wsgi_req->response_size = wsgi_req->socket->proto_write(wsgi_req, cache_value, cache_value_size);
			wsgi_req->status = -1;
			return -1;
		}
	}

	if (uwsgi.check_cache && wsgi_req->uri_len && wsgi_req->method_len == 3 &&
		wsgi_req->method[0] == 'G' && wsgi_req->method[1] == 'E' && wsgi_req->method[2] == 'T') {

		uint64_t cache_value_size;
		char *cache_value = uwsgi_cache_get(wsgi_req->uri, wsgi_req->uri_len, &cache_value_size);
		if (cache_value && cache_value_size > 0) {
			wsgi_req->response_size = wsgi_req->socket->proto_write(wsgi_req, cache_value, cache_value_size);
			wsgi_req->status = -1;
			return -1;
		}
	}

	if (uwsgi.manage_script_name) {
		if (uwsgi.apps_cnt > 0 && wsgi_req->path_info_len > 1) {
			// starts with 1 as the 0 app is the default (/) one
			int best_found = 0;
			char *orig_path_info = wsgi_req->path_info;
			int orig_path_info_len = wsgi_req->path_info_len;
			// if SCRIPT_NAME is not allocated, add a slot for it
			if (script_name == -1) {
				if (wsgi_req->var_cnt >= uwsgi.vec_size - (4 + 2)) {
					uwsgi_log( "max vec size reached. skip this header.\n");
                                        return -1;
				}
				wsgi_req->var_cnt++;
				wsgi_req->hvec[wsgi_req->var_cnt].iov_base = "SCRIPT_NAME";
                                wsgi_req->hvec[wsgi_req->var_cnt].iov_len = 11;
				wsgi_req->var_cnt++;
				script_name = wsgi_req->var_cnt;
			}
			for(i=0;i<uwsgi.apps_cnt;i++) {
				//uwsgi_log("app mountpoint = %.*s\n", uwsgi.apps[i].mountpoint_len, uwsgi.apps[i].mountpoint);
				if (orig_path_info_len >= uwsgi.apps[i].mountpoint_len) {
					if (!uwsgi_startswith(orig_path_info, uwsgi.apps[i].mountpoint, uwsgi.apps[i].mountpoint_len) && uwsgi.apps[i].mountpoint_len > best_found) {
						best_found = uwsgi.apps[i].mountpoint_len;
						wsgi_req->script_name = uwsgi.apps[i].mountpoint;
						wsgi_req->script_name_len = uwsgi.apps[i].mountpoint_len;
						wsgi_req->path_info = orig_path_info+wsgi_req->script_name_len;
						wsgi_req->path_info_len = orig_path_info_len-wsgi_req->script_name_len;

						wsgi_req->hvec[script_name].iov_base = wsgi_req->script_name;
						wsgi_req->hvec[script_name].iov_len = wsgi_req->script_name_len;

						wsgi_req->hvec[path_info].iov_base = wsgi_req->path_info;
						wsgi_req->hvec[path_info].iov_len = wsgi_req->path_info_len;
#ifdef UWSGI_DEBUG
						uwsgi_log("managed SCRIPT_NAME = %.*s PATH_INFO = %.*s\n", wsgi_req->script_name_len, wsgi_req->script_name, wsgi_req->path_info_len, wsgi_req->path_info);
#endif
					} 
				}
			}
		}
	}

	// check if a file named uwsgi.check_static+env['PATH_INFO'] exists
	if (uwsgi.check_static && wsgi_req->path_info_len > 1) {
		if (!uwsgi_file_serve(wsgi_req, uwsgi.check_static, uwsgi.check_static_len, wsgi_req->path_info, wsgi_req->path_info_len, uwsgi.check_static, uwsgi.check_static_len)) {
			return -1;
		}
	}

	// check static-map
	struct uwsgi_static_map *usm = uwsgi.static_maps;
	while(usm) {
#ifdef UWSGI_DEBUG
		uwsgi_log("checking for %.*s <-> %.*s\n", wsgi_req->path_info_len, wsgi_req->path_info, usm->mountpoint_len, usm->mountpoint);
#endif

		if (!uwsgi_starts_with(wsgi_req->path_info, wsgi_req->path_info_len, usm->mountpoint, usm->mountpoint_len)) {
			if (!uwsgi_file_serve(wsgi_req, usm->document_root, usm->document_root_len, wsgi_req->path_info+usm->mountpoint_len, wsgi_req->path_info_len-usm->mountpoint_len, usm->orig_document_root, usm->orig_document_root_len)) {
				return -1;
			}
		}
		usm = usm->next;
	}

	return 0;
}

int uwsgi_ping_node(int node, struct wsgi_request *wsgi_req) {


	struct pollfd uwsgi_poll;

	struct uwsgi_cluster_node *ucn = &uwsgi.shared->nodes[node];

	if (ucn->name[0] == 0) {
		return 0;
	}

	if (ucn->status == UWSGI_NODE_OK) {
		return 0;
	}

	uwsgi_poll.fd = socket(AF_INET, SOCK_STREAM, 0);
	if (uwsgi_poll.fd < 0) {
		uwsgi_error("socket()");
		return -1;
	}

	if (timed_connect(&uwsgi_poll, (const struct sockaddr *) &ucn->ucn_addr, sizeof(struct sockaddr_in), uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT], 0)) {
		close(uwsgi_poll.fd);
		return -1;
	}

	wsgi_req->uh.modifier1 = UWSGI_MODIFIER_PING;
	wsgi_req->uh.pktsize = 0;
	wsgi_req->uh.modifier2 = 0;
	if (write(uwsgi_poll.fd, wsgi_req, 4) != 4) {
		uwsgi_error("write()");
		return -1;
	}

	uwsgi_poll.events = POLLIN;
	if (!uwsgi_parse_response(&uwsgi_poll, uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT], (struct uwsgi_header *) wsgi_req, wsgi_req->buffer, uwsgi_proto_uwsgi_parser)) {
		return -1;
	}

	return 0;
}

ssize_t uwsgi_send_empty_pkt(int fd, char *socket_name, uint8_t modifier1, uint8_t modifier2) {

	struct uwsgi_header uh;
	char *port;
	uint16_t s_port;
	struct sockaddr_in uaddr;
	int ret;

	uh.modifier1 = modifier1;
	uh.pktsize = 0;
	uh.modifier2 = modifier2;

	if (socket_name) {
		port = strchr(socket_name, ':');
		if (!port) return -1;
		s_port = atoi(port+1);
		port[0] = 0;
		memset(&uaddr, 0, sizeof(struct sockaddr_in));
		uaddr.sin_family = AF_INET;
		uaddr.sin_addr.s_addr = inet_addr(socket_name);
		uaddr.sin_port = htons(s_port);

		port[0] = ':';

		ret = sendto(fd, &uh, 4, 0, (struct sockaddr *) &uaddr, sizeof(struct sockaddr_in));
	}
	else {
		ret = send(fd, &uh, 4, 0);
	}

	if (ret < 0) {
		uwsgi_error("sendto()");
	}

	return ret;
}

int uwsgi_get_dgram(int fd, struct wsgi_request *wsgi_req) {

	ssize_t rlen;
	struct uwsgi_header *uh;
	static char *buffer = NULL;

	struct sockaddr_in sin;
	socklen_t sin_len = sizeof(struct sockaddr_in);

	if (!buffer) {
		buffer = uwsgi_malloc(uwsgi.buffer_size + 4);
	}
		

	rlen = recvfrom(fd, buffer, uwsgi.buffer_size + 4, 0, (struct sockaddr *) &sin, &sin_len);

        if (rlen < 0) {
                uwsgi_error("recvfrom");
                return -1;
        }

	uwsgi_log("recevied request from %s\n", inet_ntoa(sin.sin_addr));

        if (rlen < 4) {
                uwsgi_log("invalid uwsgi packet\n");
                return -1;
        }

	uh = (struct uwsgi_header *) buffer;

	wsgi_req->uh.modifier1 = uh->modifier1;
	/* big endian ? */
#ifdef __BIG_ENDIAN__
	uh->pktsize = uwsgi_swap16(uh->pktsize);
#endif
	wsgi_req->uh.pktsize = uh->pktsize;
	wsgi_req->uh.modifier2 = uh->modifier2;

	if (wsgi_req->uh.pktsize > uwsgi.buffer_size) {
		uwsgi_log("invalid uwsgi packet size, probably you need to increase buffer size\n");
		return -1;
	}

	wsgi_req->buffer = buffer+4;

	uwsgi_log("request received %d %d\n", wsgi_req->uh.modifier1, wsgi_req->uh.modifier2);

	return 0;

}

int uwsgi_hooked_parse(char *buffer, size_t len, void (*hook)(char *, uint16_t, char *, uint16_t, void*), void *data) {

	char *ptrbuf, *bufferend;
        uint16_t keysize = 0, valsize = 0;
        char *key;

	ptrbuf = buffer;
	bufferend = buffer + len;

	while (ptrbuf < bufferend) {
                if (ptrbuf + 2 >= bufferend) return -1;
                memcpy(&keysize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
                keysize = uwsgi_swap16(keysize);
#endif
                /* key cannot be null */
                if (!keysize)  return -1;

                ptrbuf += 2;
                if (ptrbuf + keysize > bufferend) return -1;

                // key
                key = ptrbuf;
                ptrbuf += keysize;
                // value can be null
                if (ptrbuf + 2 > bufferend) return -1;

                memcpy(&valsize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
                valsize = uwsgi_swap16(valsize);
#endif
                ptrbuf += 2;
                if (ptrbuf + valsize > bufferend) return -1;

                // now call the hook
                hook(key, keysize, ptrbuf, valsize, data);
                ptrbuf += valsize;
        }

        return 0;

}

int uwsgi_hooked_parse_dict_dgram(int fd, char *buffer, size_t len, uint8_t modifier1, uint8_t modifier2, void (*hook)(char *, uint16_t, char *, uint16_t, void *), void *data) {

	struct uwsgi_header *uh;
	ssize_t rlen;

	struct sockaddr_in sin;
	socklen_t sin_len = sizeof(struct sockaddr_in);

	char *ptrbuf, *bufferend;

	rlen = recvfrom(fd, buffer, len, 0, (struct sockaddr *) &sin, &sin_len);

	if (rlen < 0) {
		uwsgi_error("recvfrom()");
		return -1;
	}

	uwsgi_log("recevied request from %s\n", inet_ntoa(sin.sin_addr));

	uwsgi_log("RLEN: %d\n", rlen);

	// check for valid dict 4(header) 2(non-zero key)+1 2(value)
	if (rlen < (4+2+1+2)) {
		uwsgi_log("invalid uwsgi dictionary\n");
		return -1;
	}
	
	uh = (struct uwsgi_header *) buffer;

	if (uh->modifier1 != modifier1 || uh->modifier2 != modifier2) {
		uwsgi_log("invalid uwsgi dictionary received, modifier1: %d modifier2: %d\n", uh->modifier1, uh->modifier2);
		return -1;
	}

        ptrbuf = buffer + 4;

	/* big endian ? */
#ifdef __BIG_ENDIAN__
	uh->pktsize = uwsgi_swap16(uh->pktsize);
#endif

	if (uh->pktsize > len) {
		uwsgi_log("* WARNING * the uwsgi dictionary received is too big, data will be truncated\n");
		bufferend = ptrbuf + len;
	}
	else {
        	bufferend = ptrbuf + uh->pktsize;
	}

	
	uwsgi_log("%p %p %d\n", ptrbuf, bufferend, bufferend-ptrbuf);

	uwsgi_hooked_parse(ptrbuf, bufferend-ptrbuf, hook, data);

	return 0;

}

int uwsgi_string_sendto(int fd, uint8_t modifier1, uint8_t modifier2, struct sockaddr *sa, socklen_t sa_len, char *message, size_t len) {

	ssize_t rlen ;
	struct uwsgi_header *uh;
	char *upkt = uwsgi_malloc(len + 4);

	uh = (struct uwsgi_header *) upkt;

	uh->modifier1 = modifier1;
	uh->pktsize = len;
#ifdef __BIG_ENDIAN__
	uh->pktsize = uwsgi_swap16(uh->pktsize);
#endif
	uh->modifier2 = modifier2;

	memcpy(upkt+4, message, len);

	rlen = sendto(fd, upkt, len+4, 0, sa, sa_len);

	if (rlen < 0) {
		uwsgi_error("sendto()");
	}

	free(upkt);

	return rlen;
}

ssize_t fcgi_send_param(int fd, char *key, uint16_t keylen, char *val, uint16_t vallen) {

	struct fcgi_record fr;
	struct iovec iv[5];

	uint8_t ks1 = 0;
	uint32_t ks4 = 0;

	uint8_t vs1 = 0;
	uint32_t vs4 = 0;

	uint16_t size = keylen+vallen;

	if (keylen > 127) {
		size += 4;
		ks4 = htonl(keylen) | 0x80000000;
		iv[1].iov_base = &ks4;
		iv[1].iov_len = 4;
	}
	else {
		size += 1;
		ks1 = keylen;
		iv[1].iov_base = &ks1;
		iv[1].iov_len = 1;
	}

	if (vallen > 127) {
		size += 4;
		vs4 = htonl(vallen) | 0x80000000;
		iv[2].iov_base = &vs4;
		iv[2].iov_len = 4;
	}
	else {
		size += 1;
		vs1 = vallen;
		iv[2].iov_base = &vs1;
		iv[2].iov_len = 1;
	}

	iv[3].iov_base = key;
	iv[3].iov_len = keylen;
	iv[4].iov_base = val;
	iv[4].iov_len = vallen;

	fr.version = 1;
	fr.type = 4;
	fr.req1 = 0;
	fr.req0 = 1;
	fr.cl8.cl1 = (uint8_t) ((size >> 8) & 0xff);
	fr.cl8.cl0 = (uint8_t) (size &0xff);
	fr.pad = 0;
	fr.reserved = 0;

	iv[0].iov_base = &fr;
	iv[0].iov_len = 8;

	return writev(fd, iv, 5);
	
}

ssize_t fcgi_send_record(int fd, uint8_t type, uint16_t size, char *buffer) {

	struct fcgi_record fr;
	struct iovec iv[2];

	fr.version = 1;
	fr.type = type;
	fr.req1 = 0;
	fr.req0 = 1;
	fr.cl8.cl1 = (uint8_t) ((size >> 8) & 0xff);
	fr.cl8.cl0 = (uint8_t) (size &0xff);
	fr.pad = 0;
	fr.reserved = 0;

	iv[0].iov_base = &fr;
	iv[0].iov_len = 8;

	iv[1].iov_base = buffer;
	iv[1].iov_len = size;

	return writev(fd, iv, 2);

}

uint16_t fcgi_get_record(int fd, char *buf) {

	struct fcgi_record fr;
	uint16_t remains = 8;
	char *ptr = (char *) &fr;
	ssize_t len;

        while(remains) {
        	uwsgi_waitfd(fd, -1);
                len = read(fd, ptr, remains);
                if (len <= 0) return 0;
                remains -= len;
                ptr += len;
        }

        remains = ntohs(fr.cl) + fr.pad;
        ptr = buf;

        while(remains) {
        	uwsgi_waitfd(fd, -1);
                len = read(fd, ptr, remains);
                if (len <= 0) return 0;
                remains -= len;
                ptr += len;
        }

	if (fr.type != 6) return 0;

	return ntohs(fr.cl);

}

char *uwsgi_simple_message_string(char *socket_name, uint8_t modifier1, uint8_t modifier2, char *what, uint16_t what_len, char *buffer, uint16_t *response_len, int timeout) {

	struct uwsgi_header uh;
	struct pollfd upoll;

	int fd = uwsgi_connect(socket_name, timeout, 0);

	if (fd < 0) {
		if (response_len) *response_len = 0;
		return NULL;
	}

	if (uwsgi_send_message(fd, modifier1, modifier2, what, what_len, -1, 0, timeout) <= 0) {
		close(fd);
		if (response_len) *response_len = 0;
		return NULL;
	}

	upoll.fd = fd;
	upoll.events = POLLIN;

	if (buffer) {
		if (!uwsgi_parse_response(&upoll, timeout, &uh, buffer, uwsgi_proto_uwsgi_parser)) {
			close(fd);
			if (response_len) *response_len = 0;
			return NULL;
		}

		if (response_len) *response_len = uh.pktsize;
	}

	close(fd);
	return buffer;
}

int uwsgi_simple_send_string2(char *socket_name, uint8_t modifier1, uint8_t modifier2, char *item1, uint16_t item1_len, char *item2, uint16_t item2_len, int timeout) {

	struct uwsgi_header uh;
	char strsize1[2], strsize2[2];

	struct iovec iov[5];

	int fd = uwsgi_connect(socket_name, timeout, 0);

        if (fd < 0) {
                return -1;
        }

	uh.modifier1 = modifier1;
	uh.pktsize = 2+item1_len+2+item2_len;
	uh.modifier2 = modifier2;

        strsize1[0] = (uint8_t) (item1_len & 0xff);
	strsize1[1] = (uint8_t) ((item1_len >> 8) & 0xff);

        strsize2[0] = (uint8_t) (item2_len & 0xff);
	strsize2[1] = (uint8_t) ((item2_len >> 8) & 0xff);

	iov[0].iov_base = &uh;
	iov[0].iov_len = 4;

	iov[1].iov_base = strsize1;
	iov[1].iov_len = 2;

	iov[2].iov_base = item1;
	iov[2].iov_len = item1_len;

	iov[3].iov_base = strsize2;
	iov[3].iov_len = 2;

	iov[4].iov_base = item2;
	iov[4].iov_len = item2_len;

	if (writev(fd, iov, 5) < 0) {
		uwsgi_error("writev()");
	}

	close(fd);
	
	return 0;
}

int uwsgi_simple_send_string(char *socket_name, uint8_t modifier1, uint8_t modifier2, char *item1, uint16_t item1_len, int timeout) {

        struct uwsgi_header uh;
        char strsize1[2];

        struct iovec iov[3];

        int fd = uwsgi_connect(socket_name, timeout, 0);

        if (fd < 0) {
                return -1;
        }

        uh.modifier1 = modifier1;
        uh.pktsize = 2+item1_len;
        uh.modifier2 = modifier2;

        strsize1[0] = (uint8_t) (item1_len & 0xff);
        strsize1[1] = (uint8_t) ((item1_len >> 8) & 0xff);

        iov[0].iov_base = &uh;
        iov[0].iov_len = 4;

        iov[1].iov_base = strsize1;
        iov[1].iov_len = 2;

        iov[2].iov_base = item1;
        iov[2].iov_len = item1_len;

        if (writev(fd, iov, 3) < 0) {
                uwsgi_error("writev()");
        }

        close(fd);

        return 0;
}


int uwsgi_file_serve(struct wsgi_request *wsgi_req, char *document_root, uint16_t document_root_len, char *path_info, uint16_t path_info_len, char *orig_document_root, uint16_t orig_document_root_len) {

        struct stat st;
        char real_filename[PATH_MAX];
        char *filename = uwsgi_concat3n(document_root, document_root_len, "/", 1, path_info, path_info_len);
#ifdef UWSGI_DEBUG
        uwsgi_log("checking for %s\n", filename);
#endif
        if (!realpath(filename, real_filename)) {
#ifdef UWSGI_DEBUG
                uwsgi_log("unable to get realpath() of the static file\n");
#endif
                free(filename);
                return -1;
        }

        free(filename);

        if (uwsgi_starts_with(real_filename, strlen(real_filename), document_root, document_root_len)) {
                uwsgi_log("security error: %s is not under %.*s\n", real_filename, document_root_len, document_root);
                return -1;
        }
        if (!stat(real_filename, &st)) {
                if (wsgi_req->if_modified_since_len) {
                        time_t ims = parse_http_date(wsgi_req->if_modified_since, wsgi_req->if_modified_since_len);
                        if (st.st_mtime <= ims) {
                                wsgi_req->status = 304;
                                wsgi_req->headers_size = wsgi_req->socket->proto_write_header(wsgi_req, wsgi_req->protocol, wsgi_req->protocol_len);
                                wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, " 304 Not Modified\r\n", 19);

				struct uwsgi_string_list *ah = uwsgi.additional_headers;
				while(ah) {
					wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, ah->value, ah->len);
					wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, "\r\n", 2);
					ah = ah->next;
				}

				wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, "\r\n", 2);
                                return 0;
                        }
                }
                if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
                        char http_last_modified[49];
#ifdef UWSGI_DEBUG
                        uwsgi_log("file %s found\n", real_filename);
#endif
                        // no need to set content-type/content-length, they will be fixed by the http server/router

                        wsgi_req->headers_size = wsgi_req->socket->proto_write_header(wsgi_req, wsgi_req->protocol, wsgi_req->protocol_len);
                        wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, " 200 OK\r\n", 9);

			struct uwsgi_string_list *ah = uwsgi.additional_headers;
			while(ah) {
				wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, ah->value, ah->len);
				wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, "\r\n", 2);
				ah = ah->next;
			}

			if (uwsgi.file_serve_mode == 1) {
                                wsgi_req->header_cnt = 2;
				wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, "X-Accel-Redirect: ", 18);
				wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, orig_document_root, orig_document_root_len);
				if (orig_document_root[orig_document_root_len-1] != '/') {
					wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, "/", 1);
				}
				wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, path_info, path_info_len);
				wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, "\r\n", 2);
                        	set_http_date(st.st_mtime, http_last_modified);
                        	wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, http_last_modified, 48);
			}
			else if (uwsgi.file_serve_mode == 2) {
                                wsgi_req->header_cnt = 2;
				wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, "X-Sendfile: ", 12);
				wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, orig_document_root, orig_document_root_len);
                                if (orig_document_root[orig_document_root_len-1] != '/') {
                                        wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, "/", 1);
                                }
                                wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, path_info, path_info_len);	
				wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, "\r\n", 2);
                        	set_http_date(st.st_mtime, http_last_modified);
                        	wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, http_last_modified, 48);
			}
			else {
                                wsgi_req->header_cnt = 1;
                        	set_http_date(st.st_mtime, http_last_modified);
                        	wsgi_req->headers_size += wsgi_req->socket->proto_write_header(wsgi_req, http_last_modified, 48);
                                wsgi_req->sendfile_fd = open(real_filename, O_RDONLY);
                                wsgi_req->response_size += uwsgi_sendfile(wsgi_req);
			}


                        wsgi_req->status = 200;
                        return 0;
                }
        }

        return -1;

}

