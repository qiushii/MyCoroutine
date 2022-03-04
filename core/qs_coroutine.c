#include<stdio.h>

int _switch(qs_cpu_ctx *new_ctx, qs_cpu_ctx *old_ctx);
				    //存入rdi 				存入rsi
/*切换状态*/
#ifdef __i386__
__asm__(
"    .text                                  \n"
"    .p2align 2,,3                          \n"
".globl _switch                             \n"
"_switch:                                   \n"
"__switch:                                  \n"
"movl 8(%esp), %edx      # fs->%edx         \n"
"movl %esp, 0(%edx)      # save esp         \n"
"movl %ebp, 4(%edx)      # save ebp         \n"
"movl (%esp), %eax       # save eip         \n"
"movl %eax, 8(%edx)                         \n"
"movl %ebx, 12(%edx)     # save ebx,esi,edi \n"
"movl %esi, 16(%edx)                        \n"
"movl %edi, 20(%edx)                        \n"
"movl 4(%esp), %edx      # ts->%edx         \n"
"movl 20(%edx), %edi     # restore ebx,esi,edi      \n"
"movl 16(%edx), %esi                                \n"
"movl 12(%edx), %ebx                                \n"
"movl 0(%edx), %esp      # restore esp              \n"
"movl 4(%edx), %ebp      # restore ebp              \n"
"movl 8(%edx), %eax      # restore eip              \n"
"movl %eax, (%esp)                                  \n"
"ret                                                \n"
);
#elif defined(__x86_64__)
__asm__ (
"    .text                                  \n"
"       .p2align 4,,15                                   \n"
".globl _switch                                          \n"
".globl __switch                                         \n"
"_switch:                                                \n"
"__switch:                                               \n"	  

"#保留旧的工作环境         "
"#将rsi(new_ctx)放入寄存器"
" rsi-->cur_ctx  数字是对应偏移字节数"

"       movq %rsp, 0(%rsi)      # esp = rsp   栈顶    \n"		
"       movq %rbp, 8(%rsi)      # ebp = rbp   栈底     \n"	
"#rsi 偏移8，  对应new_ctx  的第2个值栈底指针"
"#rsi 偏移0，  对应new_ctx	的第1个值 栈顶指针"

"       movq (%rsp), %rax       # save insn_pointer      \n"		
"#将栈顶指针赋值给函数返回值，ret后出栈，执行栈顶"

"#rax = eip 函数返回值"
"#保留现场rbx r12-r15"
"       movq %rax, 16(%rsi)                              \n" 			
"       movq %rbx, 24(%rsi)     # 保存rbx,r12-r15 \n" 		
"       movq %r12, 32(%rsi)                              \n"			
"       movq %r13, 40(%rsi)                              \n" 			
"       movq %r14, 48(%rsi)                              \n" 			
"       movq %r15, 56(%rsi)                              \n"	  

"#新的工作环境"
" rdi-->new_ctx    "
"       movq 56(%rdi), %r15                              \n"	  				
"       movq 48(%rdi), %r14                              \n"			
"       movq 40(%rdi), %r13     # restore rbx,r12-r15    \n" 	
"       movq 32(%rdi), %r12                              \n"		
"       movq 24(%rdi), %rbx                              \n"			
"       movq 8(%rdi), %rbp      # restore frame_pointer  \n"	
"       movq 0(%rdi), %rsp      # restore stack_pointer  \n"	
"       movq 16(%rdi), %rax     # restore insn_pointer   \n" 

"#将函数返回值赋值给栈顶寄存器"
"       movq %rax, (%rsp)                                \n"

"#跳转执行栈顶，即进入新的工作空间"
"       ret                                              \n"
);
#endif

void qs_coroutine_yield(qs_coroutine *co){
	co->ops = 0;
	_switch(&co->sched->ctx, &co->ctx);
}

/*eip指令寄存器*/
/*协程初始化时eip指向的函数，里面包含了协程的执行内容co->func*/
static void _exec(void *lt){
#if defined(___lvm__)&&defined(__x86_64__)
	__asm__("movq 16(%%rbp), %[lt]" : [lt] "=r" lt));
#endif

	qs_coroutine *co = (qs_coroutine*) lt;
	co->func(co->arg);  		//进入入口函数
	co->status |= (BIT(QS_COROUTINE_STATUS_EXITED) |
			     	   BIT(QS_COROUTINE_STATUS_DETACH) |
				   BIT(QS_COROUTINE_STATUS_FDEOF));
#if 1
	qs_coroutine_yield(co);
#else
	co->ops = 0;
	_switch(&co->sched->ctx, &co->ctx);
#endif
}

static void qs_coroutine_init(qs_coroutine *co){
	void **stack = (void **)(co->stack + co->stack_size);  //栈底   高地址

	stack[-3] = NULL; 		 
	stack[-2] = (void*)co;   //exec的参数

	co->ctx.esp = (void*)stack - (4*sizeof(void*));  //设置栈顶--低地址
 	co->ctx.ebp = (void*)stack - (3*sizeof(void*));  //设置栈底--高地址
	co->ctx.eip = (void*)_exec;
	co->status = BIT(QS_COROUTINE_STATUS_READY);
}

static inline void qs_coroutine_madvise(qs_coroutine *co){
	size_t current_stack = (co->stack + co->stack_size) - co->ctx.esp; //计算出当前栈顶距离栈底的差
	assert(current_stack <= co->stack_size); 	//断言，栈差没有大于栈的范围

	//如果当前栈差小于
	if(current_stack < co->last_stack_size && co->last_stack_size > co->sched->page_size){
		size_t tmp = current_stack + (-current_stack & (co->sched->page_size - 1 ) );
		assert(madvise(co->stack, co->stack_size-tmp, MADV_DONTNEED) == 0);
		/****************************************************************************************************************************
		  函数建议内核，在从 addr 指定的地址开始，长度等于 len 参数值的范围内，
		  该区域的用户虚拟内存应遵循特定的使用模式。内核使用这些信息优化与
		  指定范围关联的资源的处理和维护过程。
		*****************************************************************************************************************************/
	} 
	co->last_stack_size = current_stack;    	//
}

/*恢复协程运行*/
int qs_coroutine_resume(qs_coroutine *co){
	if(co->status & BIT(QS_COROUTINE_STATUS_NEW)){ //如果协程是新创建的则初始化
		qs_coroutine_init(co);
	}

	qs_schedule *sched = qs_coroutine_get_sched();  //获取当前调度器
	
	sched->curr_thread = co; 	//将调度器的当前协程设置为要恢复的协程
	_switch(&co->ctx, &co->shced->ctx); //切换CPU 寄存器工作现场
	sched->curr_thread = NULL; ////将调度器的当前协程设置为空

	qs_coroutine_madvise(co);
}


void qs_coroutine_renice(qs_coroutine *co){
	co->ops ++;
#if 1
	if(co->ops < 5)return;
#endif
	printf("qs_coroutine_renice\n");
	TAILQ_INSERT_TAIL(&qs_coroutine_get_sched->ready,co,ready_next);
	printf("qs_coroutine_renice ok\n");
	qs_coroutine_yield(co);
}


pthread_key_t global_sched_key;
static pthread_once_t sched_key_once = PTHREAD_ONCE_INIT;

static void qs_coroutine_sched_key_destructor(void *data){
    free(data);
}
static void qs_coroutinr_sched_key_creator(void){
    assert(pthread_key_create(&global_sched_key, qs_coroutine_sched_key_destructor);
    assert(pthread_setspecific(global_sched_key,NULL)==0);
    return;
}

/*将协程设置成睡眠并设置睡到什么时候*/
void qs_coroutine_sleep(uint64_t msecs){
	qs_coroutine *co = qs_coroutine_get_sched();			//获取调度器
	if(msecs == 0){
		TAILQ_INSERT_TAIL(&co->shced->ready, co, read_next);  //加入就绪队列
		qs_coroutine_yield(co);			//让出cpu
	}else{
		qs_schedule_sched_sleepdown(co, msecs);
	}
}

/*设成协程分离的状态*/
void qs_coroutine_detach(void){
	qs_coroutine *co = qs_coroutine_get_sched()->curr_thread;
	co->status |= BIT(QS_COROUTINE_STATUS_DETACH);
}

/*释放协程*/
void qs_coroutine_free(qs_coroutine *co){
    if(co==NULL)return;
        co->sched->spawned_coroutines--;//
#if 1:
    if(co->stack){
        free(co->stack);                //释放协程栈
        co->stack = NULL;
    }
#endif
    if(co){
        free(co);                       //释放协程co
    }
}

/*协程初始化，初始化协程co*/
static void qs_coroutine_init(qs_coroutine *co){
    void **stack = (void**)(co->stack + co->stack_size);   //栈的范围

    stack[-3] = NULL;   		//栈的生长方向是由高到低，所以是负3
    stack[-2] = (void*)co;   //存放协程信息

    co->ctx.esp = (void*)stack - (4*sizeof(void*));     //栈顶
    co->ctx.ebp = (void*)stack - (3*sizeof(void*));    //栈底
    co->ctx.eip = (void*)_exec; 					 //指令寄存器
    co->status = BIT(QS_COROUTINE_STATUS_READY);
}

/*创建协程*/
int qs_coroutine_create(qs_coroutine **new_co, proc_coroutine func, void *arg){
    
    assert(pthread_once(&sched_key_once, qs_coroutine_sched_key_creator)==0);
                                    //确保qs_coroutine_sched_key_creator 只被执行一次

    qs_schedule *sched = qs_coroutine_get_sched();//通过pthread_getspecific获取全局键值调度器
    if(sched == NULL){ //如果调度器未被创建或者获取失败
        qs_schedule_create(0);  //重新创建
        if(sched==NULL){
            printf("Failed to create shceduler\n");
            return -1;
        }
    }
    
    qs_coroutine *co = callloc(1, sizeof(qs_coroutine));    //创建并初始化协程
    if(co==NULL){
        printf("Fialed to allocate memory fot new coroutine\n");
        return -2;
    }

    int ret = posix_memalign(&co->stack, getpagesize(), sched->stack_size); //将协程栈分配为一个页的大小
						//目标栈 	//最小倍数   //内存大小      
    /*****************************************************************************************************************************************************
        在编写可移植的代码的时候，所有的类型都该自然对齐，因为不对齐会导致性能下降，
        在大多数情况下，编译器和C库透明地帮你处理对齐问题。POSIX 标明了通过malloc( ), calloc( ), 
        和 realloc( ) 返回的地址对于任何的C类型来说都是对齐的。在Linux中，这些函数返回的地址在
        32位系统是以8字节为边界对齐，在64位系统是以16字节为边界对齐的。但是对于更大的边界
        例如页面，程序员需要动态的对齐。因此，POSIX 1003.1d提供一个叫做posix_memalign( )的函数
       *******************************************************************************************************************************************************/
    if(ret){ //如果分配失败
        printf("Failed to allocate stack for new cotroutine\n");
        free(co);
        return -3;
    }
    /*赋值协程参数*/
    co->sched = sched;                         
    co->stack_size = sched->stack_size;      
    co->status = BIT(QS_COROUTINE_STATUS_NEW);  
    co->id = sched->spwaned_coroutines++;       
    co->func = func;                       

#if CANCEL_FD_WAIT_UINT64
    co->fd = -1;                
    co->events = 0;
#else
    co->fd_wait = -1;
#endif
    co->arg = arg;                             
    co->birth = qs_coroutine_usec_now();        
    *new_co = co;                               

    TALIQ_INSERT_TAIL(&co->shced->ready, co, ready_next); //将协程加入就绪队列

    return 0;
}
