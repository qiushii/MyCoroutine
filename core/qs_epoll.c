#include<stdio.h>

/*����epfd*/
int qs_epoll_create(void){
    return epoll_create(1024);
}

/*epoll_wait*/
int qs_epoller_wait(struct timespec t){
    qs_schedule *sched = qs_coroutineget_sched(); //�ڲ�����prhread_getsepecific��ȡ������
    return epoll_wait(sched->poller_fd, sched->eventlist, QS_CO_MAX_EVENT, t.tv_sec*1000.0 + t.tv_nsec/100000.0);
            //����epoll 						����¼�           1024*1024	   			�� 		����
												/*(һ��ȡʱ�������ȡ�����´�ȡ)*/
}

int qs_epoller_ev_register_trigger(void){

    qs_schedule *sched = qs_coroutine_get_shced(); //�ڲ�����pthread_getsepecific��ȡ������
    if(!sched->eventfd){                            //���enentfdδ������
        sched->evnetfd = eventfd(0, EFD_NONBALOCK); // evnentfd���ڽ��̻����߳�֮��ͨ�ţ������¼��ļ�������������
        assert(sched->eventfd != -1);               //�����������ɹ� 
    }
    
    //����������epfd
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = sched->events;
    int ret = epoll_ctl(sched->poller_fd, EPOLL_CTL_ADD, sched->eventfd, &ev);

    assert(ret != -1);
}
