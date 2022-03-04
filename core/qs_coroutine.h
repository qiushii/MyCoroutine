
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <assert.h>
#include <inttypes.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <netinet/tcp.h>

#include <sys/epoll.h>
#include <sys/poll.h>

#include <errno.h>

#include "nty_queue.h"
#include "nty_tree.h"


#ifndef _QS_COROUTINE_H
#define _QS_COROUTINE_H

#define _GUN_SOURCE

#define QS_CO_MAX_STACKSIZE   		(4*1024) //{HTTP:16*1024, TCP:4*1024}
#define QS_CO_MAX_EVENT 	     		(1024*1024)

#define BIT(x)				     		(1<<(x)) 		//��xλ��1
#define CLEARBIT(x) 		  	     		~(1<<(x))		//��xλ��0

#define CANCEL_FD_WAIT_UINT64  	1 				//64λ

/*Э��״̬*/
typedef enum{
	QS_COROUTINE_STATUS_WAIT_READ,	 			//�ȴ���
	QS_COROUTINE_STATUS_WAIT_WRITE, 			//�ȴ���
	QS_COROUTINE_STATUS_NEW, 					//�½�
	QS_COROUTINE_STATUS_READY,			 		//����
	QS_COROUTINE_STATUS_EXITED, 					//�˳�
	QS_COROUTINE_STATUS_BUSY, 					//��æ
	QS_COROUTINE_STATUS_SLEEPING, 				//����˯��
	QS_COROUTINE_STATUS_EXPIRED,				//���ڵ�
	QS_COROUTINE_STATUS_FDEOF,					//�ļ�ĩβ
	QS_COROUTINE_STATUS_DETACH,					//����
	QS_COROUTINE_STATUS_CANCELLED, 				//���ϣ�ȡ��
	QS_COROUTINE_STATUS_PENDING_RUNCOMPUTE,	//δ������Ȼ�л�
	QS_COROUTINE_STATUS_RUNCOMPUTE,			//��Ȼ�л�
	QS_COROUTINE_STATUS_WAIT_IO_READ, 			//�ȴ�IO ��ȡ
	QS_COROUTINE_STATUS_WAIT_IO_WRITE,		//�ȴ�IO д��
	QS_COROUTINE_STATUS_WAIT_MULTI 			//
}qs_coroutine_status;

typedef enum{
	QS_COROUTINE_COMPUTE_READ,
	QS_COROUTINR_COMPUTE_WRITE
}qs_coroutine_compute_status;

typedef enum{
	QS_COROUTINE_EV_READ,
	QS_COROUTINE_EV_WRITE
}qs_coroutine_event;

LIST_HEAD(_qs_coroutine_link, _qs_coroutine);
TAILQ_HEAD(_qs_coroutine_queue, _qs_coroutine);

RB_HEAD(_qs_coroutine_rbtree_sleep, _qs_coroutine);
RB_HEAD(_qs_coroutine_rbtree_wait, _qs_coroutine);

typedef struct _qs_coroutine_link qs_coroutine_link;
typedef struct _qs_coroutine_queue qs_coroutine_queue;

typedef struct _qs_coroutine_rbtree_sleep qs_coroutine_rbtree_sleep;
typedef struct _qs_coroutine_rbtree_wait qs_coroutine_rbtree_wait;



/*�Ĵ���*/
typedef struct _qs_cpu_ctx{
	void *esp;      //ջ��ָ��
	void *ebp;      //ջ��ָ��
	void *eip;
	void *esi;
	void *ebx;
	void *r1;
	void *r2;
	void *r3;
	void *r4;
	void *r5;
}qs_cpu_ctx;

typedef void (*proc_coroutine)(void*);

typedef struct _qs_coroutine{
	qs_cpu_ctx ctx;         //����Э���ó�ʱ�ļĴ����Իָ��ֳ�
	proc_coroutine func;    //Э�����еĺ���
	void *arg;              //����
	void *data;             
	size_t stack_size;      //Э��ջ��С
	size_t last_stack_size; //��һ��Э��ջ�Ĵ�С

	qs_coroutine_status status; //�߳�״̬

	qs_schedule *sched;         //Э�������ĵ�����

	uint64_t birth;             //Э�̵ĳ���ʱ��
	uint64_t id;                //Э��������id

#if CANCEL_FD_WAIT_UINT64
	int fd;
	unsigned short events;
#else
	int64_t fd_wait;
#endif

	char funcname[64];
	struct _qs_coroutine *co_join;

	void **co_exir_ptr;
	void *stack;    //ջ
	void *ebp;      //ջ��ָ��
	uint32_t ops;  
	uint64_t sleep_usecs;

	RB_ENTRY(_qs_coroutine) sleep_node;         //˯�ߺ����
	RB_ENTRY(_qs_coroutine) wait_node;   //�ȴ�����

	LIST_ENTRY(_qs_coroutine) busy_next;        //æ����

	TAILQ_ENTRY(_qs_coroutine) ready_next;      //��������
	TAILQ_ENTRY(_qs_coroutine) defer_next;      //
	TAILQ_ENTRY(_qs_coroutine) cond_next;       //

	struct{
		void *buf;
		size_t nbytes;
		int fd;
		int ret;
		int err;
	}io;


	struct _qs_coroutine_compute_sched *compute_sched;
	int ready_fds;
	struct pollfd *pfds;
	nfds_t nfds;
}qs_coroutine;

typedef struct _qs_schedule{
 
	uint64_t birth; 				//����ʱ��
	qs_cpu_ctx ctx;                         //�ָ�ʱ���������������ļĴ����͹����ֳ�
	void *stack;                            //������ջ
	size_t stack_size;                      //ջ��С
	int spawned_coroutines;                 //
	uint64_t default_timeout;               //Ĭ�ϵȴ�ʱ��
	struct _qs_coroutine *curr_thread;      //��ǰЭ��
	int page_size;                          //ҳ��С

	int poller_fd;                          //epfd
	int eventfd;
	struct epoll_event eventlist[QS_CO_MAX_EVENT];   //�������epoll_wait�õ���ʱ�䣬epoll_wait�ڶ�������
	int nevents;
	int num_new_events;

	pthread_mutex_t defer_mutex;	//��

	qs_coroutine_queue ready;               //��������
	qs_coroutine_queue defer;               //

	qs_coroutine_link busy;                 //æ����

	qs_coroutine_rbtree_sleep sleeping;     //˯�ߺ����
	qs_coroutine_rbtree_wait waiting;       //�ȴ������

#if COROUTINE_MP

#endif
}qs_schedule;

typedef struct _qs_coroutine_compute_sched {
	qs_cpu_ctx ctx;
	qs_coroutine_queue coroutines;

	qs_coroutine *curr_coroutine;

	pthread_mutex_t run_mutex;
	pthread_cond_t run_cond;

	pthread_mutex_t co_mutex;
	LIST_ENTRY(_qs_coroutine_compute_sched) compute_next;
	
	qs_coroutine_compute_status compute_status;
} nty_coroutine_compute_sched;


#define BIT(x) (1<<(x))

extern pthread_key_t global_sched_key;
static inline qs_schedule *qs_coroutine_get_sched(void) {
	return pthread_getspecific(global_sched_key);
}

static inline uint64_t qs_coroutine_usec_now(void){
	struct timeval t1 = {0,0};
	gettimeofday(&t1, NULL);

	return t1.tv_sec*10000000 + t1.tv_usec;
}

static inline uint64_t qs_coroutine_diff_usecs(uint64_t t1, uint64_t t2){
	return t2 - t1;
}



int qs_epoller_create(void);

void qs_schedule_cancel_event(qs_coroutine *co);
void sq_schedule_sched_event(qs_coroutine *co, int fd, qs_coroutine_event e, uint64_t timeout);

void qs_schedule_desched_sleepdown(qs_coroutine *co);
void qs_schedule_sched_sleepdown(qs_coroutine *co, uint64_t msecs);

qs_coroutine* qs_schedule_desched_wait(int fd);
void qs_schedule_sched_wait(qs_coroutine *co, int fd, unsigned short events, uint64_t timeout);

void qs_schedule_run(void);

int qs_epoller_ev_register_trigger(void);
int qs_epoller_wait(struct timespec t);
int qs_coroutine_resume(qs_coroutine *co);
void qs_coroutine_free(qs_coroutine *co);
int qs_coroutine_create(qs_coroutine **new_co, proc_coroutine func, void *arg);
void qs_coroutine_yield(qs_coroutine *co);

void sq_coroutine_sleep(uint64_t msecs);


int qs_socket(int domain, int type, int protocol);
int qs_accept(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t qs_recv(int fd, void *buf, size_t len, int flags);
ssize_t qs_send(int fd, const void *buf, size_t len, int flags);
int qs_close(int fd);
int qs_poll(struct pollfd *fds, nfds_t nfds, int timeout);
int qs_connect(int fd, struct sockaddr *name, socklen_t namelen);

ssize_t qs_sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t qs_recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);




#define COROUTINE_HOOK 

#ifdef  COROUTINE_HOOK


typedef int (*socket_t)(int domain, int type, int protocol);
extern socket_t socket_f;

typedef int(*connect_t)(int, const struct sockaddr *, socklen_t);
extern connect_t connect_f;

typedef ssize_t(*read_t)(int, void *, size_t);
extern read_t read_f;


typedef ssize_t(*recv_t)(int sockfd, void *buf, size_t len, int flags);
extern recv_t recv_f;

typedef ssize_t(*recvfrom_t)(int sockfd, void *buf, size_t len, int flags,
        struct sockaddr *src_addr, socklen_t *addrlen);
extern recvfrom_t recvfrom_f;

typedef ssize_t(*write_t)(int, const void *, size_t);
extern write_t write_f;

typedef ssize_t(*send_t)(int sockfd, const void *buf, size_t len, int flags);
extern send_t send_f;

typedef ssize_t(*sendto_t)(int sockfd, const void *buf, size_t len, int flags,
        const struct sockaddr *dest_addr, socklen_t addrlen);
extern sendto_t sendto_f;

typedef int(*accept_t)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
extern accept_t accept_f;

// new-syscall
typedef int(*close_t)(int);
extern close_t close_f;


int init_hook(void);



#endif
