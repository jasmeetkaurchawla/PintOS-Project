#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* wait_list fo threads in timer sleep*/
static struct list wait_list;

/*Binary Semaphore used to control who can access the sleeping list*/
static struct semaphore sema_sleep;

static struct FIXED17_14 load_avg; // The average amount of threads over the last minute that are waiting in the ready list.

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

//Sorting function for treads in a given list based on priority.(sorts in a greater than fashion rather than a less than one as described by list.h)
//Sorted like this because in pintos larger priorities have higher priority; as apposed to 1 being the highest, 64 is the highest in this case.
static bool sort_threads_priority(const struct list_elem *a, const struct list_elem *b, void *aux){
	struct thread *t1 = list_entry(a, struct thread, elem); //See foreach thread function call for example of how this works
	struct thread *t2 = list_entry(b, struct thread, elem); //"                                                             "
	return t1->priority > t2->priority;
}


/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init(&wait_list);
  sema_init(&sema_sleep, 1);
  load_avg = int_to_fixed17_14(0);					//Load avg is 0 on startup

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;
  
  
 //Check to see if any threads are waiting to wakeup
  if(list_size(&wait_list) != 0){
	 struct list_elem *e;
	 struct thread *next_awake;
	int64_t current_tick = idle_ticks + user_ticks + kernel_ticks; //No other way to check how many timer ticks have passed
	 
	 /*If the next thread to wakeup has to wake up because we are passed its wakeup time, then we have at least one thread to wake up.
			If no threads in this list need to wake up, the first element check will just break the loop anyway*/
	 for(e = list_begin(&wait_list); e != list_end(&wait_list); e = list_next(e)){
		 next_awake = list_entry(e, struct thread , waitelem);
		 if(next_awake->wakeup_time > current_tick){break;}// This would mean that we have reached the end of the threads that need to be awake
		 else{
			 sema_up(&next_awake->sema_wakeup);
			 next_awake->wakeup_time = 0;
			 list_pop_front(&wait_list);
		 }
	 }
	
  }


  if(thread_mlfqs){		//advanced scheduler enabled, must do our checks for priority changes
	struct thread *current = thread_current();
	current-> recent_cpu.full = current -> recent_cpu.full + 1*CONVERT;
	
	if(timer_ticks()%TIMER_FREQ == 0){
		
		update_load_avg();									//Will update the load_avg before updating recent_cpu
		
		struct list_elem *e;
		for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))//Will update the recent_cpu of every thread.
			{
			  struct thread *t = list_entry (e, struct thread, allelem);
			  update_recent_cpu(t);
			}
										
	}//Every second we calculate load_avg and recent_cpu of all threads.
	
	if(timer_ticks()%4 == 0){
		struct list_elem *e;
		for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))
			{
			  struct thread *t = list_entry (e, struct thread, allelem);
			  calculate_priority(t);
			}
		list_sort(&ready_list, sort_threads_priority, NULL);//Priorities were updated so we have to resort the ready list;
		
		//must check for preemption here in case new priority is less than the next scheduled thread.
		if(!list_empty(&ready_list)){//if list is empty no need for checking this
			struct thread *next = list_entry(list_next(&ready_list), struct thread, elem);// grabs the next scheduled thread on the ready list
			if (next!= NULL && t->priority < next->priority){intr_yield_on_return();}//If current priority is less than next scheduled, yield the thread.
		}
	}//Every 4 ticks, recalculate all priorities
  }

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();

  
}

/*Function updates the load_avg. This function should be called once a second.*/
void update_load_avg(void){
	struct FIXED17_14 value59_60 = divide_fixed17_14(int_to_fixed17_14(59),int_to_fixed17_14(60));
	struct FIXED17_14 value1_60 = divide_fixed17_14(int_to_fixed17_14(1), int_to_fixed17_14(60));
	struct FIXED17_14 ready = int_to_fixed17_14(list_size(&ready_list));				//Gives # of ready threads
	if(thread_current()!=idle_thread){ready.full = ready.full + 1*CONVERT;}
	struct FIXED17_14 new_load = multiply_fixed17_14(value59_60, load_avg);				//(59/60)*load_avg
	ready = multiply_fixed17_14(ready, value1_60);										//(1/60)*ready_threads
	
	load_avg.full = new_load.full + ready.full;											//(59/60)*load_avg + (1/60)*ready_threads
	//End load_avg calcs
}

/*Function updates the current_cpu of the thread that is passed into the function. This should only be called once a second for every existing thread.*/
void update_recent_cpu(struct thread *t){
	struct FIXED17_14 value1 = int_to_fixed17_14(1);
	struct FIXED17_14 double_load;
	double_load.full = load_avg.full*2;									//double the load avg;
	
	struct FIXED17_14 dl_plus1;
	dl_plus1.full = double_load.full + value1.full;						//2*load_avg +1
	
	struct FIXED17_14 c_cpu = divide_fixed17_14(double_load, dl_plus1);	//coefficient of recent_cpu ie (2*load_avg)/(2*load_avg +1)
	struct FIXED17_14 nice = int_to_fixed17_14(t -> nice);				//gets the threads nice value.
	
	t->recent_cpu = multiply_fixed17_14(c_cpu, t -> recent_cpu);
	t->recent_cpu.full = t->recent_cpu.full + nice.full;
	//End update to this threads recent_cpu
	
}

/*Function updates the priority of the thread that is passed into the function. This should only be called once every 4 ticks for every existing thread.*/
void calculate_priority(struct thread *t){
	struct FIXED17_14 max = int_to_fixed17_14(PRI_MAX);
	struct FIXED17_14 dnice = int_to_fixed17_14(2*t->nice);
	struct FIXED17_14 rcpu_4 = divide_fixed17_14(t->recent_cpu, int_to_fixed17_14(4));
	max.full = max.full - rcpu_4.full - dnice.full;
	t->priority = max.full/CONVERT;
	if(t->priority <PRI_MIN){t-> priority = PRI_MIN;}
	else if(t->priority > PRI_MAX){ t-> priority = PRI_MAX;}	//If priority is too high, then assign priority to be 63;
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  //inserting into ready list, must insert in the proper priority order.
  list_insert_ordered (&ready_list, &t->elem, sort_threads_priority, NULL); //See sort_threads_priority for details on the comparator used here
  t->status = THREAD_READY;
  //if the inserted thread has a lower priority than the currently running thread, we must yield the current thread before unblocking this one
  //must check that current thread is not an idle thread as well to avoid yielding the idle thread
  if( thread_current() != idle_thread && (thread_current()->priority < t->priority)) {thread_yield();}//only yield given these conditions on the new unblocked thread
  intr_set_level (old_level);
}

static bool compare_wakeup_time (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{

const struct thread *t1 = list_entry (a, struct thread, waitelem);
const struct thread *t2 = list_entry (b, struct thread, waitelem);

return t1->wakeup_time < t2->wakeup_time;
}

void thread_sleep(int64_t wakeup_time)
{
	struct thread *t = thread_current();
	t->wakeup_time = wakeup_time;
	sema_down(&sema_sleep); /*Ensures that no two threads are able to access the wait list at a single time*/
	list_insert_ordered(&wait_list, &t->waitelem, compare_wakeup_time, NULL); //Inserts threads in ordered of having to wake up first
	sema_up(&sema_sleep);
    sema_down(&t->sema_wakeup);				//semaphore used to only force the thread to sleep??
		
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_insert_ordered (&ready_list, &cur->elem, sort_threads_priority, NULL);//insert yielded thread into ready list in the proper order
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to new_priority. */
void
thread_set_priority (int new_priority) 
{
  if(!thread_mlfqs){
	  ASSERT (new_priority<=PRI_MAX && new_priority>=PRI_MIN);//Check that the new priority is valid;
	  struct thread *t = thread_current();
	  t->priority = new_priority;
	  t->original_priority = new_priority;
	  //must check for preemption here in case new priority is less than the next scheduled thread.
	  if(!list_empty(&ready_list)){//if list is empty no need for checking this
		  struct thread *next = list_entry(list_next(&ready_list), struct thread, elem);// grabs the next scheduled thread on the ready list
		  if (next!= NULL && t->priority < next->priority){thread_yield();}//If current priority is less than next scheduled, yield the thread.
	  }
  }
}

//Allows the thread's priority to change via a donation --- Currently unimplemented.
void thread_donate_priority(int donated_priority){
	
}

/* Returns the current thread's priority(could be the donated priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE.*/
void
thread_set_nice (int new_nice) 
{
	ASSERT(-20 <= new_nice && new_nice <= 20);
	thread_current() ->nice = new_nice;
	calculate_priority(thread_current());
	
	//must check for preemption here in case new priority is less than the next scheduled thread.
	  if(!list_empty(&ready_list)){//if list is empty no need for checking this
		  struct thread *next = list_entry(list_next(&ready_list), struct thread, elem);// grabs the next scheduled thread on the ready list
		  if (next!= NULL && thread_current()->priority < next->priority){thread_yield();}//If current priority is less than next scheduled, yield the thread.
	  }
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current() ->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  struct FIXED17_14 hundred = int_to_fixed17_14(100);
  struct FIXED17_14 result = multiply_fixed17_14(load_avg, hundred);
  return result.full/CONVERT	;									//No need to worry over sign here because load_avg should never be negative.
}

/*Fixed Point Math Opperations*/

struct FIXED17_14 int_to_fixed17_14(int integer){
		struct FIXED17_14 temp;
		int sign = 1;
		if(integer<0){			//If the integer is negative doing shift operations will cause problems with the sign bit
			sign *= -1;
		}
		temp.full = integer*CONVERT;
		temp.full *=sign;		//Return the original sign. Note sub properties integer and fraction are not signed.
		return temp;
}

struct FIXED17_14 multiply_fixed17_14(struct FIXED17_14 a, struct FIXED17_14 b){
	struct FIXED17_14 temp;
	int sign = 1;
	if(a.full<0){			//If the integer is negative doing shift operations will cause problems with the sign bit
		sign *= -1;
		a.full *= -1;		//Remove the sign to make shift operations easier to deal with
	}
	if(b.full<0){			//If the integer is negative doing shift operations will cause problems with the sign bit
		sign *= -1;
		b.full *= -1;		//Remove the sign to make shift operations easier to deal with
	}
	//Here both structs have their full value returned to being positive for the shifting to be done easier, and we know the sign of the result.
	
	temp.full = ((int64_t)a.full)*b.full/CONVERT;
	temp.full *= sign;							//Restore the proper sign value for the result.
	return temp;
	
}

struct FIXED17_14 divide_fixed17_14(struct FIXED17_14 a, struct FIXED17_14 b){
	struct FIXED17_14 temp;
	int sign = 1;
	if(a.full<0){			//If the integer is negative doing shift operations will cause problems with the sign bit
		sign *= -1;
		a.full *= -1;		//Remove the sign to make shift operations easier to deal with
	}
	if(b.full<0){			//If the integer is negative doing shift operations will cause problems with the sign bit
		sign *= -1;
		b.full *= -1;		//Remove the sign to make shift operations easier to deal with
	}
	//Here both structs have their full value returned to being positive for the shifting to be done easier, and we know the sign of the result.
	
	temp.full = ((int64_t)a.full)*CONVERT/b.full;
	temp.full *= sign;							//Restore the proper sign value for the result.
	return temp;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  struct thread *t = thread_current();
  return t->recent_cpu.full*100/CONVERT;			//integer does not have the sign of the result by definition, so return based on the sign of result.full; 
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->nice = 0;//default
  t->wakeup_time = 0;
  sema_init(&t->sema_wakeup, 0);
  t->recent_cpu = int_to_fixed17_14(0);			//Default to 0 when creating a new thread.
  if(!thread_mlfqs){
	t->priority = priority;
	t->original_priority = priority;
  }
  else{
	  struct FIXED17_14 pri;
	  pri.full = PRI_MAX*CONVERT - (t->recent_cpu.full/(4*CONVERT)) - 2*(t->nice)*CONVERT;
	  t->priority = pri.full/CONVERT;
	  if(t->priority<PRI_MIN){t->priority = PRI_MIN;}
	  else if(t->priority > PRI_MAX){t->priority = PRI_MAX;}		//Calculate the threads initial priority
  }
  t->magic = THREAD_MAGIC;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
