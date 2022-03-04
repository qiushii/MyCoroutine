#include "qs_coroutine.h"

#define FD_KEY(f,e)  (((uint64_t)(f)<<(sizeof(int32_t)*8))|e)
#define FD_EVENT(f) ((uint32_t)(f))
#define FD_ONLY(f)   ((f)>>((sizeof(int32_t)*8)))


/*�Ƚ�����Э�̵�˯��ʱ�䣬co1��co2���򷵻�-1*/
static inline int qs_coroutine_sleep_cmp(qs_coroutine *co1, qs_coroutine *co2){
	if(co1->sleep_usecs < co2->sleep_usecs){
		return -1;
	}
	if(co1->sleep_usecs == co2->sleep_usecs){
		return 0;
	}
	return 1;
}

/*�Ƚ�����Э�̵�fd��С*/
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


/*�����Э�̵�������С�ó�ʱʱ�䣬��Э�̵�˯��ʱ���й�ϵ*/
static uint64_t qs_schedule_min_timeout(qs_schedule *sched){
	uint64_t t_diff_usecs = qs_coroutine_diff_usecs(sched->birth, qs_coroutine_usec_now); //�����������������ʱ��
	uint64_t min = sched->default_timeout; 		//Ĭ�ϳ�ʱʱ��

	qs_coroutine *co = RB_MIN(_qs_coroutine_rbtree_sleep, &sched->sleeping); //ȡ������˯�ߵ�Э��
	if(!co)return min; 		//���û�д���˯������Э�̣��򷵻�Ĭ�ϵĳ�ʱʱ��

	min = co->sleep_usecs; //Э�̵�˯��ʱ��
	if(min > t_diff_usecs){//ʱ���
		return min - t_diff_usecs;
	}

	return 0;
}

/*�ȼ���epoll_wait�ȴ���ʱ���ٽ���epoll*/
static int qs_schedule_epoll(qs_schedule *sched){
	sched->num_new_events = 0;

	struct timespec t = {0, 0};
	uint64_t usecs = qs_schedule_min_timeout(sched);//��ȡ������
	if(usecs && TAILQ_EMPTY(&sched->ready)){  //���������������Э�̲���usecs��Ϊ0 ����epoll_waitһֱ�ȴ�
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

/*��˯������ȡ����*/
void qs_schedule_desched_sleepdown(qs_coroutine *co){
	if(co->status & BIT(QS_COROUTINE_STATUS_SLEEPING)){
		RB_REMOVE(_qs_coroutine_rbtree_sleep, &co->sched->sleeping, co);

		co->status &= CLEARBIT(QS_COROUTINE_STATUS_SLEEPING);
		co->status |= BIT(QS_COROUTINE_STATUS_READY);
		co->status &= CLEARBIT(QS_COROUTINE_STATUS_EXPIRED);
	}
}

/*ͨ��fd���ȴ��������˯�ߺ�����ϵ�Э��ȡ����*/
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


/*�ͷŵ�����*/
int qs_schedule_free(qs_schedule *sched){
    if(sched->poller_fd > 0){ //�رմ򿪵�epfd
        close(sched->poller_fd);
    }
    if(sched->eventfd > 0){
        close(sched->eventfd); //�رմ򿪵�eventfd
    }
    free(sched);//��ȫ�ͷŵ�������Դ
    
    assert(pthread_setpecific(gloabal_sched_key, NULL)==0);//���ԣ�����gloabal_sched_key��ֵΪ����NULL
}

/*����������*/
int qs_schedule_create(int stack_size){
   
    int sched_stack_size = stack_size ? stack_size : QS_CO_MAX_STACKSIZE; //�����߳�ջ��С�����Ϊ
    
    qs_schedule *sched = (qs_schedule*)calloc(1, sizeof(qs_schedule));    //Ϊ�����������ڴ�
    if(sched == NULL){
        printf("Failed to initialize scheduler\n");
        return -1;
    }

    assert(pthread_setpecific(gloabal_sched_key, sched) == 0);       //���ԣ�����������Ϊȫ�ּ�������Э��ʹ��
    
    sched->poller_fd = qs_epoller_create();     //����epoll�����
    if(sched->poller_fd == -1){//����ʧ��
        printf("failed to initialize\n");
        qs_schedule_free(sched);    //�ͷŵ�����
        return -2;
    }

    qs_epoller_ev_register_trigger();       //����epoll������

    sched->stack_size = sched_stack_size;    //���õ�����ջ��С
    sched->page_size = getpagesize();       //getpagesize()������ȡһ��ҳ�Ĵ�С

    sched->spawned_coroutines = 0; 		//�Ӵ�����Э������
    sched->default_timeout = 300000u;       //��������ʱʱ��

    RB_INIT(&sched->sleeping);              //����һ��˯�ߺ����
    RB_INIT(&sched->waiting);               //�����ȴ������
    
    sched->birth = qs_coroutine_usec_now(); //��ȡ����������ʱ��
    
    TAILQ_INIT(&sched->ready);              //������������
    
    LIST_INIT(&sched->busy);                //����æ�б�

    bzero(&sched->ctx, sizeof(qs_cpu_ctx)); //���������ָ�ʱ��ȡ�ļĴ�����ֵ��ʼ��Ϊ0
}

/*�жϵ��������Ƿ���ڿɵ��ȵ�Э��*/
static inline int qs_schedule_isdone(qs_schedule *sched){
	return  ((RB_EMPTY(&sched->waiting))&&
		      (LIST_EMPTY(&sched->busy))&&
		      (RB_EMPTY(&sched->sleeping))&&
		      (TAILQ_EMPTY(&sched->ready))
	);
}

/*����˯������ʱ�䵽�ڵ�Э��*/
static qs_coroutine *qs_schedule_expired(qs_schedule *sched){
	uint64_t t_diff_usecs = qs_coroutine_diff_usecs(sched->birth, qs_coroutine_usec_now()); //����������ʱ��
	qs_coroutine *co = RB_MIN(_qs_coroutine_rbtree_sleep, &sched->sleeping); //ȡ��˯�߶�������С��Э��	
	if(co==NULL)return NULL;

	if(co->sleep_usecs <= t_diff_usecs){//���Э��˯��ʱ�䵽���ˣ���Э�̴�����ժ����������Э��
		RB_REMOVE(_qs_coroutine_rbtree_sleep, &co->sched->sleeping, co); //ɾ���������˯�ߵĽڵ�
		return co;
	}
	return NULL;
}

/*�������ӵȴ��������ͨ��fd���ҳ�Э��*/
qs_coroutine *qs_schedule_search_wait(int fd){
	qs_coroutine find_it = {0};
	find_it.fd = fd;

	qs_schedule *sched = qs_coroutine_get_sched(); 	//��ȡ������

	qs_coroutine *co = RB_FIND(_qs_coroutine_retree_wait, &sched->waitting, &find_it);  //ͨ��fd�����ϲ���
	co->status = 0;

	return co;	
}

/*����co��˯��ʱ�䣬˯��ʲôʱ��*/
void qs_schedule_sched_sleepdown(qs_coroutine *co, uint64_t msecs){
	uint64_t usecs = msecs * 1000u;

	//��˯�������ҵ�co���Ƴ�����
	qs_coroutine *co_tmp = RB_FIND(_qs_coroutine_retree_sleep, co->sched->sleeping, co_tmp);
	if(co_tmp != NULL){
		RB_REMOVE(_qs_coroutine_rbtree_sleep, &co->sched->sleeping, co_tmp);
	}

	co->sleep_usecs = qs_coroutine_diff_usecs(co->sched->birth, qs_coroutine_usec_now)+usecs; 
	//��ʱ��ʱ��-����������ʱ��+msecs*1000u

	while(msecs){ //����˯����
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


/*����Э��epoll�¼����ͣ���Э�̷���ȴ�������ϣ�ͬʱ����Э�̷���˯�ߺ������*/
void qs_schedule_sched_wait(qs_coroutine *co, int fd, unsigned short events, uint64_t timeout){
	//����¼��Ѿ������óɵ�EPOLLIN����EPOLLOUT
	if(
		co->status & BIT(QS_COROUTINE_STATUS_WAIT_READ) || 		
		co->status & BIT(QS_COROUTINE_STATUS_WAIT_WRITE)){
		
		printf("Unexpected event. lt id %"PRIu64" fd %"PRId32" already in %"PRId32" state\n",
			co->id, co->fd, co->status);
		assert(0);
	}

	if(events & EPOLLIN){//�ж��¼����Ͳ�����status
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
								//��co���뵽�������ĵȴ�������

	assert(co_tmp==NULL);

	if(timeout==1)return;
 
	qs_schedule_sched_sleepdown(co, timeout);  //��co����˯�����ϣ�������˯��ʱ��
}

/*��Э�̴ӵȴ��������ɾ��*/
void qs_schedule_cancel_wait(qs_coroutine *co){
	RB_REMOVE(_qs_coroutine_rbtree_wait, &co->sched->waiting, co);
}

/*������������*/
void qs_schedule_run(void){
	
	qs_schedule *sched = qs_coroutine_get_sched(); //��ȡ��ǰ������
	if(sched == NULL)return;

	while(!qs_schedule_isdone(sched)){

		/*���˯��������û����˯�ߵ�Э�̣�����еĻ��ͽ���ȡ��������resume����*/
		/*����Ҳ��*/
		qs_coroutine *expired = NULL;
		while((expired = qs_schedule_expired(sched)) != NULL){
				qs_coroutine_resume(expired);
		}
		
		/*��������*/
		qs_coroutine *last_co_ready = TAILQ_LAST(&sched->ready, _qs_coroutine_queue); //��ȡ���һ��������Э��
		while(!TAILQ_EMPTY(&sched->ready)){   					//����������в��ǿյ�
			qs_coroutine *co = TAILQ_FIRST(&sched->ready); 		 //��ȡ��һ������Э��
			TAILQ_REMOVE(&co->sched->ready, co, ready_next);		  //�Ӿ��������Ƴ�

			if(&co->status & BIT(QS_COROUTINE_STATUS_FDEOF)){  	//���Э�����������
				qs_coroutine_free(co);
				break;
			}

			qs_coroutine_resume(co); 					//Э�ָ̻�����
			if(co== last_co_ready)break;
		}

		/*�ȴ������*/
		qs_schedule_epoll(sched);
		while(sched->num_new_events){  //������¼�
			int idx = --sched->num_new_events; 	//ȡ�¼��±�
			struct epoll_event *ev = sched->eventlist + idx; //ͨ���±�������ó��¼�
			
			int fd = ev->data.fd; 						//�¼�fd
			int is_eof = ev->events & EPOLLHUP;			//
			if(is_eof)errno = ECONNRESET;

			qs_coroutine *co = qs_schedule_search_wait(fd);    //ͨ��fd���ҳ�Э��
			if(co != NULL){ //�ҵ���
				if(is_eof){ //����Э���¼���EPOLLHUP�Ļ�
					co->status |= BIT(QS_COROUTINE_STATUS_FDEOF);
				}
				qs_coroutine_resume(co); 		//�ó�cpu���ָ�Э��
			}
			is_eof = 0;
		}
	}
	qs_schedule_free(sched);
	return;
}


