#include "kn_type.h"
#include "kn_list.h"
#include "kendynet_private.h"
#include <assert.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "kn_event.h"

typedef struct{
	handle comm_head;
	int    domain;
	int    type;
	int    protocal;
	engine_t e;
	kn_list pending_send;//尚未处理的发请求
    	kn_list pending_recv;//尚未处理的读请求
    	struct kn_sockaddr    addr_local;
    	struct kn_sockaddr    addr_remote;    
	void   (*cb_disconnect)(handle_t,int);
	void   (*cb_accept)(handle_t,void *ud);
	void   (*cb_ontranfnish)(handle_t,st_io*,int,int);
	void   (*cb_connect)(handle_t,int,void *ud,kn_sockaddr*);
	void   (*destry_stio)(st_io*);
	SSL_CTX *ctx;
	SSL *ssl;
}kn_socket;

enum{
	SOCKET_NONE = 0,
	SOCKET_ESTABLISH  = 1,
	SOCKET_CONNECTING = 2,
	SOCKET_LISTENING  = 3,
	SOCKET_CLOSE      = 4,
};

void ShowCerts(SSL * ssl)
{
    X509 *cert;
    char *line;

    cert = SSL_get_peer_certificate(ssl);
    if (cert != NULL) {
        printf("数字证书信息:\n");
        line = X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
        printf("证书: %s\n", line);
        free(line);
        line = X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
        printf("颁发者: %s\n", line);
        free(line);
        X509_free(cert);
    } else
        printf("无证书信息！\n");
}

static void on_events(handle_t h,int events);

static void on_destroy(void *_){
	kn_socket *s = (kn_socket*)_;
	st_io *io_req;
	if(s->destry_stio){
        		while((io_req = (st_io*)kn_list_pop(&s->pending_send))!=NULL)
            			s->destry_stio(io_req);
        		while((io_req = (st_io*)kn_list_pop(&s->pending_recv))!=NULL)
            			s->destry_stio(io_req);
	}
	if(s->ctx){
		SSL_CTX_free(s->ctx);
	}
	if(s->ssl){
		if(s->comm_head.status == SOCKET_ESTABLISH)
        			SSL_shutdown(s->ssl);
        		SSL_free(s->ssl);
	}
	if(s->e){
		kn_event_del(s->e,(handle_t)s);
	}
	close(s->comm_head.fd);
	free(s);
}

static handle_t new_sock(int fd,int domain,int type,int protocal){
	kn_socket *s = calloc(1,sizeof(*s));
	if(!s){
		return NULL;
	}	
	s->comm_head.fd = fd;
	s->comm_head.type = KN_SOCKET;
	s->domain = domain;
	s->type = type;
	s->protocal = protocal;
	s->comm_head.on_events = on_events;
	s->comm_head.on_destroy = on_destroy;
	return (handle_t)s; 
}

static void process_read(kn_socket *s){
	//printf("process_read\n");
	st_io* io_req = 0;
	int bytes_transfer = 0;
	while((io_req = (st_io*)kn_list_pop(&s->pending_recv))!=NULL){
		errno = 0;
		if(s->protocal == IPPROTO_TCP){
			if(s->ssl){
				bytes_transfer = TEMP_FAILURE_RETRY(SSL_read(s->ssl,io_req->iovec[0].iov_base,io_req->iovec[0].iov_len));
				int ssl_error = SSL_get_error(s->ssl,bytes_transfer);
				if(bytes_transfer < 0 && (ssl_error == SSL_ERROR_WANT_WRITE ||
							ssl_error == SSL_ERROR_WANT_READ ||
							ssl_error == SSL_ERROR_WANT_X509_LOOKUP)){
					errno = EAGAIN;
				}			
			}
			else
				bytes_transfer = TEMP_FAILURE_RETRY(readv(s->comm_head.fd,io_req->iovec,io_req->iovec_count));
		}else if(s->protocal == IPPROTO_UDP){		
		}else
			assert(0);
		
		if(bytes_transfer < 0 && errno == EAGAIN){
				//将请求重新放回到队列
				kn_list_pushback(&s->pending_recv,(kn_list_node*)io_req);
				break;
		}else{
			s->cb_ontranfnish((handle_t)s,io_req,bytes_transfer,errno);
			if(s->comm_head.status == SOCKET_CLOSE)
				return;			
		}
	}	
	if(!kn_list_size(&s->pending_recv)){
		//没有接收请求了,取消EPOLLIN
		kn_disable_read(s->e,(handle_t)s);
	}	
}

static void process_write(kn_socket *s){
	//printf("process_write\n");
	st_io* io_req = 0;
	int bytes_transfer = 0;
	while((io_req = (st_io*)kn_list_pop(&s->pending_send))!=NULL){
		errno = 0;
		if(s->protocal == IPPROTO_TCP){
			if(s->ssl){
				bytes_transfer = TEMP_FAILURE_RETRY(SSL_write(s->ssl,io_req->iovec[0].iov_base,io_req->iovec[0].iov_len));
				int ssl_error = SSL_get_error(s->ssl,bytes_transfer);
				if(bytes_transfer < 0 && (ssl_error == SSL_ERROR_WANT_WRITE ||
							ssl_error == SSL_ERROR_WANT_READ ||
							ssl_error == SSL_ERROR_WANT_X509_LOOKUP)){
					errno = EAGAIN;
				}			
			}
			else	
				bytes_transfer = TEMP_FAILURE_RETRY(writev(s->comm_head.fd,io_req->iovec,io_req->iovec_count));
		}else if(s->protocal == IPPROTO_UDP){		
		}else
			assert(0);
		
		if(bytes_transfer < 0 && errno == EAGAIN){
				//将请求重新放回到队列
				kn_list_pushback(&s->pending_send,(kn_list_node*)io_req);
				break;
		}else{
			s->cb_ontranfnish((handle_t)s,io_req,bytes_transfer,errno);
			if(s->comm_head.status == SOCKET_CLOSE)
				return;
		}
	}
	if(!kn_list_size(&s->pending_send)){
		//没有接收请求了,取消EPOLLOUT
		kn_disable_write(s->e,(handle_t)s);
	}		
}

static int _accept(kn_socket *a,kn_sockaddr *remote){
	int fd;
	socklen_t len;
	int domain = a->domain;
again:
	if(domain == AF_INET){
		len = sizeof(remote->in);
		fd = accept(a->comm_head.fd,(struct sockaddr*)&remote->in,&len);
	}else if(domain == AF_INET6){
		len = sizeof(remote->in6);
		fd = accept(a->comm_head.fd,(struct sockaddr*)&remote->in6,&len);
	}else if(domain == AF_LOCAL){
		len = sizeof(remote->un);
		fd = accept(a->comm_head.fd,(struct sockaddr*)&remote->un,&len);
	}else{
		return -1;
	}

	if(fd < 0){
#ifdef EPROTO
		if(errno == EPROTO || errno == ECONNABORTED)
#else
		if(errno == ECONNABORTED)
#endif
			goto again;
		else
			return -errno;
	}
	int flags;
	int dummy = 0;
	if ((flags = fcntl(fd, F_GETFL, dummy)) < 0){
		printf("fcntl get error\n");
    		close(fd);
    		return -1;
	}
	if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) <0){
    		printf("fcntl set  FD_CLOEXEC error\n");
    		close(fd);
    		return -1;	
	}
	
	return fd;
}

static void process_accept(kn_socket *s){
    int fd;
    kn_sockaddr remote;
    for(;;)
    {
    	fd = _accept(s,&remote);
    	if(fd < 0)
    		break;
    	else{
		   handle_t h = new_sock(fd,s->domain,s->type,s->protocal);	
		   ((kn_socket*)h)->addr_local = s->addr_local;
		   ((kn_socket*)h)->addr_remote = remote;
		   if(s->ctx){
		   	((kn_socket*)h)->ssl = SSL_new(s->ctx);
		   	SSL_set_fd(((kn_socket*)h)->ssl, fd);
        			if (SSL_accept(((kn_socket*)h)->ssl) == -1) {
            				printf("SSL_accept error\n");
            				kn_close_sock(h);
            				continue;
        			}
		   }
		   kn_set_noblock(fd,0);
		   ((kn_socket*)h)->comm_head.status = SOCKET_ESTABLISH;
		   s->cb_accept(h,s->comm_head.ud);
    	}      
    }
}

static void process_connect(kn_socket *s,int events){
	int err = 0;
	socklen_t len = sizeof(err);    
	kn_event_del(s->e,(handle_t)s);
	s->e = NULL;
	if(getsockopt(s->comm_head.fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1) {
	    s->cb_connect((handle_t)s,err,s->comm_head.ud,&s->addr_remote);
	    return;
	}
	if(err){
	    errno = err;
	    s->cb_connect((handle_t)s,errno,s->comm_head.ud,&s->addr_remote);
	    return;
	}
	//connect success
	s->comm_head.status = SOCKET_ESTABLISH;
	s->cb_connect((handle_t)s,0,s->comm_head.ud,&s->addr_remote);		
}

static void on_events(handle_t h,int events){	
	kn_socket *s = (kn_socket*)h;
	if(s->comm_head.status == SOCKET_CLOSE)
		return;
	do{
		if(s->comm_head.status == SOCKET_LISTENING){
			process_accept(s);
		}else if(s->comm_head.status == SOCKET_CONNECTING){
			process_connect(s,events);
		}else if(s->comm_head.status == SOCKET_ESTABLISH){
			if(events & EVENT_READ){
				if(kn_list_size(&s->pending_recv) == 0){
					s->cb_ontranfnish((handle_t)s,NULL,-1,0);
					break;
				}else{
					process_read(s);	
					if(s->comm_head.status == SOCKET_CLOSE) 
						break;								
				}
			}		
			if(events & EVENT_WRITE)
				process_write(s);			
		}
	}while(0);
}

handle_t kn_new_sock(int domain,int type,int protocal){	
	if(type == SOCK_DGRAM) return NULL;//暂不支持数据报套接字
	int fd = socket(domain,type|/*SOCK_NONBLOCK|*/SOCK_CLOEXEC,protocal);
	if(fd < 0) return NULL;
	handle_t h = new_sock(fd,domain,type,protocal);
	if(!h) close(fd);
	return h;
}

int kn_sock_associate(handle_t h,
		           engine_t e,
		           void (*cb_ontranfnish)(handle_t,st_io*,int,int),
		           void (*destry_stio)(st_io*)){
	if(((handle_t)h)->type != KN_SOCKET) return -1;						  
	kn_socket *s = (kn_socket*)h;
	if(!cb_ontranfnish) return -1;
	if(s->comm_head.status != SOCKET_ESTABLISH) return -1;
	if(s->e) kn_event_del(s->e,h);
	s->destry_stio = destry_stio;
	s->cb_ontranfnish = cb_ontranfnish;
	s->e = e;
#ifdef _LINUX
	kn_event_add(s->e,h,EPOLLRDHUP);
#elif   _BSD
	kn_event_add(s->e,h,EVFILT_READ);
	kn_event_add(s->e,h,EVFILT_WRITE);
	kn_disable_read(s->e,h);
	kn_disable_write(s->e,h);
#endif
	return 0;
}

int kn_sock_send(handle_t h,st_io *req){
	if(((handle_t)h)->type != KN_SOCKET){ 
		return -1;
	}	
	kn_socket *s = (kn_socket*)h;
	if(!s->e || s->comm_head.status != SOCKET_ESTABLISH){
		return -1;
	 }
	
	if(0 != kn_list_size(&s->pending_send))
		return kn_sock_post_send(h,req);
	errno = 0;			
	int bytes_transfer = 0;

	if(s->protocal == IPPROTO_TCP)
		if(s->ssl){
			assert(req->iovec_count == 1);
			if(req->iovec_count != 1)
				return -1;			
			bytes_transfer = TEMP_FAILURE_RETRY(SSL_write(s->ssl,req->iovec[0].iov_base,req->iovec[0].iov_len));
			int ssl_error = SSL_get_error(s->ssl,bytes_transfer);
			if(bytes_transfer < 0 && (ssl_error == SSL_ERROR_WANT_WRITE ||
						ssl_error == SSL_ERROR_WANT_READ ||
						ssl_error == SSL_ERROR_WANT_X509_LOOKUP)){
				errno = EAGAIN;
			}		
		}
		else	
			bytes_transfer = TEMP_FAILURE_RETRY(writev(s->comm_head.fd,req->iovec,req->iovec_count));		
	else if(s->protocal == IPPROTO_UDP){		
	}else
		assert(0);
		
	if(bytes_transfer < 0 && errno == EAGAIN)
		return kn_sock_post_send(h,req);				
	return bytes_transfer > 0 ? bytes_transfer:-1;		
}

int kn_sock_recv(handle_t h,st_io *req){
	if(((handle_t)h)->type != KN_SOCKET){ 
		return -1;
	}	
	kn_socket *s = (kn_socket*)h;
	if(!s->e || s->comm_head.status != SOCKET_ESTABLISH){
		return -1;
	 }
	errno = 0;	
	if(0 != kn_list_size(&s->pending_send))
		return kn_sock_post_recv(h,req);
		
	int bytes_transfer = 0;

	if(s->protocal == IPPROTO_TCP)
		if(s->ssl){
			assert(req->iovec_count == 1);
			if(req->iovec_count != 1)
				return -1;
			bytes_transfer = TEMP_FAILURE_RETRY(SSL_read(s->ssl,req->iovec[0].iov_base,req->iovec[0].iov_len));
			int ssl_error = SSL_get_error(s->ssl,bytes_transfer);
			if(bytes_transfer < 0 && (ssl_error == SSL_ERROR_WANT_WRITE ||
						ssl_error == SSL_ERROR_WANT_READ ||
						ssl_error == SSL_ERROR_WANT_X509_LOOKUP)){
				errno = EAGAIN;
			}		
		}
		else	
			bytes_transfer = TEMP_FAILURE_RETRY(readv(s->comm_head.fd,req->iovec,req->iovec_count));		
	else if(s->protocal == IPPROTO_UDP){		
	}else
		assert(0);
		
	if(bytes_transfer < 0 && errno == EAGAIN)
		return kn_sock_post_recv(h,req);				
	return bytes_transfer > 0 ? bytes_transfer:-1;		
}

int kn_sock_post_send(handle_t h,st_io *req){
	if(((handle_t)h)->type != KN_SOCKET){ 
		return -1;
	}	
	kn_socket *s = (kn_socket*)h;
	if(!s->e || s->comm_head.status != SOCKET_ESTABLISH){
		return -1;
	 }
	if(s->ssl){
		assert(req->iovec_count == 1);
		if(req->iovec_count != 1)
			return -1;
	}			 
	if(!is_set_write((handle_t)s)){
	 	if(0 != kn_enable_write(s->e,(handle_t)s))
	 		return -1;
	}
	kn_list_pushback(&s->pending_send,(kn_list_node*)req);	 	
	return 0;
}

int kn_sock_post_recv(handle_t h,st_io *req){
	if(((handle_t)h)->type != KN_SOCKET){ 
		return -1;
	}	
	kn_socket *s = (kn_socket*)h;
	if(!s->e || s->comm_head.status != SOCKET_ESTABLISH){
		return -1;
	}
	if(s->ssl){
		assert(req->iovec_count == 1);
		if(req->iovec_count != 1)
			return -1;
	}	
	 if(!is_set_read((handle_t)s)){
	 	if(0 != kn_enable_read(s->e,(handle_t)s))
	 		return -1;
	 }	
	kn_list_pushback(&s->pending_recv,(kn_list_node*)req);		
	return 0;	
}

static int kn_bind(int fd,kn_sockaddr *addr_local){
	assert(addr_local);
	int ret = -1;
	if(addr_local->addrtype == AF_INET)
		ret = bind(fd,(struct sockaddr*)&addr_local->in,sizeof(addr_local->in));
	else if(addr_local->addrtype == AF_INET6)
		ret = bind(fd,(struct sockaddr*)&addr_local->in6,sizeof(addr_local->in6));
	else if(addr_local->addrtype == AF_LOCAL)
		ret = bind(fd,(struct sockaddr*)&addr_local->un,sizeof(addr_local->un));
	return ret;	
}


static int stream_listen(engine_t e,
		             kn_socket *s,
		             int fd,
		             kn_sockaddr *local)
{	
	int32_t yes = 1;
	if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)))
		return -1;
	
	if(kn_bind(fd,local) < 0){
		 return -1;
	}
	
	if(listen(fd,SOMAXCONN) < 0){
		return -1;
	}

	s->addr_local = *local;
#ifdef _LINUX	
	int events = EPOLLIN;
#elif _BSD
	int events = EVFILT_READ;
#else

#error "un support platform!"				

#endif
	if(0 == kn_event_add(e,(handle_t)s,events)){
		((handle_t)s)->events = events;
	}
	else
		return -1;		
	return 0;
}

static int dgram_listen(engine_t e,
		            kn_socket *s,
		            int fd,
		            kn_sockaddr *local)
{
	return -1;
}

int kn_sock_listen(engine_t e,
		   handle_t h,
		   kn_sockaddr *local,
		   void (*cb_accept)(handle_t,void*),
		   void *ud)
{
	if(((handle_t)h)->type != KN_SOCKET) return -1;	
	kn_socket *s = (kn_socket*)h;
	if(s->comm_head.status != SOCKET_NONE) return -1;
	if(s->e) return -1;
	int ret;
	
	kn_set_noblock(s->comm_head.fd,0);
	if(s->protocal == IPPROTO_UDP)
		ret = dgram_listen(e,s,s->comm_head.fd,local);
	else
		ret = stream_listen(e,s,s->comm_head.fd,local);
	
	if(ret == 0){
		s->cb_accept = cb_accept;
		s->comm_head.ud = ud;
		s->e = e;
		s->comm_head.status = SOCKET_LISTENING;		
	}	
	return ret;		
}

static int stream_connect(engine_t e,
			  kn_socket *s,
			  int fd,
			  kn_sockaddr *local,
			  kn_sockaddr *remote)
{
	socklen_t len;	
	if(local){
		if(kn_bind(fd,local) < 0){
			return -1;
		}
	}
	int ret;
	if(s->domain == AF_INET)
		ret = connect(fd,(const struct sockaddr *)&remote->in,sizeof(remote->in));
	else if(s->domain == AF_INET6)
		ret = connect(fd,(const struct sockaddr *)&remote->in6,sizeof(remote->in6));
	else if(s->domain == AF_LOCAL)
		ret = connect(fd,(const struct sockaddr *)&remote->un,sizeof(remote->un));
	else{
		return -1;
	}
	if(ret < 0 && errno != EINPROGRESS){
		return -1;
	}
	if(ret == 0){		
		if(!local){		
			s->addr_local.addrtype = s->domain;
			if(s->addr_local.addrtype == AF_INET){
				len = sizeof(s->addr_local.in);
				getsockname(fd,(struct sockaddr*)&s->addr_local.in,&len);
			}else if(s->addr_local.addrtype == AF_INET6){
				len = sizeof(s->addr_local.in6);
				getsockname(fd,(struct sockaddr*)&s->addr_local.in6,&len);
			}else{
				len = sizeof(s->addr_local.un);
				getsockname(fd,(struct sockaddr*)&s->addr_local.un,&len);
			}
    	}
		return 1;
	}else{
#ifdef _LINUX			
		int events = EPOLLIN | EPOLLOUT;
#elif _BSD
		int events = EVFILT_READ | EVFILT_WRITE;
#else

#error "un support platform!"				

#endif		
		if(0 == kn_event_add(e,(handle_t)s,events)){
			s->e = e;
			((handle_t)s)->events = events;
		}else
			return -1;
	}
	return 0;
}

static int dgram_connect(engine_t e,
			 kn_socket *s,
			 int fd,
			 kn_sockaddr *local,
			 kn_sockaddr *remote)
{
	return -1;
}

void   kn_sock_set_connect_cb(handle_t h,void (*cb_connect)(handle_t,int,void*,kn_sockaddr*),void*ud){
	kn_socket *s = (kn_socket*)h;	
	s->cb_connect = cb_connect;
	s->comm_head.ud = ud;
}

int kn_sock_connect(engine_t e,
		        handle_t h,
		        kn_sockaddr *remote,
		        kn_sockaddr *local)
{
	if(((handle_t)h)->type != KN_SOCKET) return -1;
	kn_socket *s = (kn_socket*)h;
	if(s->comm_head.status != SOCKET_NONE) return -1;
	if(s->e) return -1;	
#ifdef _LINUX
	kn_set_noblock(s->comm_head.fd,0);
#endif
	int ret;
	s->addr_remote = *remote;
	if(s->protocal == IPPROTO_UDP)
		ret = dgram_connect(e,s,s->comm_head.fd,local,remote);
	else
		ret = stream_connect(e,s,s->comm_head.fd,local,remote);
	
	if(ret == 0){
		s->comm_head.status = SOCKET_CONNECTING;
		s->e = e;
	}else if(ret == 1){
		s->comm_head.status = SOCKET_ESTABLISH;
#ifdef _BSD
	kn_set_noblock(s->comm_head.fd,0);
#endif
		//cb_connect(h,0,ud,&s->addr_remote);
		//ret = 0;
	}	
	return ret;
}

void SSL_init(){
    /* SSL 库初始化 */
    SSL_library_init();
    /* 载入所有 SSL 算法 */
    OpenSSL_add_all_algorithms();
    /* 载入所有 SSL 错误消息 */
    SSL_load_error_strings();
}

int      kn_sock_ssllisten(engine_t e,
		             handle_t h,
		             kn_sockaddr *addr,
		             void (*cb_accept)(handle_t,void*),
		             void *ud,
		             const char *certificate,
		             const char *privatekey
		             ){
   if(((handle_t)h)->type != KN_SOCKET) return -1;	
   kn_socket *s = (kn_socket*)h;
   if(s->comm_head.status != SOCKET_NONE) return -1;
   if(s->e) return -1;	
    /* 以 SSL V2 和 V3 标准兼容方式产生一个 SSL_CTX ，即 SSL Content Text */
    SSL_CTX *ctx = SSL_CTX_new(SSLv23_server_method());
    /* 也可以用 SSLv2_server_method() 或 SSLv3_server_method() 单独表示 V2 或 V3标准 */
    if (ctx == NULL) {
        ERR_print_errors_fp(stdout);
        return -1;
    }
    /* 载入用户的数字证书， 此证书用来发送给客户端。 证书里包含有公钥 */
    if (SSL_CTX_use_certificate_file(ctx,certificate, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stdout);
        SSL_CTX_free(ctx);
        return -1;
    }
    /* 载入用户私钥 */
    if (SSL_CTX_use_PrivateKey_file(ctx, privatekey, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stdout);
        SSL_CTX_free(ctx);
        return -1;
    }
    /* 检查用户私钥是否正确 */
    if (!SSL_CTX_check_private_key(ctx)) {
        ERR_print_errors_fp(stdout);
        SSL_CTX_free(ctx);
        return -1;
    }
    kn_set_noblock(s->comm_head.fd,0);    
    s->ctx = ctx;
    int ret;
    if(s->protocal == IPPROTO_UDP)
	ret = dgram_listen(e,s,s->comm_head.fd,addr);
    else
	ret = stream_listen(e,s,s->comm_head.fd,addr);

    if(ret == 0){
	s->cb_accept = cb_accept;
	s->comm_head.ud = ud;
	s->e = e;
	s->comm_head.status = SOCKET_LISTENING;		
    }	
    return ret;	    
}

int      kn_sock_sslconnect(engine_t e,
		                  handle_t h,
		                  kn_sockaddr *remote,
		                  kn_sockaddr *local){
	if(((handle_t)h)->type != KN_SOCKET) return -1;
	kn_socket *s = (kn_socket*)h;
	if(s->comm_head.status != SOCKET_NONE) return -1;
	if(s->e) return -1;
	SSL_CTX *ctx = SSL_CTX_new(SSLv23_client_method());
	if (ctx == NULL) {
	    ERR_print_errors_fp(stdout);
	    return -1;
	}
    	s->ctx = ctx;		
	int ret;
	s->addr_remote = *remote;
	if(s->protocal == IPPROTO_UDP)
		ret = dgram_connect(e,s,s->comm_head.fd,local,remote);
	else
		ret = stream_connect(e,s,s->comm_head.fd,local,remote);
	if(ret == 1){
		s->ssl = SSL_new(ctx);
		SSL_set_fd(s->ssl,s->comm_head.fd);
		if (SSL_connect(s->ssl) == -1){
		        ERR_print_errors_fp(stderr);
		        ret = -1;
		}
		else {
		        kn_set_noblock(s->comm_head.fd,0);
		        s->comm_head.status = SOCKET_ESTABLISH;
		        printf("Connected with %s encryption/n", SSL_get_cipher(s->ssl));
		        ShowCerts(s->ssl);
		}		
	}else{
		ret = -1;
	}	
	return ret;	
}

int kn_close_sock(handle_t h){
	if(((handle_t)h)->type != KN_SOCKET) return -1;
	kn_socket *s = (kn_socket*)h;
	if(s->comm_head.status != SOCKET_CLOSE){
		if(s->e){
			s->comm_head.status = SOCKET_CLOSE;
			shutdown(s->comm_head.fd,SHUT_WR);
			kn_push_destroy(s->e,(handle_t)s);
		}else
			on_destroy(s);				
		return 0;
	}
	return -1;	
}

void kn_sock_setud(handle_t h,void *ud){
	if(((handle_t)h)->type != KN_SOCKET) return;
	((handle_t)h)->ud = ud;	
}

void* kn_sock_getud(handle_t h){
	if(((handle_t)h)->type != KN_SOCKET) return NULL;	
	return ((handle_t)h)->ud;
}

int kn_sock_fd(handle_t h){
	return ((handle_t)h)->fd;
}

kn_sockaddr* kn_sock_addrlocal(handle_t h){
	if(((handle_t)h)->type != KN_SOCKET) return NULL;		
	kn_socket *s = (kn_socket*)h;
	return &s->addr_local;
}

kn_sockaddr* kn_sock_addrpeer(handle_t h){
	if(((handle_t)h)->type != KN_SOCKET) return NULL;		
	kn_socket *s = (kn_socket*)h;
	return &s->addr_remote;	
}

engine_t kn_sock_engine(handle_t h){
	if(((handle_t)h)->type != KN_SOCKET) return NULL;
	kn_socket *s = (kn_socket*)h;
	return s->e;	
}

int      kn_is_ssl_socket(handle_t h){
	kn_socket *s = (kn_socket*)h;
	return (s->ssl || s->ctx) ? 1 : 0;
}

int      kn_get_ssl_error(handle_t h,int ret){
	kn_socket *s = (kn_socket*)h;
	if(s->ssl)
		return SSL_get_error(s->ssl,ret);
	else
		return 0;
}
