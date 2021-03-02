#include "threadpool.h"
#include <stdio.h>
#include <stdlib.h>

#define MEMORY_FAILED 1
#define MUTEX_INIT_FAILED 2
#define COND_INIT_FAILED 3
#define THREAD_CREATE_FAILED 4

void err(int err_type, void *threadpool, void *threads)
{
    if (threadpool)
        free(threadpool);
    if (threads)
        free(threads);

    switch (err_type)
    {
    case MEMORY_FAILED:
        perror("ERROR: MEMORY_ALOC_FAILED");
        break;
    case MUTEX_INIT_FAILED:
        perror("ERROR: MUTEX_INIT_FAILED");
        break;
    case COND_INIT_FAILED:
        perror("ERROR: COND_INIT_FAILED");
        break;
    case THREAD_CREATE_FAILED:
        perror("ERROR: THREAD_CREATE_FAILED");
        break;
    }
}

threadpool *create_threadpool(int num_threads_in_pool)
{
    if (num_threads_in_pool < 1 || num_threads_in_pool > MAXT_IN_POOL)
        return NULL;
    threadpool *t = (threadpool *)calloc(1, sizeof(threadpool));
    if (!t)
    {
        err(MEMORY_FAILED, NULL, NULL);
        return NULL;
    }
    // threadpool arguments init
    t->num_threads = num_threads_in_pool;
    t->threads = (pthread_t *)calloc(num_threads_in_pool, sizeof(pthread_t));
    if (!t->threads)
    {
        err(MEMORY_FAILED, t, NULL);
        return NULL;
    }
    // start with empty queue
    t->qhead = NULL;
    t->qtail = NULL;
    t->qsize = 0;
    // threadpool flags init
    t->shutdown = 0;
    t->dont_accept = 0;
    // threadpool mutex and cond's init
    if (pthread_mutex_init(&(t->qlock), NULL))
    {
        err(MUTEX_INIT_FAILED, t, t->threads);
        return NULL;
    }
    if (pthread_cond_init(&(t->q_empty), NULL))
    {
        err(COND_INIT_FAILED, t, t->threads);
        pthread_mutex_destroy(&(t->qlock));
        return NULL;
    }
    if (pthread_cond_init(&(t->q_not_empty), NULL))
    {
        err(COND_INIT_FAILED, t, t->threads);
        pthread_mutex_destroy(&(t->qlock));
        pthread_cond_destroy(&(t->q_empty));
        return NULL;
    }
    // create requested number of threads in threadpool
    for (int i = 0; i < num_threads_in_pool; i++)
    {
        if (pthread_create(&(t->threads[i]), NULL, do_work, t))
        {
            err(THREAD_CREATE_FAILED, t, t->threads);
            pthread_mutex_destroy(&(t->qlock));
            pthread_cond_destroy(&(t->q_empty));
            pthread_cond_destroy(&(t->q_not_empty));
            return NULL;
        }
    }
    return t;
}

void dispatch(threadpool *from_me, dispatch_fn dispatch_to_here, void *arg)
{

    // lock the threadpool to insert new job safely
    pthread_mutex_lock(&(from_me->qlock));
    // check if shutdown flag is up
    if (from_me->dont_accept)
        return;
    // if (!dispatch_to_here)
    //     return;
    work_t *work = (work_t *)calloc(1, sizeof(work_t));
    if (!work)
    {
        err(MEMORY_FAILED, NULL, NULL);
        return;
    }
    // init work args
    work->arg = arg;
    work->next = NULL;
    work->routine = dispatch_to_here;

    // insert work into queue
    if (!from_me->qhead)
    {
        from_me->qhead = work;
        from_me->qtail = work;
    }
    else
    {
        from_me->qtail->next = work;
        from_me->qtail = work;
    }
    // updating works queue size
    from_me->qsize++;
    // unlock threadpool for other thread
    pthread_mutex_unlock(&(from_me->qlock));
    // signal to sleeping thread to do_work
    pthread_cond_signal(&(from_me->q_not_empty));
}

void destroy_threadpool(threadpool *destroyme)
{
    // lock the threadpool to other
    pthread_mutex_lock(&(destroyme->qlock));
    // set dont accept flag up
    destroyme->dont_accept = 1;
    // there other jobs to do -> wait
    if (destroyme->qsize)
        pthread_cond_wait(&(destroyme->q_empty), &(destroyme->qlock));
    // the queue jobs is empty, set shut down flag up
    destroyme->shutdown = 1;
    // wake up sleeping threads to finish
    pthread_cond_broadcast(&destroyme->q_not_empty);
    // unlock threadpool for threads
    pthread_mutex_unlock(&(destroyme->qlock));
    //
    // pthread_cond_broadcast(&destroyme->q_empty);
    // clear the threads by using pthread_join
    for (int i = 0; i < destroyme->num_threads; i++)
        pthread_join(destroyme->threads[i], NULL);
    // free table args from memory
    pthread_cond_destroy(&(destroyme->q_empty));
    pthread_cond_destroy(&(destroyme->q_not_empty));
    pthread_mutex_destroy(&(destroyme->qlock));
    free(destroyme->threads);
    free(destroyme);
}

void *do_work(void *p)
{
    threadpool *t = (threadpool *)p;
    while (1)
    {
        pthread_mutex_lock(&(t->qlock));
        // if shutdown flag is up, dont accepet new work -> unlock mutex and kill thread
        if (t->shutdown)
        {
            pthread_mutex_unlock(&(t->qlock));
            return NULL;
        }

        // if threse no jobs to so -> go to sleep
        while (t->qsize == 0)
        {
            pthread_cond_wait(&(t->q_not_empty), &(t->qlock));
            // if threadpool shut down flag is up leave job and finish thread work
            if (t->shutdown)
            {
                pthread_mutex_unlock(&(t->qlock));
                return NULL;
            }
        }
        work_t *cur_work = t->qhead;
        t->qsize--;
        if (t->qhead->next)
        {

            t->qhead = t->qhead->next;
        }
        else
        {
            // no other jobs in queue -> set head and tail to be NULL
            t->qhead = NULL;
            t->qtail = NULL;
            // if threadpool dont_accept is up
            if (t->dont_accept)
                pthread_cond_signal(&(t->q_empty));
        }
        pthread_mutex_unlock(&(t->qlock));
        cur_work->routine(cur_work->arg);
        free(cur_work);
    }
}
