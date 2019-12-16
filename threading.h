/*****************************************************************************
 * Copyright (C) 2013-2017 MulticoreWare, Inc
 *
 * Authors: Steve Borho <steve@borho.org>
 *          Min Chen <chenm003@163.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at license @ x265.com
 *****************************************************************************/

#ifndef X265_THREADING_H
#define X265_THREADING_H

#include "common.h"
#include "x265.h"

#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <fcntl.h>

#if MACOS
#include <sys/param.h>
#include <sys/sysctl.h>
#endif

#include <sys/time.h>
#include <unistd.h>

namespace X265_NS {
// x265 private namespace
int no_atomic_or(int* ptr, int mask);
int no_atomic_and(int* ptr, int mask);
int no_atomic_inc(int* ptr);
int no_atomic_dec(int* ptr);
int no_atomic_add(int* ptr, int val);
}

#define CLZ(id, x)            id = (unsigned long)__builtin_clz(x) ^ 31
#define CTZ(id, x)            id = (unsigned long)__builtin_ctz(x)
#define ATOMIC_OR(ptr, mask)  no_atomic_or((int*)ptr, mask)
#define ATOMIC_AND(ptr, mask) no_atomic_and((int*)ptr, mask)
#define ATOMIC_INC(ptr)       no_atomic_inc((int*)ptr)
#define ATOMIC_DEC(ptr)       no_atomic_dec((int*)ptr)
#define ATOMIC_ADD(ptr, val)  no_atomic_add((int*)ptr, val)
#define GIVE_UP_TIME()        usleep(0)

namespace X265_NS {
// x265 private namespace

typedef pthread_t ThreadHandle;

class Lock
{
public:

    Lock()
    {
        pthread_mutex_init(&this->handle, NULL);
    }

    ~Lock()
    {
        pthread_mutex_destroy(&this->handle);
    }

    void acquire()
    {
        pthread_mutex_lock(&this->handle);
    }

    void release()
    {
        pthread_mutex_unlock(&this->handle);
    }

protected:

    pthread_mutex_t handle;
};

class Event
{
public:

    Event()
    {
        m_counter = 0;
        if (pthread_mutex_init(&m_mutex, NULL) ||
            pthread_cond_init(&m_cond, NULL))
        {
            x265_log(NULL, X265_LOG_ERROR, "fatal: unable to initialize conditional variable\n");
        }
    }

    ~Event()
    {
        pthread_cond_destroy(&m_cond);
        pthread_mutex_destroy(&m_mutex);
    }

    void wait()
    {
        pthread_mutex_lock(&m_mutex);

        /* blocking wait on conditional variable, mutex is atomically released
         * while blocked. When condition is signaled, mutex is re-acquired */
        while (!m_counter)
            pthread_cond_wait(&m_cond, &m_mutex);

        m_counter--;
        pthread_mutex_unlock(&m_mutex);
    }

    bool timedWait(uint32_t waitms)
    {
        bool bTimedOut = false;

        pthread_mutex_lock(&m_mutex);
        if (!m_counter)
        {
            struct timeval tv;
            struct timespec ts;
            gettimeofday(&tv, NULL);
            /* convert current time from (sec, usec) to (sec, nsec) */
            ts.tv_sec = tv.tv_sec;
            ts.tv_nsec = tv.tv_usec * 1000;

            ts.tv_nsec += 1000 * 1000 * (waitms % 1000);    /* add ms to tv_nsec */
            ts.tv_sec += ts.tv_nsec / (1000 * 1000 * 1000); /* overflow tv_nsec */
            ts.tv_nsec %= (1000 * 1000 * 1000);             /* clamp tv_nsec */
            ts.tv_sec += waitms / 1000;                     /* add seconds */

            /* blocking wait on conditional variable, mutex is atomically released
             * while blocked. When condition is signaled, mutex is re-acquired.
             * ts is absolute time to stop waiting */
            bTimedOut = pthread_cond_timedwait(&m_cond, &m_mutex, &ts) == ETIMEDOUT;
        }
        if (m_counter > 0)
        {
            m_counter--;
            bTimedOut = false;
        }
        pthread_mutex_unlock(&m_mutex);
        return bTimedOut;
    }

    void trigger()
    {
        pthread_mutex_lock(&m_mutex);
        if (m_counter < UINT_MAX)
            m_counter++;
        /* Signal a single blocking thread */
        pthread_cond_signal(&m_cond);
        pthread_mutex_unlock(&m_mutex);
    }

protected:

    pthread_mutex_t m_mutex;
    pthread_cond_t  m_cond;
    uint32_t        m_counter;
};

/* This class is intended for use in signaling state changes safely between CPU
 * cores. One thread should be a writer and multiple threads may be readers. The
 * mutex's main purpose is to serve as a memory fence to ensure writes made by
 * the writer thread are visible prior to readers seeing the m_val change. Its
 * secondary purpose is for use with the condition variable for blocking waits */
class ThreadSafeInteger
{
public:

    ThreadSafeInteger()
    {
        m_val = 0;
        if (pthread_mutex_init(&m_mutex, NULL) ||
            pthread_cond_init(&m_cond, NULL))
        {
            x265_log(NULL, X265_LOG_ERROR, "fatal: unable to initialize conditional variable\n");
        }
    }

    ~ThreadSafeInteger()
    {
        pthread_cond_destroy(&m_cond);
        pthread_mutex_destroy(&m_mutex);
    }

    int waitForChange(int prev)
    {
        pthread_mutex_lock(&m_mutex);
        if (m_val == prev)
            pthread_cond_wait(&m_cond, &m_mutex);
        pthread_mutex_unlock(&m_mutex);
        return m_val;
    }

    int get()
    {
        pthread_mutex_lock(&m_mutex);
        int ret = m_val;
        pthread_mutex_unlock(&m_mutex);
        return ret;
    }

    int getIncr(int n = 1)
    {
        pthread_mutex_lock(&m_mutex);
        int ret = m_val;
        m_val += n;
        pthread_mutex_unlock(&m_mutex);
        return ret;
    }

    void set(int newval)
    {
        pthread_mutex_lock(&m_mutex);
        m_val = newval;
        pthread_cond_broadcast(&m_cond);
        pthread_mutex_unlock(&m_mutex);
    }

    void poke(void)
    {
        /* awaken all waiting threads, but make no change */
        pthread_mutex_lock(&m_mutex);
        pthread_cond_broadcast(&m_cond);
        pthread_mutex_unlock(&m_mutex);
    }

    void incr()
    {
        pthread_mutex_lock(&m_mutex);
        m_val++;
        pthread_cond_broadcast(&m_cond);
        pthread_mutex_unlock(&m_mutex);
    }

protected:

    pthread_mutex_t m_mutex;
    pthread_cond_t  m_cond;
    int             m_val;
};

class ScopedLock
{
public:

    ScopedLock(Lock &instance) : inst(instance)
    {
        this->inst.acquire();
    }

    ~ScopedLock()
    {
        this->inst.release();
    }

protected:

    // do not allow assignments
    ScopedLock &operator =(const ScopedLock &);

    Lock &inst;
};

// Utility class which adds elapsed time of the scope of the object into the
// accumulator provided to the constructor
struct ScopedElapsedTime
{
    ScopedElapsedTime(int64_t& accum) : accumlatedTime(accum) { startTime = x265_mdate(); }

    ~ScopedElapsedTime() { accumlatedTime += x265_mdate() - startTime; }

protected:

    int64_t  startTime;
    int64_t& accumlatedTime;

    // do not allow assignments
    ScopedElapsedTime &operator =(const ScopedElapsedTime &);
};

//< Simplistic portable thread class.  Shutdown signalling left to derived class
class Thread
{
private:

    ThreadHandle thread;

public:

    Thread();

    virtual ~Thread();

    //< Derived class must implement ThreadMain.
    virtual void threadMain() = 0;

    //< Returns true if thread was successfully created
    bool start();

    void stop();
};
} // end namespace X265_NS

#endif // ifndef X265_THREADING_H
