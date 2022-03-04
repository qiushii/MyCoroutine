#include "qs_coroutine.h"

#define FD_KEY(f,e)  (((uint64_t)(f)<<(sizeof(int32_t)*8))|e)
#define FD_EVENT(f) ((uint32_t)(f))
#define FD_ONLY(f)   ((f)>>((sizeof(int32_t)*8)))


/*比较两个协程的睡眠时间，co1比co2早则返回-1*/
static inline int qs_coroutine_sleep_cmp(qs_coroutine *co1, qs_coroutine *co2){
	if(co1->sleep_usecs < co2->sleep_usecs){
		return -1;
	}
	if(co1->sleep_usecs == co2->sleep_usecs){
		return 0;
	}
	return 1;
}

/*比较两个协程的fd大小*/
static inline int qs_coroutine_wait_cmp(qs_coroutine *co1, qs_coroutine *co2){
#if CANCEL_FD_WAIT_UINT64
	if(co1->fd < co2->fd)return -1;
	else if(co1->fd == co2->fd)return 0;
	else return 1;
#else
	if(co1->fd_wait < co2->fd_wait){
		return -1;
	}
	if (co1->fd_wait == co2->fd_wait) {
		return 0;
	}
#endif
	return  1;
}

RB_GENERATE(_nty_coroutine_rbtree_sleep, _nty_coroutine, sleep_node, nty_coroutine_sleep_cmp);
RB_GENERATE(_nty_coroutine_rbtree_wait, _nty_coroutine, wait_node, nty_coroutine_wait_cmp);


/*计算出协程调度器最小得超时时间，和协程得睡眠时间有关系*/
static uint64_t qs_schedule_min_timeout(qs_schedule *sched){
	uint64_t t_diff_usecs = qs_coroutine_diff_usecs(sched->birth, qs_coroutine_usec_now); //计算出调度器的运行时间
	uint64_t min = sched->default_timeout; 		//默认超时时间

	qs_coroutine *co = RB_MIN(_qs_coroutine_rbtree_sleep, &sched->sleeping); //取出正在睡眠的协程
	if(!co)return min; 		//如果没有处于睡眠树的协程，则返回默认的超时时间

	min = co->sleep_usecs; //协程的睡眠时间
	if(min > t_diff_usecs){//时间差
		return min - t_diff_usecs;
	}

	return 0;
}

/*先计算epoll_wait等待的时间再进行epoll*/
static int qs_schedule_epoll(qs_schedule *sched){
	sched->num_new_events = 0;

	struct timespec t = {0, 0};
	uint64_t usecs = qs_schedule_min_timeout(sched);//获取调度器
	if(usecs && TAILQ_EMPTY(&sched->ready)){  //如果就绪队列上有协程并且usecs不为0 否则epoll_wait一直等待
		t.ts_sec = usecs / 1000000u; 	//
		if(t.tv_sec != 0){
			t.tv_nsec= (usecs % 1000u) * 1000u;
		} else {
			t.tv_nsec = usecs * 1000u;
		}
	} else {
		return 0;
	}
	int nready = 0;
	while(1){
		nready = qs_epoller_wait(t);
		if(nready == -1){
			if(errno == EINTR)continue;
			else assert(0);
		}
		break;
	}
	sched->nevents = 0;
	sched->num_new_events = nready;

	return 0;
}

/*从睡眠树上取下来*/
void qs_schedule_desched_sleepdown(qs_coroutine *co){
	if(co->status & BIT(QS_COROUTINE_STATUS_SLEEPING)){
		RB_REMOVE(_qs_coroutine_rbtree_sleep, &co->sched->sleeping, co);

		co->status &= CLEARBIT(QS_COROUTINE_STATUS_SLEEPING);
		co->status |= BIT(QS_COROUTINE_STATUS_READY);
		co->status &= CLEARBIT(QS_COROUTINE_STATUS_EXPIRED);
	}
}

/*通过fd将等待红黑树和睡眠红黑树上的协程取下来*/
qs_coroutine *qs_schedule_desched_wait(int fd){
	qs_coroutine find_it = {0,0};
	find_it.fd = fd;

	qs_schedule *sched = qs_coroutine_get_sched();

	qs_coroutine *co = RB_FIND(_qs_coroutine_rbtree_wait, &sched->waiting, &find_it);
	if(co !=NULL){
		RB_REMOVE(_qs_coroutine_rbtree_wait, &co->sched->waiting, co);
	}
	co->status = 0;
	qs_schedule_desched_sleepdown(co);

	return co;
}


/*释放调度器*/
int qs_schedule_free(qs_schedule *sched){
    if(sched->poller_fd > 0){ //关闭打开的epfd
        close(sched->poller_fd);
    }
    if(sched->eventfd > 0){
        close(sched->eventfd); //关闭打开的eventfd
    }
    free(sched);//完全释放调度器资源
    
    assert(pthread_setpecific(gloabal_sched_key, NULL)==0);//断言，设置gloabal_sched_key的值为邋邋NULL
}

/*创建调度器*/
int qs_schedule_create(int stack_size){
   
    int sched_stack_size = stack_size ? stack_size : QS_CO_MAX_STACKSIZE; //设置线程栈大小，最大为
    
    qs_schedule *sched = (qs_schedule*)calloc(1, sizeof(qs_schedule));    //为调度器分配内存
    if(sched == NULL){
        printf("Failed to initialize scheduler\n");
        return -1;
    }

    assert(pthread_setpecific(gloabal_sched_key, sched) == 0);       //断言，将调度器设为全局键给各个协程使用
    
    sched->poller_fd = qs_epoller_create();     //创建epoll红黑树
    if(sched->poller_fd == -1){//创建失败
        printf("failed to initialize\n");
        qs_schedule_free(sched);    //释放调度器
        return -2;
    }

    qs_epoller_ev_register_trigger();       //设置epoll的属性

    sched->stack_size = sched_stack_size;    //设置调度器栈大小
    sched->page_size = getpagesize();       //getpagesize()函数获取一个页的大小

    sched->spawned_coroutines = 0; 		//从创建的协程数量
    sched->default_timeout = 300000u;       //调度器超时时间

    RB_INIT(&sched->sleeping);              //创建一个睡眠红黑树
    RB_INIT(&sched->waiting);               //创建等待红黑树
    
    sched->birth = qs_coroutine_usec_now(); //获取调度器创建时间
    
    TAILQ_INIT(&sched->ready);              //创建就绪队列
    
    LIST_INIT(&sched->busy);                //创建忙列表

    bzero(&sched->ctx, sizeof(qs_cpu_ctx)); //将调度器恢复时获取的寄存器的值初始化为0
}

/*判断调度器下是否存在可调度的协程*/
static inline int qs_schedule_isdone(qs_schedule *sched){
	return  ((RB_EMPTY(&sched->waiting))&&
		      (LIST_EMPTY(&sched->busy))&&
		      (RB_EMPTY(&sched->sleeping))&&
		      (TAILQ_EMPTY(&sched->ready))
	);
}

/*查找睡眠树上时间到期的协程*/
static qs_coroutine *qs_schedule_expired(qs_schedule *sched){
	uint64_t t_diff_usecs = qs_coroutine_diff_usecs(sched->birth, qs_coroutine_usec_now()); //调度器运行时间
	qs_coroutine *co = RB_MIN(_qs_coroutine_rbtree_sleep, &sched->sleeping); //取出睡眠队列中最小的协程	
	if(co==NULL)return NULL;

	if(co->sleep_usecs <= t_diff_usecs){//如果协程睡眠时间到期了，则将协程从树上摘下来并返回协程
		RB_REMOVE(_qs_coroutine_rbtree_sleep, &co->sched->sleeping, co); //删除红黑树上睡眠的节点
		return co;
	}
	return NULL;
}

/*调度器从等待红黑树上通过fd查找出协程*/
qs_coroutine *qs_schedule_search_wait(int fd){
	qs_coroutine find_it = {0};
	find_it.fd = fd;

	qs_schedule *sched = qs_coroutine_get_sched(); 	//获取调度器

	qs_coroutine *co = RB_FIND(_qs_coroutine_retree_wait, &sched->waitting, &find_it);  //通过fd在树上查找
	co->status = 0;

	return co;	
}

/*设置co的睡眠时间，睡到什么时候*/
void qs_schedule_sched_sleepdown(qs_coroutine *co, uint64_t msecs){
	uint64_t usecs = msecs * 1000u;

	//从睡眠树上找到co并移除出来
	qs_coroutine *co_tmp = RB_FIND(_qs_coroutine_retree_sleep, co->sched->sleeping, co_tmp);
	if(co_tmp != NULL){
		RB_REMOVE(_qs_coroutine_rbtree_sleep, &co->sched->sleeping, co_tmp);
	}

	co->sleep_usecs = qs_coroutine_diff_usecs(co->sched->birth, qs_coroutine_usec_now)+usecs; 
	//此时的时间-调度器创建时间+msecs*1000u

	while(msecs){ //插入睡眠树
		co_tmp = RB_INSERT(_qs_coroutine_rbtree_sleep, &co->sched->sleeping, co);
		if(co_tmp){
			printf("1111 sleep_usecs %"PRIu64"\n", co->sleep_usecs);
			co->sleep_usecs++;
			continue;
		}
		co->status |= BIT(QS_COROUTINE_STATUS_SLEEPING);
		break;
	}
	
}


/*设置协程epoll事件类型，将协程放入等待红黑树上，同时并将协程放入睡眠红黑树上*/
void qs_schedule_sched_wait(qs_coroutine *co, int fd, unsigned short events, uint64_t timeout){
	//如果事件已经被设置成的EPOLLIN或者EPOLLOUT
	if(
		co->status & BIT(QS_COROUTINE_STATUS_WAIT_READ) || 		
		co->status & BIT(QS_COROUTINE_STATUS_WAIT_WRITE)){
		
		printf("Unexpected event. lt id %"PRIu64" fd %"PRId32" already in %"PRId32" state\n",
			co->id, co->fd, co->status);
		assert(0);
	}

	if(events & EPOLLIN){//判断事件类型并设置status
		co->status |= QS_COROUTINE_STATUS_WAIT_READ;
	}else if(events & EPOLLOUT){
		co->status |= QS_COROUTINE_STATUS_WAIT_WRITE;
	}else{
		printf("events :%d\n", events);
		assert(0);
	}
	co->fd = fd;
	co->events = events;
	qs_coroutine *co_tmp = RB_INSERT(_qs_coroutine_retree_wait, &co->sched->waiting, co);  
								//将co插入到调度器的等待队列中

	assert(co_tmp==NULL);

	if(timeout==1)return;
 
	qs_schedule_sched_sleepdown(co, timeout);  //将co插入睡眠树上，并设置睡眠时间
}

/*将协程从等待红黑树上删除*/
void qs_schedule_cancel_wait(qs_coroutine *co){
	RB_REMOVE(_qs_coroutine_rbtree_wait, &co->sched->waiting, co);
}

/*调度器跑起来*/
void qs_schedule_run(void){
	
	qs_schedule *sched = qs_coroutine_get_sched(); //获取当前调度器
	if(sched == NULL)return;

	while(!qs_schedule_isdone(sched)){

		/*检查睡眠树上有没有在睡眠的协程，如果有的话就将其取出来，并resume运行*/
		/*下面也是*/
		qs_coroutine *expired = NULL;
		while((expired = qs_schedule_expired(sched)) != NULL){
				qs_coroutine_resume(expired);
		}
		
		/*就绪队列*/
		qs_coroutine *last_co_ready = TAILQ_LAST(&sched->ready, _qs_coroutine_queue); //获取最后一个就绪的协程
		while(!TAILQ_EMPTY(&sched->ready)){   					//如果就绪队列不是空的
			qs_coroutine *co = TAILQ_FIRST(&sched->ready); 		 //获取第一个就绪协程
			TAILQ_REMOVE(&co->sched->ready, co, ready_next);		  //从就绪队列移除

			if(&co->status & BIT(QS_COROUTINE_STATUS_FDEOF)){  	//如果协程是运行完的
				qs_coroutine_free(co);
				break;
			}

			qs_coroutine_resume(co); 					//协程恢复运行
			if(co== last_co_ready)break;
		}

		/*等待红黑树*/
		qs_schedule_epoll(sched);
		while(sched->num_new_events){  //如果有事件
			int idx = --sched->num_new_events; 	//取事件下标
			struct epoll_event *ev = sched->eventlist + idx; //通过下标从数组拿出事件
			
			int fd = ev->data.fd; 						//事件fd
			int is_eof = ev->events & EPOLLHUP;			//
			if(is_eof)errno = ECONNRESET;

			qs_coroutine *co = qs_schedule_search_wait(fd);    //通过fd查找出协程
			if(co != NULL){ //找到了
				if(is_eof){ //但是协程事件是EPOLLHUP的话
					co->status |= BIT(QS_COROUTINE_STATUS_FDEOF);
				}
				qs_coroutine_resume(co); 		//让出cpu，恢复协程
			}
			is_eof = 0;
		}
	}
	qs_schedule_free(sched);
	return;
}


