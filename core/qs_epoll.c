#include<stdio.h>

/*创建epfd*/
int qs_epoll_create(void){
    return epoll_create(1024);
}

/*epoll_wait*/
int qs_epoller_wait(struct timespec t){
    qs_schedule *sched = qs_coroutineget_sched(); //内部调用prhread_getsepecific获取调度器
    return epoll_wait(sched->poller_fd, sched->eventlist, QS_CO_MAX_EVENT, t.tv_sec*1000.0 + t.tv_nsec/100000.0);
            //设置epoll 						存放事件           1024*1024	   			秒 		纳秒
												/*(一次取时间得数量取不完下次取)*/
}

int qs_epoller_ev_register_trigger(void){

    qs_schedule *sched = qs_coroutine_get_shced(); //内部调用pthread_getsepecific获取调度器
    if(!sched->eventfd){                            //如果enentfd未被创建
        sched->evnetfd = eventfd(0, EFD_NONBALOCK); // evnentfd用于进程或者线程之间通信，创建事件文件描述符并返回
        assert(sched->eventfd != -1);               //断言他创建成功 
    }
    
    //创建并设置epfd
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = sched->events;
    int ret = epoll_ctl(sched->poller_fd, EPOLL_CTL_ADD, sched->eventfd, &ev);

    assert(ret != -1);
}
