#include "spall_auto.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>


#define THREAD_QUEUE_CAP 16000
typedef ssize_t tpool_task_proc(void *data);
typedef struct TPoolTask {
	tpool_task_proc  *do_work;
	void             *args;
} TPoolTask;

typedef struct Thread {
	pthread_t thread;
	int idx;

	TPoolTask *queue;
	size_t capacity;
	_Atomic uint64_t head;
	_Atomic uint64_t tail;
	pthread_mutex_t queue_lock;

	struct TPool *pool;
} Thread;

typedef struct TPool {
	struct Thread *threads;

	int thread_count;
	_Atomic bool running;

	pthread_cond_t tasks_available;
	pthread_mutex_t task_lock;

	_Atomic uint64_t tasks_done;
	_Atomic uint64_t tasks_total;
} TPool;

_Thread_local Thread *current_thread = NULL;
_Thread_local int work_count = 0;

void mutex_init(pthread_mutex_t *mut) {
	pthread_mutex_init(mut, NULL);
}
void mutex_lock(pthread_mutex_t *mut) {
	pthread_mutex_lock(mut);
}
void mutex_unlock(pthread_mutex_t *mut) {
	pthread_mutex_unlock(mut);
}
int mutex_trylock(pthread_mutex_t *mut) {
	return pthread_mutex_trylock(mut);
}
void cond_init(pthread_cond_t *cond) {
	pthread_cond_init(cond, NULL);
}
void cond_broadcast(pthread_cond_t *cond) {
	pthread_cond_broadcast(cond);
}
void cond_signal(pthread_cond_t *cond) {
	pthread_cond_signal(cond);
}
int cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
	return pthread_cond_wait(cond, mutex);
}

void tqueue_push(Thread *thread, TPoolTask task) {
	if ((thread->head - thread->tail) >= thread->capacity) {
		printf("Task queue is too full!!\n");
		exit(1);
	}

	size_t idx = thread->head % thread->capacity;
	thread->queue[idx] = task;
	thread->head++;
	thread->pool->tasks_total++;

	cond_broadcast(&thread->pool->tasks_available);
}

void tqueue_push_safe(Thread *thread, TPoolTask task) {
	mutex_lock(&thread->queue_lock);
	tqueue_push(thread, task);
	mutex_unlock(&thread->queue_lock);
}

TPoolTask *tqueue_pop(Thread *thread) {
	if (thread->tail >= thread->head) {
		return NULL;
	}

	size_t idx = thread->tail % thread->capacity;
	TPoolTask *task = &thread->queue[idx];
	thread->tail++;
	return task;
}

TPoolTask *tqueue_pop_safe(Thread *thread) {
	mutex_lock(&thread->queue_lock);
	TPoolTask *task = tqueue_pop(thread);
	mutex_unlock(&thread->queue_lock);
	return task;
}

void thread_sleep(void) {
	sched_yield();
}

void *tpool_worker(void *ptr) {
	current_thread = (Thread *)ptr;
	TPool *pool = current_thread->pool;
	spall_auto_thread_init(current_thread->idx, SPALL_DEFAULT_BUFFER_SIZE, SPALL_DEFAULT_SYMBOL_CACHE_SIZE);

	for (;;) {
work_start:
		if (!pool->running) {
			break;
		}

		// If we've got tasks to process, work through them
		size_t finished_tasks = 0;
		while (current_thread->head > current_thread->tail) {
			TPoolTask *task = tqueue_pop_safe(current_thread);
			if (!task) {
				break;
			}

			task->do_work(task->args);
			pool->tasks_done++;
			finished_tasks++;
		}
		if (finished_tasks > 0 && pool->tasks_done == pool->tasks_total) {
			cond_broadcast(&pool->tasks_available);
		}

		// If there's still work somewhere and we don't have it, steal it
		if ((pool->tasks_done < pool->tasks_total) && (current_thread->head == current_thread->tail)) {
			int idx = current_thread->idx;
			for (int i = 0; i < pool->thread_count; i++) {
				if (pool->tasks_done == pool->tasks_total) {
					break;
				}

				idx = (idx + 1) % pool->thread_count;
				Thread *thread = &pool->threads[idx];

				if (thread->head > thread->tail) {
					int ret = mutex_trylock(&thread->queue_lock);
					if (ret) {
						continue;
					}

					TPoolTask *task = tqueue_pop(thread);
					mutex_unlock(&thread->queue_lock);
					if (!task) {
						continue;
					}

					task->do_work(task->args);
					pool->tasks_done++;

					if (pool->tasks_done == pool->tasks_total) {
						cond_broadcast(&pool->tasks_available);
					}
					goto work_start;
				}
			}
		}

		// if we've done all our work, and there's nothing to steal, go to sleep
		mutex_lock(&pool->task_lock);
		int ret = cond_wait(&pool->tasks_available, &pool->task_lock);
		if (!ret) {
			mutex_unlock(&pool->task_lock);
		}
	}

	spall_auto_thread_quit();
	return NULL;
}

void tpool_wait(TPool *pool) {
	while (pool->tasks_done < pool->tasks_total) {

		// if we've got tasks on our queue, run them
		while (current_thread->head > current_thread->tail) {
			TPoolTask *task = tqueue_pop_safe(current_thread);
			if (!task) {
				break;
			}

			task->do_work(task->args);
			pool->tasks_done++;
		}

		if (pool->tasks_done == pool->tasks_total) {
			break;
		}

		mutex_lock(&pool->task_lock);
		int ret = cond_wait(&pool->tasks_available, &pool->task_lock);
		if (!ret) {
			mutex_unlock(&pool->task_lock);
		}
	}
}

void thread_start(Thread *thread) {
	pthread_create(&thread->thread, NULL, tpool_worker, (void *)thread);
}
void thread_end(Thread thread) {
	pthread_join(thread.thread, NULL);
	free(thread.queue);
}

void thread_init(TPool *pool, Thread *thread, int idx) {
	mutex_init(&thread->queue_lock);
	thread->capacity = THREAD_QUEUE_CAP;
	thread->queue = calloc(sizeof(TPoolTask), thread->capacity);
	thread->head = 0;
	thread->tail = 0;
	thread->pool = pool;
	thread->idx = idx;
}

TPool *tpool_init(int child_thread_count) {
	TPool *pool = malloc(sizeof(TPool));

	int thread_count = child_thread_count + 1;

	pool->thread_count = thread_count;
	pool->threads = malloc(sizeof(Thread) * pool->thread_count);
	cond_init(&pool->tasks_available);
	mutex_init(&pool->task_lock);
	pool->running = true;

	// setup the main thread
	thread_init(pool, &pool->threads[0], 0);
	current_thread = &pool->threads[0];

	for (int i = 1; i < pool->thread_count; i++) {
		thread_init(pool, &pool->threads[i], i);
		thread_start(&pool->threads[i]);
	}

	return pool;
}

void tpool_destroy(TPool *pool) {
	pool->running = false;
	for (int i = 1; i < pool->thread_count; i++) {
		Thread *thread = &pool->threads[i];
		cond_broadcast(&pool->tasks_available);
		thread_end(pool->threads[i]);
	}

	free(pool->threads[0].queue);
	free(pool->threads);
	free(pool);
}

ssize_t little_work(void *args) {
	size_t count = (size_t)args;

	// this is my workload. enjoy
	int sleep_time = rand() % 201;
	usleep(sleep_time);

	if (current_thread->pool->tasks_total < 10000) {
		mutex_lock(&current_thread->queue_lock);
		for (int i = 0; i < 5; i++) {
			TPoolTask task;
			task.do_work = little_work;
			task.args = (void *)(uint64_t)(count);
			tqueue_push(current_thread, task);
		}
		mutex_unlock(&current_thread->queue_lock);
	}
	return 0;
}


int main(void) {
	srand(1);
	spall_auto_init("pool_test.spall");
	spall_auto_thread_init(0, SPALL_DEFAULT_BUFFER_SIZE, SPALL_DEFAULT_SYMBOL_CACHE_SIZE);

	TPool *pool = tpool_init(12);

	int initial_task_count = 10;

	mutex_lock(&current_thread->queue_lock);
	for (int i = 0; i < initial_task_count; i++) {
		TPoolTask task;
		task.do_work = little_work;
		task.args = (void *)(uint64_t)(i + 1);
		tqueue_push(current_thread, task);
	}
	mutex_unlock(&current_thread->queue_lock);

	tpool_wait(pool);
	usleep(500);

	// this is a dumb hack because of the way I do task growth.
	// not required to make this pool work
	pool->tasks_total = 0;
	pool->tasks_done  = 0;

	mutex_lock(&current_thread->queue_lock);
	for (int i = initial_task_count; i < (initial_task_count * 2); i++) {
		TPoolTask task;
		task.do_work = little_work;
		task.args = (void *)(uint64_t)(i + 1);
		tqueue_push(current_thread, task);
	}
	mutex_unlock(&current_thread->queue_lock);

	tpool_wait(pool);
	tpool_destroy(pool);

	spall_auto_thread_quit();
	spall_auto_quit();
}

#define SPALL_AUTO_IMPLEMENTATION
#define SPALL_BUFFER_PROFILING
#define SPALL_BUFFER_PROFILING_GET_TIME() __rdtsc()
#include "spall_auto.h"
