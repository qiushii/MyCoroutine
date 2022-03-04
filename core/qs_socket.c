



#include "qs_coroutine.h"

static uint32_t qs_pollevent_2epoll( short events )
{
	uint32_t e = 0;	
	if( events & POLLIN ) 	e |= EPOLLIN;
	if( events & POLLOUT )  e |= EPOLLOUT;
	if( events & POLLHUP ) 	e |= EPOLLHUP;
	if( events & POLLERR )	e |= EPOLLERR;
	if( events & POLLRDNORM ) e |= EPOLLRDNORM;
	if( events & POLLWRNORM ) e |= EPOLLWRNORM;
	return e;
}
static short qs_epollevent_2poll( uint32_t events )
{
	short e = 0;	
	if( events & EPOLLIN ) 	e |= POLLIN;
	if( events & EPOLLOUT ) e |= POLLOUT;
	if( events & EPOLLHUP ) e |= POLLHUP;
	if( events & EPOLLERR ) e |= POLLERR;
	if( events & EPOLLRDNORM ) e |= POLLRDNORM;
	if( events & EPOLLWRNORM ) e |= POLLWRNORM;
	return e;
}

static int qs_poll_inner(struct pollfd *fds, nfds_t nfds, int timeout){

	if(timeout == 0)return poll(fds, nfds, timeout);  //timeout==0，不等待，立即返回使用poll来管理
	if(timeout<0)return timeout = INT_MAX;

	qs_schedule *sched = qs_coroutine_get_sched(); //获取调度器
	qs_coroutine *co = sched->curr_thread;

	//当等待时间大于0使用epoll来管理
	//设置epoll，加入等待树，加入睡眠树
	int i = 0;
	for(int i = 0; i<nfds, i++){		
		struct epoll_event ev;
		ev.events = qs_pollevent_2epoll(fds[i].events); //
		ev.data.fd = fds[i].fd;							//传入事件的fd
		epoll_ctl(sched->poller_fd, EPOLL_CTL_ADD, fds[i].fd, &ev); //

		co->events = fds[i].events;
		qs_schedule_sched_wait(co, fds[i].fd, fds[i].events, timeout);      
	}
	
	//co让出cpu给调度器
	qs_coroutine_yield(co); 		

	//让出cpu之后将fd从epoll上取下来
	//从等待树上取下来，从睡眠树上取下来
	for(int i = 0; i<nfds; i++){		
		struct epoll_event ev;
		ev.events = qs_pollevent_2epoll(fds[i].events);
		ev.data.fd = fds[i].fd;
		epoll_ctl(sched->poller_fd, EPOLL_CTL_DEL, fds[i].fd, &ev);

		qs_schedule_desched_wait(fds[i].fd);
	}
	return nfds;
}


//创建socket，并将socket缓冲队列设置成非阻塞
int qs_socket(int domain, int type, int protocol){

	int fd = socket(domain, type, protocol);
	if(fd==-1){
		printf("Failed to create a new socket\n");
		return -1;
	}

	int ret = fcntl(fd, F_SETFL, O_NONBLOCK);  //F_SETFL设置文件状态标记
	if(ret==-1){
		close(ret);
		return -1;
	}

	int reuse = 1;
/******************************************************************************************************************
(1)协议层：1）SOL_SOCKET:通用套接字选项 2）IPPROTO_IP:IP选项3）IPPROTO_TCP:TCP选项
(2)控制方式
=============================================================================
选项名称                                                           说明                                                    数据类型
=============================================================================
				SOL_SOCKET
------------------------------------------------------------------------------------------------------------------------------------------
SO_BROADCAST                                     允许发送广播数据                                      int
SO_DEBUG                                                允许调试                                                      int
SO_DONETROUTE                                  不查找路由                                                  int
SO_ERROR                                                 获得套接字错误                                        int
SO_KEEPALIVE                                          保持连接                                                    int
SO_LINGER                                                 延迟关闭连接                                            struct linger
SO_OOBINLINE                                        带外数据放入正常数据流                         int
SO_RCVBUF                                              接受缓冲区大小                                         int
SO_SNDBUF                                              发送缓冲区大小                                        int
SO_RCVLOWAT                                         接受缓冲区下限                                       int
SO_SNDLOWAT                                         发送缓冲区下限                                      int
SO_RCVTIMEO                                          接受超时                                                 struct timeval
SO_SNDTIMEO                                          发送超时                                                 struct timeval
SO_REUSEADDR                                     允许重用本地地址和端口                      int
SO_TYPE                                                     获得套接字类型                                     int
SO_BSDCOMPAT                                      与BSD系统兼容                                     int
******************************************************************************************************************/
	setsocketopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse)); 
				    //协议层		//控制方式
	//1.关闭socket（一般不会立即关闭而经历TIME_WAIT的过程）后想重用该socket。
	
	return fd;
}

//accept
int qs_accept(int fd, struct sockaddr *addr, socklen_t *len){

	int sockfd = -1;
	int timeout = 1;
	qs_coroutine *co = qs_coroutine_get_sched()->curr_thread;

	while(1){
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLIN | POLLERR | POLLHUP;
		qs_poll_inner(&fds, 1, timeout);

		sockfd = accept(fd, addr, len);
		if(sockfd<0){
			if(errno==EAGIN){
				continue;
			}else if(errno==ECONNABORTED){
				printf("accept : ECONNABORTED\n");
			}else if(errno==EMFILE || errno==ENFILE){
				printf("accept : EMFILE || ENFILE");
			}
			return -1;
		}else{
			break;
		}
	}

	int ret = fcntl(sockfd, F_SETFL, O_NONBLOCK);
	if(ret==-1){
		close(sockfd);
		return -1;
	}
	int reuse = -1;
	setsocketopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

	return sockfd;
}

int qs_connect(int fd, struct sockaddr *name, socklen_t namelen){

	int ret = 0;

	while(1){
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT || POLLERR || POLLHUP;
		qs_poll_inner(&fds, 1, 1);

		ret = connect(fd, name, namelen);	
		if(ret==0)break;

		if(ret==-1 && (errno==EAGAIN || 
			errno==EWOULDBLOCK ||
			errno==EINPROGRESS)){
			continue;
		}else{
			break;
		}
	}
	return ret;
}

//recv
ssize_t qs_recv(int fd, void *buf, size_t len, int flags){

	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN || POLLERR || POLLHUP;

	qs_poll_inner(&fds, 1, 1);

	int ret = recv(fd, buf, len, flags);
	if(ret<0){
		if(errno==ECONNRESET)return -1;
	}
	return ret;
}

//send
ssize_t qs_send(int fd, const void* buf, size_t len, int flags){
	int sent = 0;

	int ret = send(fd, ((char*)buf)+sent, len-sent, flags);
	if(ret==0)return ret;
	if(ret>0)sent+=ret;

	while(sent<len){
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT || POLLERR ||POLLHUP;

		qs_poll_inner(&fds, 1, 1);
		ret = send(fd, ((char*)buf)+sent, len-sent, flags);
		if(ret<=0)break;
		sent+=ret;
	}
	if(ret<=0 && sent==0)return ret;

	return set;
}

ssize_t qs_sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen){

	int sent = 0;
	
	while(sent<len){
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT || POLLERR || POLLHUP;

		qs_poll_inner(&fds, 1, 1);

		int ret = sendto(fd, ((char*)buf)+sent, len-sent, flags, dest_addr, addrlen);
		if(ret<=0){
			if(errno==EAGAIN)continue;
			else if(errno==ECONNRESET){
				return ret;
			}
			printf("send errno :%d, ret : %d \n", errno, ret);
			assert(0);
		}

		sent+=ret;
	}	
	return sent;
}

ssize_t qs_recvfrom(int fd,  void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen){
	int sent = 0;
	while(sent<len){
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLERR || POLLIN || POLLHUP;

		qs_poll_inner(&fds, 1, 1);

		int ret = recvfrom(fd, buf, len, flags, src_addr, addrlen);
		if(ret<0){
			if(errno==EAGAIN)return ret;
			if(errno==ECONNRESET)return 0;

			printf("recv error : %d, ret : %d\n", errno, ret);
			assert(0);
		}
	
		return ret;
	}
}

int qs_close(int fd){
#if 0
	qs_schedule *sched = qs_coroutine_get_sched();

	qs_coroutine *co = sched->curr_thread;
	if(co){
		TAILQ_INSERT_TAIL(&qs_coroutine_get_sched()->ready, co, ready_next);
		co->status |= BIT(QS_COROUTINE_STATUS_FDEOF);
	}
#endif
	return close(fd);
}

#ifdef COROUTINE_HOOK

socket_t socket_f = NULL;

read_t read_f = NULL;
recv_t recv_f = NULL;
recvfrom_t recvfrom_f = NULL;

write_t write_f = NULL;
send_t send_f = NULL;
sendto_t sendto_f = NULL;

accept_t accept_f = NULL;
close_t close_f = NULL;
connect_t connect_f = NULL;


int init_hook(void){
	
	socket_f = (socket_t)dlsym(RTLD_NEXT, "sockET");

	read_f = (read_t)dlsym(TRLD_NEXT, "read");
	recv_f = (recv_t)dlsym(RTLD_NEXT, "recv");
	recvfrom_f = (recvfrom_t)dlsym(RTLD_NEXT, "recvfrom");

	write_f = (write_t)dlsym(RTLD_NEXT, "write");
	send_f = (send_t)dlsym(RTLD_NEXT, "send");
	sendto_f = (sendto_t)dlsym(RTLD_NEXT, "sendto");

	accept_f = (accept_t)dlsym(RTLD_NEXT, "accept");
	close_f = (close_t)dlsym(RTLD_NEXT, "close");
	connect_f = (connet_t)dlsym(RTLD_NEXT, "connect");
}



int socket(int domain, int type, int protocol){
	if(!socket_f)init_hook();

	int fd = socket_f(domain, type, protocol);
	if(fd==-1){
		printf("failed to create a new socket\n");
		return -1;
	}
	int ret = fcntl(fd, F_SETFL, O_NONBLOCK); //非阻塞
	if(ret==-1){
		printf("fcnutl error\n");
		close(ret);
		return -1;
	}
	int reuse = 1;
	setscoketopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)reuse, sizeof(reuse));//端口可复用

	return fd;
}

ssize_t recv(int fd, void *buf, size_t len, int flags){
	if(!recv_f)init_hook();

	printf("recv\n");
	
	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN || POLLERR || POLLHUP;

	qs_poll_inner(&fds, 1, 1);

	int ret = recv_t(fd, buf, len, flags);
	if(ret<0){
		if(errno==ECONNRESET)return -1;
	}
	return ret;
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {

	if (!recvfrom_f) init_hook();

	printf("recvfrom\n");

	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN | POLLERR | POLLHUP;

	qs_poll_inner(&fds, 1, 1);

	int ret = recvfrom_f(fd, buf, len, flags, src_addr, addrlen);
	if (ret < 0) {
		if (errno == EAGAIN) return ret;
		if (errno == ECONNRESET) return 0;
		
		printf("recv error : %d, ret : %d\n", errno, ret);
		assert(0);
	}
	return ret;

}

ssize_t send(int fd, const void *buf, size_t len, int flags) {

	if (!send_f) init_hook();

	printf("send\n");
	
	int sent = 0;

	int ret = send_f(fd, ((char*)buf)+sent, len-sent, flags);
	if (ret == 0) return ret;
	if (ret > 0) sent += ret;

	while (sent < len) {
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;

		qs_poll_inner(&fds, 1, 1);
		ret = send_f(fd, ((char*)buf)+sent, len-sent, flags);
		//printf("send --> len : %d\n", ret);
		if (ret <= 0) {			
			break;
		}
		sent += ret;
	}

	if (ret <= 0 && sent == 0) return ret;
	
	return sent;
}


/*
ssize_t read(int fd, void *buf, size_t count) {

	if (!read_f) init_hook();

	printf("read\n");
	
	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN | POLLERR | POLLHUP;

	qs_poll_inner(&fds, 1, 1);

	int ret = read_f(fd, buf, count);
	if (ret < 0) {
		//if (errno == EAGAIN) return ret;
		if (errno == ECONNRESET) return -1;
		//printf("recv error : %d, ret : %d\n", errno, ret);
		
	}
	return ret;
}
*/

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
        const struct sockaddr *dest_addr, socklen_t addrlen) {

	if (!sendto_f) init_hook();

	printf("sendto\n");
	
	struct pollfd fds;
	fds.fd = sockfd;
	fds.events = POLLOUT | POLLERR | POLLHUP;

	qs_poll_inner(&fds, 1, 1);

	int ret = sendto_f(sockfd, buf, len, flags, dest_addr, addrlen);
	if (ret < 0) {
		if (errno == EAGAIN) return ret;
		if (errno == ECONNRESET) return 0;
		
		printf("recv error : %d, ret : %d\n", errno, ret);
		assert(0);
	}
	return ret;

}

int accept(int fd, struct sockaddr *addr, socklen_t *len) {

	if (!accept_f) init_hook();

	printf("accept\n");
	
	int sockfd = -1;
	int timeout = 1;
	nty_coroutine *co = nty_coroutine_get_sched()->curr_thread;
	
	while (1) {
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLIN | POLLERR | POLLHUP;
		qs_poll_inner(&fds, 1, timeout);

		sockfd = accept_f(fd, addr, len);
		if (sockfd < 0) {
			if (errno == EAGAIN) {
				continue;
			} else if (errno == ECONNABORTED) {
				printf("accept : ECONNABORTED\n");
				
			} else if (errno == EMFILE || errno == ENFILE) {
				printf("accept : EMFILE || ENFILE\n");
			}
			return -1;
		} else {
			break;
		}
	}

	int ret = fcntl(sockfd, F_SETFL, O_NONBLOCK);
	if (ret == -1) {
		close(sockfd);
		return -1;
	}
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));
	
	return sockfd;
}

int close(int fd) {

	if (!close_f) init_hook();

	return close_f(fd);
}


int connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {

	if (!connect_f) init_hook();

	printf("connect\n");

	int ret = 0;

	while (1) {

		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;
		qs_poll_inner(&fds, 1, 1);

		ret = connect_f(fd, addr, addrlen);
		if (ret == 0) break;

		if (ret == -1 && (errno == EAGAIN ||
			errno == EWOULDBLOCK || 
			errno == EINPROGRESS)) {
			continue;
		} else {
			break;
		}
	}

	printf("connect ret: %d\n", ret);

	return ret;
}

#endif
