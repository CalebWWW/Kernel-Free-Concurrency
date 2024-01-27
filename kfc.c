#include <assert.h>
#include <sys/types.h>
#include <ucontext.h>
#include <stdlib.h>
#include <stdint.h>

#include "valgrind.h"
#include "queue.h"
#include "kfc.h"

static int inited = 0;

typedef struct {
  int complete;
  tid_t tid;
  ucontext_t *context;
  void *return_value; //Return value from exit
  void *(*func)(void *);
  void *arg;
} pcb;

static pcb *storage[1025];

static pcb *wait[1]; //Holds the process thats in the waiting state
int waiting; //The tid that that is being waited on

tid_t tid; //Will be n where n is the total number of user threads
pcb *current;
queue_t ready;
ucontext_t *finished_context; //Context will return to the wrapper function
ucontext_t *swap_context; //Context will return to fcfs algorithm

/**
 * Initializes the kfc library.  Programs are required to call this function
 * before they may use anything else in the library's public interface.
 *
 * @param kthreads    Number of kernel threads (pthreads) to allocate
 * @param quantum_us  Preemption timeslice in microseconds, or 0 for cooperative
 *                    scheduling
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_init(int kthreads, int quantum_us)
{
	assert(!inited);
	queue_init(&ready);
	tid = 1;
	waiting = -1;

	//initializes the context switch to the trampoline function
	finished_context = malloc(sizeof(*finished_context));
	getcontext(finished_context); 
	finished_context->uc_stack.ss_sp = malloc(KFC_DEF_STACK_SIZE);
	finished_context->uc_stack.ss_size = KFC_DEF_STACK_SIZE;
	makecontext(finished_context, (void(*)(void)) trap, 0);

	//initializes the context switch top fcfs
	swap_context = malloc(sizeof(*swap_context));
	getcontext(swap_context);
	swap_context->uc_stack.ss_sp = malloc(KFC_DEF_STACK_SIZE);
	swap_context->uc_stack.ss_size = KFC_DEF_STACK_SIZE;
	VALGRIND_STACK_REGISTER (swap_context->uc_stack.ss_sp, 
				 swap_context->uc_stack.ss_size
				 + swap_context->uc_stack.ss_sp);
	makecontext(swap_context, (void(*)(void)) fcfs, 0);

	//initializes the main thread in the static array
	storage[0] = malloc(sizeof(*storage[0]));
	storage[0]->tid = KFC_TID_MAIN; //KFC_TID_MAIN = 0

	storage[0]->context = malloc(sizeof(*storage[0]->context));
	getcontext(storage[0]->context);
	storage[0]->context->uc_stack.ss_sp = (caddr_t) malloc(KFC_DEF_STACK_SIZE);
	storage[0]->context->uc_stack.ss_size = KFC_DEF_STACK_SIZE;
	VALGRIND_STACK_REGISTER (storage[0]->context->uc_stack.ss_sp, 
				 storage[0]->context->uc_stack.ss_size
				 + storage[0]->context->uc_stack.ss_sp);
	storage[0]->complete = 0;
	storage[0]->return_value = NULL;

	current = storage[0];

	//initialized waiting queue
	wait[0] = malloc(sizeof(*wait[0]));
	wait[0]->tid = -1;
	wait[0]->complete = -1;

	inited = 1;
	return 0;
}

/**
 * Cleans up any resources which were allocated by kfc_init.  You may assume
 * that this function is called only from the main thread, that any other
 * threads have terminated and been joined, and that threading will not be
 * needed again.  (In other words, just clean up and don't worry about the
 * consequences.)
 *
 * I won't be testing this function specifically, but it is provided as a
 * convenience to you if you are using Valgrind to check your code, which I
 * always encourage.
 */
void
kfc_teardown(void)
{
	assert(inited);
	free(swap_context);
	free(finished_context);
	free(wait[0]);
	inited = 0;
}

/**
 * Creates a new user thread which executes the provided function concurrently.
 * It is left up to the implementation to decide whether the calling thread
 * continues to execute or the new thread takes over immediately.
 *
 * @param ptid[out]   Pointer to a tid_t variable in which to store the new
 *                    thread's ID
 * @param start_func  Thread main function
 * @param arg         Argument to be passed to the thread main function
 * @param stack_base  Location of the thread's stack if already allocated, or
 *                    NULL if requesting that the library allocate it
 *                    dynamically
 * @param stack_size  Size (in bytes) of the thread's stack, or 0 to use the
 *                    default thread stack size KFC_DEF_STACK_SIZE
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_create(tid_t *ptid, void *(*start_func)(void *), void *arg,
	   caddr_t stack_base, size_t stack_size)
{
  assert(inited);

  if (stack_base == NULL) { //initiating stack if there is no size defined
    if (stack_size > 0)
      stack_base = (caddr_t) malloc(stack_size); 
    else {
      stack_base = (caddr_t) malloc(KFC_DEF_STACK_SIZE);
      stack_size = KFC_DEF_STACK_SIZE;
    }
  }
  
  *ptid = tid;
  
  storage[tid] = malloc(sizeof(*storage[tid]));
  storage[tid]->context = malloc(sizeof(*storage[tid]->context));

  getcontext(storage[tid]->context);
  storage[tid]->context->uc_link = finished_context;
  storage[tid]->context->uc_stack.ss_sp = stack_base;
  storage[tid]->context->uc_stack.ss_size = stack_size;
  storage[tid]->complete = 0;
  storage[tid]->tid = *ptid;
  storage[tid]->return_value = NULL;
  storage[tid]->func = start_func;
  storage[tid]->arg = arg;

  makecontext(storage[*ptid]->context, (void(*)(void)) trap, 1, arg);

  queue_enqueue(&ready, storage[tid]);

  tid++;

  return 0;
}

/*
 * Trampoline function that will run immediatly
 */
void trap() /* uWu */ {
  if (current->complete != 1) {
    current->return_value = current->func(current->arg);
    current->complete = 1;
    kfc_exit(current->return_value);
  }
  fcfs();
}

/*
 * Runs the First Come First Serve algorithm
 */ 
void fcfs() {
  //Enqueues the context if it isn't completed
  if (current->complete == 0)
    queue_enqueue(&ready, current);

  if (queue_size(&ready) > 0) {
    current = queue_dequeue(&ready);
    //FCFS swapping
    setcontext(current->context);
  }
}


/**
 * Exits the calling thread.  This should be the same thing that happens when
 * the thread's start_func returns.
 *
 * @param ret  Return value from the thread
 */
void
kfc_exit(void *ret)
{
  assert(inited);
  current->complete = 1;
  current->return_value = ret;
  if (waiting == current->tid) {
    waiting = -1;
    current = wait[0];
    current->complete = 0;
    setcontext(current->context);
  }
  //returns back to fcfs algorithm
  setcontext(swap_context);
}

/**
 * Waits for the thread specified by tid to terminate, retrieving that threads
 * return value.  Returns immediately if the target thread has already
 * terminated, otherwise blocks.  Attempting to join a thread which already has
 * another thread waiting to join it, or attempting to join a thread which has
 * already been joined, results in undefined behavior.
 *
 * @param pret[out]  Pointer to a void * in which the thread's return value from
 *                   kfc_exit should be stored, or NULL if the caller does not
 *                   care.
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_join(tid_t tid, void **pret)
{
  assert(inited);
  if (!storage[tid]->complete) {
    current->complete = 1;
    wait[0] = current;
    waiting = tid;
    swapcontext(current->context, swap_context);
    *pret = storage[tid]->return_value;
    
    //Cleans up the allocated threads
    free(storage[tid]->context);
    free(storage[tid]->context->uc_stack.ss_sp);
    free(storage[tid]);
  } else { //The case where join is called on a thread that has already exited
    *pret = storage[tid]->return_value;
  }
  return 1;
}

/**
 * Returns a small integer which identifies the calling thread.
 *
 * @return Thread ID of the currently executing thread
 */
tid_t kfc_self(void)
{
  assert(inited);
  return current->tid;
}

/**
 * Causes the calling thread to yield the processor voluntarily.  This may
 * result in another thread being scheduled, but it does not preclude the
 * possibility of the same thread continuing if re-chosen by the scheduling
 * algorithm.
 */
void
kfc_yield(void)
{
  assert(inited);
  swapcontext(current->context, swap_context);
}

/**
 * Initializes a user-level counting semaphore with a specific value.
 *
 * @param sem    Pointer to the semaphore to be initialized
 * @param value  Initial value for the semaphore's counter
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_sem_init(kfc_sem_t *sem, int value)
{
	assert(inited);
	sem->val = value;
	queue_init(&sem->waiting_sem);
	return 0;
}

/**
 * Increments the value of the semaphore.  This operation is also known as
 * up, signal, release, and V (Dutch verhoog, "increase").
 *
 * @param sem  Pointer to the semaphore which the thread is releasing
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_sem_post(kfc_sem_t *sem)
{
	assert(inited);
	sem->val++;
	if (sem->val <= 0) {
	  queue_enqueue(&sem->waiting_sem, queue_dequeue(&sem->waiting_sem));
	}
	return 0;
}

/**
 * Attempts to decrement the value of the semaphore.  This operation is also
 * known as down, acquire, and P (Dutch probeer, "try").  This operation should
 * block when the counter is not above 0.
 *
 * @param sem  Pointer to the semaphore which the thread wishes to acquire
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_sem_wait(kfc_sem_t *sem)
{
  assert(inited);
  sem->val--;
  if (sem->val < 0) {
    queue_enqueue(&sem->waiting_sem, current);
    swapcontext(current->context, swap_context);
    return 0;
  }
  return 1;
}

/**
 * Frees any resources associated with a semaphore.  Destroying a semaphore on
 * which threads are waiting results in undefined behavior.
 *
 * @param sem  Pointer to the semaphore to be destroyed
 */
void
kfc_sem_destroy(kfc_sem_t *sem)
{
	assert(inited);
	//free(sem);
}
