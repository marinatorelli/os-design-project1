#include <stdio.h>
#include <sys/time.h>
#include <signal.h>
#include <stdlib.h>
#include <ucontext.h>
#include <unistd.h>
#include "my_io.h"

//#include "mythread.h"
#include "interrupt.h"

#include "queue.h"

TCB* scheduler();
void activator();
void timer_interrupt(int sig);
void disk_interrupt(int sig);


/* Array of state thread control blocks: the process allows a maximum of N threads */
static TCB t_state[N]; 

/* Queue of ready processes */
struct queue* high_queue;
struct queue* low_queue;
struct queue* waiting_queue;

/* Current running thread */
static TCB* running;
static int current = 0;

/* Variable indicating if the library is initialized (init == 1) or not (init == 0) */
static int init=0;

/* Thread control block for the idle thread */
static TCB idle;

static void idle_function()
{
  while(1);
}

void function_thread(int sec)
{
    //time_t end = time(NULL) + sec;
    while(running->remaining_ticks)
    {
		//Do something
    }
    mythread_exit();
}


/* Initialize the thread library */
void init_mythreadlib() 
{
  int i;

  /* Create context for the idle thread */
  if(getcontext(&idle.run_env) == -1)
  {
    perror("*** ERROR: getcontext in init_thread_lib");
    exit(-1);
  }

  idle.state = IDLE;
  idle.priority = SYSTEM;
  idle.function = idle_function;
  idle.run_env.uc_stack.ss_sp = (void *)(malloc(STACKSIZE));
  idle.tid = -1;

  if(idle.run_env.uc_stack.ss_sp == NULL)
  {
    printf("*** ERROR: thread failed to get stack space\n");
    exit(-1);
  }

  idle.run_env.uc_stack.ss_size = STACKSIZE;
  idle.run_env.uc_stack.ss_flags = 0;
  idle.ticks = QUANTUM_TICKS;
  makecontext(&idle.run_env, idle_function, 1); 

  t_state[0].state = INIT;
  t_state[0].priority = LOW_PRIORITY;
  t_state[0].ticks = QUANTUM_TICKS;
  t_state[0].remaining_ticks = seconds_to_ticks(3);
  printf("%d\n", t_state[0].remaining_ticks);

  if(getcontext(&t_state[0].run_env) == -1)
  {
    perror("*** ERROR: getcontext in init_thread_lib");
    exit(5);
  }	

  for(i=1; i<N; i++)
  {
    t_state[i].state = FREE;
  }

  t_state[0].tid = 0;
  running = &t_state[0];

  /* Initialize disk and clock interrupts */
  init_disk_interrupt();
  init_interrupt();
  
  /* Initialize both queues */
  high_queue = queue_new();
  low_queue = queue_new();
  waiting_queue = queue_new();
}


/* Create and intialize a new thread with body fun_addr and one integer argument */ 
int mythread_create (void (*fun_addr)(),int priority,int seconds)
{
  int i;
  
  if (!init) { init_mythreadlib(); init=1;}

  for (i=0; i<N; i++)
    if (t_state[i].state == FREE) break;

  if (i == N) return(-1);

  if(getcontext(&t_state[i].run_env) == -1)
  {
    perror("*** ERROR: getcontext in my_thread_create");
    exit(-1);
  }

  t_state[i].state = INIT;
  t_state[i].priority = priority;
  t_state[i].function = fun_addr;
  t_state[i].execution_total_ticks = seconds_to_ticks(seconds);
  t_state[i].remaining_ticks = t_state[i].execution_total_ticks;
  t_state[i].run_env.uc_stack.ss_sp = (void *)(malloc(STACKSIZE));
  
  if(t_state[i].run_env.uc_stack.ss_sp == NULL)
  {
    printf(" *** ERROR: thread failed to get stack space\n");
    exit(-1);
  }

  t_state[i].tid = i;
  t_state[i].run_env.uc_stack.ss_size = STACKSIZE;
  t_state[i].run_env.uc_stack.ss_flags = 0;
  makecontext(&t_state[i].run_env, fun_addr,2,seconds);
  
  t_state[i].ticks = QUANTUM_TICKS;
  
  if (priority == HIGH_PRIORITY) {
	if (running->priority == LOW_PRIORITY) {
	  running->ticks = LOW_PRIORITY;
	  disable_interrupt();
      disable_disk_interrupt();
      enqueue(low_queue, running);
      enable_disk_interrupt();
      enable_interrupt();
	  printf(" *** ENQUEUED THREAD %d OF LOW PRIORITY PREEMPTED BY THREAD %d\n", running->tid, i);
	  activator(&t_state[i]);
    }
	else if (running->priority == HIGH_PRIORITY && t_state[i].execution_total_ticks < running->remaining_ticks) {
	  disable_interrupt();
      disable_disk_interrupt();
      sorted_enqueue(high_queue, running, running->remaining_ticks);
      enable_disk_interrupt();
      enable_interrupt();
	  printf(" *** REMAINING TICKS FOR THREAD %d : %d\n", running->tid, running->remaining_ticks);
	  printf("%d\n", t_state[i].remaining_ticks);
	  printf(" *** ENQUEUED THREAD %d OF HIGH_PRIORITY PREEMPTED BY THREAD %d\n", running->tid, i);
	  activator(&t_state[i]);
	} 
	else {
	  disable_interrupt();
      disable_disk_interrupt();
      sorted_enqueue(high_queue, &t_state[i], t_state[i].execution_total_ticks);
      enable_disk_interrupt();
      enable_interrupt();
	  printf(" *** ENQUEUED THREAD %d OF HIGH_PRIORITY\n", i);
	}
  } 
  else {
    disable_interrupt();
    disable_disk_interrupt();
    enqueue(low_queue, &t_state[i]);
    enable_disk_interrupt();
    enable_interrupt();
	printf(" *** ENQUEUED THREAD %d OF LOW_PRIORITY\n", i);
  }

  return i;
} 
/****** End my_thread_create() ******/


/* Read disk syscall */
int read_disk()
{
  if (data_in_page_cache() != 0) {
    running->state = WAITING;
    disable_interrupt();
    disable_disk_interrupt();
    enqueue(waiting_queue, running);
    enable_disk_interrupt();
    enable_interrupt(); 
	TCB* next_process = scheduler();
	activator(next_process);
  }
  else {
    printf(" *** TRHEAD %d READ FROM DISK : DATA IN CACHE\n", running->tid); 
  }
  return 1;
}

/* Disk interrupt  */
void disk_interrupt(int sig)
{
  if (!queue_empty(waiting_queue)) {
    disable_interrupt();
    disable_disk_interrupt();
    TCB* ready = dequeue(waiting_queue);
    enable_disk_interrupt();
    enable_interrupt();
	ready->state = INIT;
	printf(" *** THREAD %d READY\n", ready->tid);
    if (ready->priority == HIGH_PRIORITY) {
      disable_interrupt();
      disable_disk_interrupt();
      sorted_enqueue(high_queue, ready, ready->remaining_ticks);
      enable_disk_interrupt();
      enable_interrupt();
    }
    else {
      disable_interrupt();
      disable_disk_interrupt();
      enqueue(low_queue, ready);
      enable_disk_interrupt();
      enable_interrupt();
    }  
  }
}


/* Free terminated thread and exits */
void mythread_exit() {
  int tid = mythread_gettid();	

  printf(" *** THREAD %d FINISHED\n", tid);	
  t_state[tid].state = FREE;
  free(t_state[tid].run_env.uc_stack.ss_sp); 

  TCB* next = scheduler();
  activator(next);
}


void mythread_timeout(int tid) {

    printf(" *** THREAD %d EJECTED\n", tid);
    t_state[tid].state = FREE;
    free(t_state[tid].run_env.uc_stack.ss_sp);

    TCB* next = scheduler();
    activator(next);
}


/* Sets the priority of the calling thread */
void mythread_setpriority(int priority) 
{
  int tid = mythread_gettid();	
  t_state[tid].priority = priority;
  if(priority ==  HIGH_PRIORITY){
    t_state[tid].remaining_ticks = 195;
  }
}

/* Returns the priority of the calling thread */
int mythread_getpriority(int priority) 
{
  int tid = mythread_gettid();	
  return t_state[tid].priority;
}


/* Get the current thread id.  */
int mythread_gettid(){
  if (!init) { init_mythreadlib(); init=1;}
  return current;
}


/* SJF para alta prioridad, RR para baja*/

TCB* scheduler()
{
  if (!queue_empty(high_queue)) {
	 disable_interrupt();
     disable_disk_interrupt();
     TCB* next = dequeue(high_queue);
     enable_disk_interrupt();
     enable_interrupt();
	 return next;
  }
  if (!queue_empty(low_queue)) {
	 disable_interrupt();
     disable_disk_interrupt();
     TCB* next = dequeue(low_queue);
     enable_disk_interrupt();
     enable_interrupt();
	 return next;
  }
  if (!queue_empty(waiting_queue)) {
    return &idle;
  }
  printf(" *** FINISH\n");
  exit(1);
}


/* Timer interrupt */
void timer_interrupt(int sig)
{
  if (running->state == IDLE) {
    TCB* next_process = scheduler();
	if (next_process->state != IDLE) {
      activator(next_process);
	}
  }
  running->remaining_ticks--;
  if (running->priority == LOW_PRIORITY) {
	running->ticks--;
	if (!queue_empty(high_queue) || running->ticks == 0) {
	  running->ticks = QUANTUM_TICKS;
	  disable_interrupt();
      disable_disk_interrupt();
      enqueue(low_queue, running);
      enable_disk_interrupt();
      enable_interrupt();
	  TCB* next_process = scheduler();
	  if (running->tid != next_process->tid) {
        activator(next_process);
	  }
	}
  }
} 

/* Activator */
void activator(TCB* next)
{
  if (running->state == FREE) {
	printf(" *** THREAD %d FINISHED : SET CONTEXT OF %d\n", running->tid, next->tid);
	running = next;
	current = running->tid;
    if(setcontext(&(running->run_env)) == -1){
      perror("*** ERROR: SETCONTEXT");
      exit(-1);
    }
    printf("mythread_free: After setcontext, should never get here!!...\n");	
  }
  else if (running->state == WAITING) {
    printf(" *** THREAD %d READ FROM DISK : SET CONTEXT OF %d\n", running->tid, next->tid);
  }	  
  else if (running->state == IDLE) {
	printf(" *** THREAD READY : SET CONTEXT TO %d\n", next->tid);
  }
  else if (next->priority == HIGH_PRIORITY) {
	printf(" *** THREAD %d PREEMPTED : SET CONTEXT OF %d\n", running->tid, next->tid);
  } 
  else {
	printf(" *** SWAPCONTEXT FROM %d to %d\n", running->tid, next->tid); 
  }
  TCB* prev = running;
  running = next;
  current = running->tid;
  if(swapcontext (&(prev->run_env), &(next->run_env)) == -1){
      perror("*** ERROR: SWAPCONTEXT");
      exit(-1);
  }
}