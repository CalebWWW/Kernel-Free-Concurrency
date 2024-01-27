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
  tid_t ctid;
  ucontext_t *context;
} pcb;

static pcb storage[1025];

tid_t *tid; //Will be n where n is the total number of user threads
tid_t *current_tid;
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
	printf("starting the program\n");
	queue_init(&ready);
        tid = malloc(sizeof(*tid));
	*tid = 1;
	current_tid = malloc(sizeof(*current_tid));
	*current_tid = 0;
	
	finished_context = malloc(sizeof(*finished_context));
	getcontext(finished_context); //Initializes the context switch top fcfs
	finished_context->uc_stack.ss_sp = malloc(KFC_DEF_STACK_SIZE);
	finished_context->uc_stack.ss_size = KFC_DEF_STACK_SIZE;
	makecontext(finished_context, (void(*)(void)) trap, 0);

	swap_context = malloc(sizeof(*swap_context));
	getcontext(swap_context); //Initializes the context switch top fcfs
	swap_context->uc_stack.ss_sp = malloc(KFC_DEF_STACK_SIZE);
	swap_context->uc_stack.ss_size = KFC_DEF_STACK_SIZE;
	VALGRIND_STACK_REGISTER (swap_context->uc_stack.ss_sp, swap_context->uc_stack.ss_size
				 + swap_context->uc_stack.ss_sp);
	makecontext(swap_context, (void(*)(void)) fcfs, 0);

	//initializes the main thread in the static array
	storage[0].ctid = KFC_TID_MAIN; //KFC_TID_MAIN = 0

	storage[0].context = malloc(sizeof(*storage[0].context));
	getcontext(storage[0].context);
	storage[0].context->uc_stack.ss_sp = (caddr_t) malloc(KFC_DEF_STACK_SIZE);
	storage[0].context->uc_stack.ss_size = KFC_DEF_STACK_SIZE;
	VALGRIND_STACK_REGISTER (storage[0].context->uc_stack.ss_sp, storage[0].context->uc_stack.ss_size
				 + storage[0].context->uc_stack.ss_sp);
	storage[0].complete = 0;

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

  printf("\nEntering kfc_create \n");

  assert(inited);

  //only enqueues the ptid, and uses it to search the context
  //queue_enqueue(&ready, storage[*current_tid].ctid);
  //printf("enqueued ptid = %d\n", storage[*current_tid].ctid);

  current_tid = malloc(sizeof(*current_tid));

  if (stack_base == NULL) { //initiating stack if there is no size defined
    if (stack_size > 0)
      stack_base = (caddr_t) malloc(stack_size); 
    else {
      stack_base = (caddr_t) malloc(KFC_DEF_STACK_SIZE);
      stack_size = KFC_DEF_STACK_SIZE;
    }
  }
  
  ptid = malloc(sizeof(*ptid));
  *ptid = *tid;

  storage[*ptid].context = malloc(sizeof(*storage[*ptid].context));

  getcontext(storage[*ptid].context);
  storage[*ptid].context->uc_link = finished_context;
  storage[*ptid].context->uc_stack.ss_sp = stack_base;
  storage[*ptid].context->uc_stack.ss_size = stack_size;
  storage[*ptid].complete = 0;
  storage[*ptid].ctid = *ptid;

  makecontext(storage[*ptid].context, (void(*)(void)) start_func, 1, arg);

  printf("context 0: %p \n",storage[0].context);

  (*tid)++;
  *current_tid = *ptid;

  printf("current_tid: %d ptid: %p \n",*current_tid, ptid);

  //printf("swapping %d and swap_context\n\n", old);
  //swapcontext(storage[old].context, swap_context);
  return 0;
}

/*
 * Catches the thread if it is finished
 */
void trap() {
  printf("finished tid %d\n\n", *current_tid);
  storage[*current_tid].complete = 1;
  fcfs();
}

/*
 * Runs the First Come First Serve algorithm
 */ 
void fcfs() {

  printf("Entering kfc_fcfs \n");


  //Enqueues the context if it isn't completed
  if (storage[*current_tid].complete != 1) {
    queue_enqueue(&ready, storage[*current_tid].ctid);
    printf("enqueued ptid = %d\n", storage[*current_tid].ctid);
  }

  printf("beginning queue size = %d \n", queue_size(&ready));

  if (queue_size(&ready) > 0) {

    tid_t *conv = (tid_t*) queue_peek(&ready);
    DPRINTF("conv = %d\n", *conv);

    //if the next thread has already been completed
    /**
    if (storage[*conv].complete == 1) {
      current_tid = queue_dequeue(&ready);
      printf("removing tid %d from the queue \n\n", *current_tid);
      setcontext(swap_context);
    }
    **/
    //FCFS swapping
    if (queue_peek(&ready) != NULL) {
      current_tid = queue_dequeue(&ready);
      printf("dequeued, current_tid = %d, %p\n", *current_tid, current_tid);
      printf("context 0: %p \n",storage[0].context);
      assert(storage[*current_tid].context);
      setcontext(storage[*current_tid].context);
    }
  }
  printf("exiting fcfs, queue size = %d \n\n", queue_size(&ready));
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
  storage[*current_tid].complete = 1;
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

	return 0;
}

/**
 * Returns a small integer which identifies the calling thread.
 *
 * @return Thread ID of the currently executing thread
 */
tid_t
kfc_self(void)
{
	assert(inited);
	printf("self tid: %d\n", *current_tid);
	return *current_tid;
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
	
	if (queue_peek(&ready) != NULL) {
          //queue_enqueue(&ready, current_tid);
	  printf("yielding current %d\n\n", *current_tid);
          swapcontext(storage[*current_tid].context, swap_context);
	}
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
	return 0;
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
}
