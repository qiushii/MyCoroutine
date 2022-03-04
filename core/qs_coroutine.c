#include<stdio.h>

int _switch(qs_cpu_ctx *new_ctx, qs_cpu_ctx *old_ctx);
				    //����rdi 				����rsi
/*�л�״̬*/
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

"#�����ɵĹ�������         "
"#��rsi(new_ctx)����Ĵ���"
" rsi-->cur_ctx  �����Ƕ�Ӧƫ���ֽ���"

"       movq %rsp, 0(%rsi)      # esp = rsp   ջ��    \n"		
"       movq %rbp, 8(%rsi)      # ebp = rbp   ջ��     \n"	
"#rsi ƫ��8��  ��Ӧnew_ctx  �ĵ�2��ֵջ��ָ��"
"#rsi ƫ��0��  ��Ӧnew_ctx	�ĵ�1��ֵ ջ��ָ��"

"       movq (%rsp), %rax       # save insn_pointer      \n"		
"#��ջ��ָ�븳ֵ����������ֵ��ret���ջ��ִ��ջ��"

"#rax = eip ��������ֵ"
"#�����ֳ�rbx r12-r15"
"       movq %rax, 16(%rsi)                              \n" 			
"       movq %rbx, 24(%rsi)     # ����rbx,r12-r15 \n" 		
"       movq %r12, 32(%rsi)                              \n"			
"       movq %r13, 40(%rsi)                              \n" 			
"       movq %r14, 48(%rsi)                              \n" 			
"       movq %r15, 56(%rsi)                              \n"	  

"#�µĹ�������"
" rdi-->new_ctx    "
"       movq 56(%rdi), %r15                              \n"	  				
"       movq 48(%rdi), %r14                              \n"			
"       movq 40(%rdi), %r13     # restore rbx,r12-r15    \n" 	
"       movq 32(%rdi), %r12                              \n"		
"       movq 24(%rdi), %rbx                              \n"			
"       movq 8(%rdi), %rbp      # restore frame_pointer  \n"	
"       movq 0(%rdi), %rsp      # restore stack_pointer  \n"	
"       movq 16(%rdi), %rax     # restore insn_pointer   \n" 

"#����������ֵ��ֵ��ջ���Ĵ���"
"       movq %rax, (%rsp)                                \n"

"#��תִ��ջ�����������µĹ����ռ�"
"       ret                                              \n"
);
#endif

void qs_coroutine_yield(qs_coroutine *co){
	co->ops = 0;
	_switch(&co->sched->ctx, &co->ctx);
}

/*eipָ��Ĵ���*/
/*Э�̳�ʼ��ʱeipָ��ĺ��������������Э�̵�ִ������co->func*/
static void _exec(void *lt){
#if defined(___lvm__)&&defined(__x86_64__)
	__asm__("movq 16(%%rbp), %[lt]" : [lt] "=r" lt));
#endif

	qs_coroutine *co = (qs_coroutine*) lt;
	co->func(co->arg);  		//������ں���
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
	void **stack = (void **)(co->stack + co->stack_size);  //ջ��   �ߵ�ַ

	stack[-3] = NULL; 		 
	stack[-2] = (void*)co;   //exec�Ĳ���

	co->ctx.esp = (void*)stack - (4*sizeof(void*));  //����ջ��--�͵�ַ
 	co->ctx.ebp = (void*)stack - (3*sizeof(void*));  //����ջ��--�ߵ�ַ
	co->ctx.eip = (void*)_exec;
	co->status = BIT(QS_COROUTINE_STATUS_READY);
}

static inline void qs_coroutine_madvise(qs_coroutine *co){
	size_t current_stack = (co->stack + co->stack_size) - co->ctx.esp; //�������ǰջ������ջ�׵Ĳ�
	assert(current_stack <= co->stack_size); 	//���ԣ�ջ��û�д���ջ�ķ�Χ

	//�����ǰջ��С��
	if(current_stack < co->last_stack_size && co->last_stack_size > co->sched->page_size){
		size_t tmp = current_stack + (-current_stack & (co->sched->page_size - 1 ) );
		assert(madvise(co->stack, co->stack_size-tmp, MADV_DONTNEED) == 0);
		/****************************************************************************************************************************
		  ���������ںˣ��ڴ� addr ָ���ĵ�ַ��ʼ�����ȵ��� len ����ֵ�ķ�Χ�ڣ�
		  ��������û������ڴ�Ӧ��ѭ�ض���ʹ��ģʽ���ں�ʹ����Щ��Ϣ�Ż���
		  ָ����Χ��������Դ�Ĵ����ά�����̡�
		*****************************************************************************************************************************/
	} 
	co->last_stack_size = current_stack;    	//
}

/*�ָ�Э������*/
int qs_coroutine_resume(qs_coroutine *co){
	if(co->status & BIT(QS_COROUTINE_STATUS_NEW)){ //���Э�����´��������ʼ��
		qs_coroutine_init(co);
	}

	qs_schedule *sched = qs_coroutine_get_sched();  //��ȡ��ǰ������
	
	sched->curr_thread = co; 	//���������ĵ�ǰЭ������ΪҪ�ָ���Э��
	_switch(&co->ctx, &co->shced->ctx); //�л�CPU �Ĵ��������ֳ�
	sched->curr_thread = NULL; ////���������ĵ�ǰЭ������Ϊ��

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

/*��Э�����ó�˯�߲�����˯��ʲôʱ��*/
void qs_coroutine_sleep(uint64_t msecs){
	qs_coroutine *co = qs_coroutine_get_sched();			//��ȡ������
	if(msecs == 0){
		TAILQ_INSERT_TAIL(&co->shced->ready, co, read_next);  //�����������
		qs_coroutine_yield(co);			//�ó�cpu
	}else{
		qs_schedule_sched_sleepdown(co, msecs);
	}
}

/*���Э�̷����״̬*/
void qs_coroutine_detach(void){
	qs_coroutine *co = qs_coroutine_get_sched()->curr_thread;
	co->status |= BIT(QS_COROUTINE_STATUS_DETACH);
}

/*�ͷ�Э��*/
void qs_coroutine_free(qs_coroutine *co){
    if(co==NULL)return;
        co->sched->spawned_coroutines--;//
#if 1:
    if(co->stack){
        free(co->stack);                //�ͷ�Э��ջ
        co->stack = NULL;
    }
#endif
    if(co){
        free(co);                       //�ͷ�Э��co
    }
}

/*Э�̳�ʼ������ʼ��Э��co*/
static void qs_coroutine_init(qs_coroutine *co){
    void **stack = (void**)(co->stack + co->stack_size);   //ջ�ķ�Χ

    stack[-3] = NULL;   		//ջ�������������ɸߵ��ͣ������Ǹ�3
    stack[-2] = (void*)co;   //���Э����Ϣ

    co->ctx.esp = (void*)stack - (4*sizeof(void*));     //ջ��
    co->ctx.ebp = (void*)stack - (3*sizeof(void*));    //ջ��
    co->ctx.eip = (void*)_exec; 					 //ָ��Ĵ���
    co->status = BIT(QS_COROUTINE_STATUS_READY);
}

/*����Э��*/
int qs_coroutine_create(qs_coroutine **new_co, proc_coroutine func, void *arg){
    
    assert(pthread_once(&sched_key_once, qs_coroutine_sched_key_creator)==0);
                                    //ȷ��qs_coroutine_sched_key_creator ֻ��ִ��һ��

    qs_schedule *sched = qs_coroutine_get_sched();//ͨ��pthread_getspecific��ȡȫ�ּ�ֵ������
    if(sched == NULL){ //���������δ���������߻�ȡʧ��
        qs_schedule_create(0);  //���´���
        if(sched==NULL){
            printf("Failed to create shceduler\n");
            return -1;
        }
    }
    
    qs_coroutine *co = callloc(1, sizeof(qs_coroutine));    //��������ʼ��Э��
    if(co==NULL){
        printf("Fialed to allocate memory fot new coroutine\n");
        return -2;
    }

    int ret = posix_memalign(&co->stack, getpagesize(), sched->stack_size); //��Э��ջ����Ϊһ��ҳ�Ĵ�С
						//Ŀ��ջ 	//��С����   //�ڴ��С      
    /*****************************************************************************************************************************************************
        �ڱ�д����ֲ�Ĵ����ʱ�����е����Ͷ�����Ȼ���룬��Ϊ������ᵼ�������½���
        �ڴ��������£���������C��͸���ذ��㴦��������⡣POSIX ������ͨ��malloc( ), calloc( ), 
        �� realloc( ) ���صĵ�ַ�����κε�C������˵���Ƕ���ġ���Linux�У���Щ�������صĵ�ַ��
        32λϵͳ����8�ֽ�Ϊ�߽���룬��64λϵͳ����16�ֽ�Ϊ�߽����ġ����Ƕ��ڸ���ı߽�
        ����ҳ�棬����Ա��Ҫ��̬�Ķ��롣��ˣ�POSIX 1003.1d�ṩһ������posix_memalign( )�ĺ���
       *******************************************************************************************************************************************************/
    if(ret){ //�������ʧ��
        printf("Failed to allocate stack for new cotroutine\n");
        free(co);
        return -3;
    }
    /*��ֵЭ�̲���*/
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

    TALIQ_INSERT_TAIL(&co->shced->ready, co, ready_next); //��Э�̼����������

    return 0;
}
