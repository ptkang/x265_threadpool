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

#include "common.h"
#include "threadpool.h"
#include "threading.h"

#include <new>

/* use 32-bit primitives defined in threading.h */
#define SLEEPBITMAP_CTZ CTZ
#define SLEEPBITMAP_OR  ATOMIC_OR
#define SLEEPBITMAP_AND ATOMIC_AND

namespace X265_NS {
// x265 private namespace

class WorkerThread : public Thread
{
private:

    ThreadPool&  m_pool;
    int          m_id;
    Event        m_wakeEvent;

    WorkerThread& operator =(const WorkerThread&);

public:

    JobProvider*     m_curJobProvider;
    BondedTaskGroup* m_bondMaster;

    WorkerThread(ThreadPool& pool, int id) : m_pool(pool), m_id(id) {}
    virtual ~WorkerThread() {}

    void threadMain();
    void awaken()           { m_wakeEvent.trigger(); }
};

void WorkerThread::threadMain()
{
    THREAD_NAME("Worker", m_id);

    __attribute__((unused)) int val = nice(10);

    m_pool.setCurrentThreadAffinity();

    sleepbitmap_t idBit = (sleepbitmap_t)1 << m_id;
    m_curJobProvider = m_pool.m_jpTable[0];
    m_bondMaster = NULL;

    SLEEPBITMAP_OR(&m_curJobProvider->m_ownerBitmap, idBit);
    SLEEPBITMAP_OR(&m_pool.m_sleepBitmap, idBit);
    m_wakeEvent.wait();

    while (m_pool.m_isActive)
    {
        if (m_bondMaster)
        {
            m_bondMaster->processTasks(m_id);
            m_bondMaster->m_exitedPeerCount.incr();
            m_bondMaster = NULL;
        }

        do
        {
            /* do pending work for current job provider */
            m_curJobProvider->findJob(m_id);

            /* if the current job provider still wants help, only switch to a
             * higher priority provider (lower slice type). Else take the first
             * available job provider with the highest priority */
            int curPriority = (m_curJobProvider->m_helpWanted) ? m_curJobProvider->m_sliceType :
                                                                 INVALID_SLICE_PRIORITY + 1;
            int nextProvider = -1;
            for (int i = 0; i < m_pool.m_numProviders; i++)
            {
                if (m_pool.m_jpTable[i]->m_helpWanted &&
                    m_pool.m_jpTable[i]->m_sliceType < curPriority)
                {
                    nextProvider = i;
                    curPriority = m_pool.m_jpTable[i]->m_sliceType;
                }
            }
            if (nextProvider != -1 && m_curJobProvider != m_pool.m_jpTable[nextProvider])
            {
                SLEEPBITMAP_AND(&m_curJobProvider->m_ownerBitmap, ~idBit);
                m_curJobProvider = m_pool.m_jpTable[nextProvider];
                SLEEPBITMAP_OR(&m_curJobProvider->m_ownerBitmap, idBit);
            }
        }
        while (m_curJobProvider->m_helpWanted);

        /* While the worker sleeps, a job-provider or bond-group may acquire this
         * worker's sleep bitmap bit. Once acquired, that thread may modify 
         * m_bondMaster or m_curJobProvider, then waken the thread */
        SLEEPBITMAP_OR(&m_pool.m_sleepBitmap, idBit);
        m_wakeEvent.wait();
    }

    SLEEPBITMAP_OR(&m_pool.m_sleepBitmap, idBit);
}

void JobProvider::tryWakeOne()
{
    int id = m_pool->tryAcquireSleepingThread(m_ownerBitmap, ALL_POOL_THREADS);
    if (id < 0)
    {
        m_helpWanted = true;
        return;
    }

    WorkerThread& worker = m_pool->m_workers[id];
    if (worker.m_curJobProvider != this) /* poaching */
    {
        sleepbitmap_t bit = (sleepbitmap_t)1 << id;
        SLEEPBITMAP_AND(&worker.m_curJobProvider->m_ownerBitmap, ~bit);
        worker.m_curJobProvider = this;
        SLEEPBITMAP_OR(&worker.m_curJobProvider->m_ownerBitmap, bit);
    }
    worker.awaken();
}

int ThreadPool::tryAcquireSleepingThread(sleepbitmap_t firstTryBitmap, sleepbitmap_t secondTryBitmap)
{
    unsigned long id;

    sleepbitmap_t masked = m_sleepBitmap & firstTryBitmap;
    while (masked)
    {
        SLEEPBITMAP_CTZ(id, masked);

        sleepbitmap_t bit = (sleepbitmap_t)1 << id;
        if (SLEEPBITMAP_AND(&m_sleepBitmap, ~bit) & bit)
            return (int)id;

        masked = m_sleepBitmap & firstTryBitmap;
    }

    masked = m_sleepBitmap & secondTryBitmap;
    while (masked)
    {
        SLEEPBITMAP_CTZ(id, masked);

        sleepbitmap_t bit = (sleepbitmap_t)1 << id;
        if (SLEEPBITMAP_AND(&m_sleepBitmap, ~bit) & bit)
            return (int)id;

        masked = m_sleepBitmap & secondTryBitmap;
    }

    return -1;
}

int ThreadPool::tryBondPeers(int maxPeers, sleepbitmap_t peerBitmap, BondedTaskGroup& master)
{
    int bondCount = 0;
    do
    {
        int id = tryAcquireSleepingThread(peerBitmap, 0);
        if (id < 0)
            return bondCount;

        m_workers[id].m_bondMaster = &master;
        m_workers[id].awaken();
        bondCount++;
    }
    while (bondCount < maxPeers);

    return bondCount;
}

ThreadPool* ThreadPool::allocThreadPools(x265_param* p, int& numPools, bool isThreadsReserved)
{
    enum { MAX_NODE_NUM = 127 };
    int cpusPerNode[MAX_NODE_NUM + 1];
    int threadsPerPool[MAX_NODE_NUM + 2];
    uint64_t nodeMaskPerPool[MAX_NODE_NUM + 2];

    memset(cpusPerNode, 0, sizeof(cpusPerNode));
    memset(threadsPerPool, 0, sizeof(threadsPerPool));
    memset(nodeMaskPerPool, 0, sizeof(nodeMaskPerPool));

    int numNumaNodes = X265_MIN(getNumaNodeCount(), MAX_NODE_NUM);

    cpusPerNode[0] = getCpuCount();

    for (int i = 0; i < numNumaNodes; i++)
    {
        threadsPerPool[numNumaNodes]  += cpusPerNode[i];
        nodeMaskPerPool[numNumaNodes] |= ((uint64_t)1 << i);
    }
 
    // If the last pool size is > MAX_POOL_THREADS, clip it to spawn thread pools only of size >= 1/2 max (heuristic)
    if ((threadsPerPool[numNumaNodes] > MAX_POOL_THREADS) &&
        ((threadsPerPool[numNumaNodes] % MAX_POOL_THREADS) < (MAX_POOL_THREADS / 2)))
    {
        threadsPerPool[numNumaNodes] -= (threadsPerPool[numNumaNodes] % MAX_POOL_THREADS);
        x265_log(p, X265_LOG_DEBUG,
                 "Creating only %d worker threads beyond specified numbers with --pools (if specified) to prevent asymmetry in pools; may not use all HW contexts\n", threadsPerPool[numNumaNodes]);
    }

    numPools = 0;
    for (int i = 0; i < numNumaNodes + 1; i++)
    {
        if (threadsPerPool[i])
            numPools += (threadsPerPool[i] + MAX_POOL_THREADS - 1) / MAX_POOL_THREADS;
    }

    if (!numPools)
        return NULL;

    if (numPools > p->frameNumThreads)
    {
        numPools = X265_MAX(p->frameNumThreads / 2, 1);
    }
    if (isThreadsReserved)
        numPools = 1;
    ThreadPool *pools = new ThreadPool[numPools];
    if (pools)
    {
        int maxProviders = (p->frameNumThreads + numPools - 1) / numPools + !isThreadsReserved; /* +1 is Lookahead, always assigned to threadpool 0 */
        int node = 0;
        for (int i = 0; i < numPools; i++)
        {
            while (!threadsPerPool[node])
                node++;
            int numThreads = X265_MIN(MAX_POOL_THREADS, threadsPerPool[node]);
            int origNumThreads = numThreads;
            if (p->lookaheadThreads > numThreads / 2)
            {
                p->lookaheadThreads = numThreads / 2;
                x265_log(p, X265_LOG_DEBUG, "Setting lookahead threads to a maximum of half the total number of threads\n");
            }
            if (isThreadsReserved)
            {
                numThreads = p->lookaheadThreads;
                maxProviders = 1;
            }

            else
                numThreads -= p->lookaheadThreads;
            if (!pools[i].create(numThreads, maxProviders, nodeMaskPerPool[node]))
            {
                X265_FREE(pools);
                numPools = 0;
                return NULL;
            }
            if (numNumaNodes > 1)
            {
                char *nodesstr = new char[64 * strlen(",63") + 1];
                int len = 0;
                for (int j = 0; j < 64; j++)
                    if ((nodeMaskPerPool[node] >> j) & 1)
                        len += sprintf(nodesstr + len, ",%d", j);
                x265_log(p, X265_LOG_INFO, "Thread pool %d using %d threads on numa nodes %s\n", i, numThreads, nodesstr + 1);
            }
            else
                x265_log(p, X265_LOG_INFO, "Thread pool created using %d threads\n", numThreads);
            threadsPerPool[node] -= origNumThreads;
        }
    }
    else
        numPools = 0;
    return pools;
}

ThreadPool::ThreadPool()
{
    memset(this, 0, sizeof(*this));
}

bool ThreadPool::create(int numThreads, int maxProviders, uint64_t nodeMask)
{
    X265_CHECK(numThreads <= MAX_POOL_THREADS, "a single thread pool cannot have more than MAX_POOL_THREADS threads\n");

    m_numWorkers = numThreads;

    m_workers = X265_MALLOC(WorkerThread, numThreads);
    /* placement new initialization */
    if (m_workers)
        for (int i = 0; i < numThreads; i++)
            new (m_workers + i)WorkerThread(*this, i);

    m_jpTable = X265_MALLOC(JobProvider*, maxProviders);
    m_numProviders = 0;

    return m_workers && m_jpTable;
}

bool ThreadPool::start()
{
    m_isActive = true;
    for (int i = 0; i < m_numWorkers; i++)
    {
        if (!m_workers[i].start())
        {
            m_isActive = false;
            return false;
        }
    }
    return true;
}

void ThreadPool::stopWorkers()
{
    if (m_workers)
    {
        m_isActive = false;
        for (int i = 0; i < m_numWorkers; i++)
        {
            while (!(m_sleepBitmap & ((sleepbitmap_t)1 << i)))
                GIVE_UP_TIME();
            m_workers[i].awaken();
            m_workers[i].stop();
        }
    }
}

ThreadPool::~ThreadPool()
{
    if (m_workers)
    {
        for (int i = 0; i < m_numWorkers; i++)
            m_workers[i].~WorkerThread();
    }

    X265_FREE(m_workers);
    X265_FREE(m_jpTable);

}

void ThreadPool::setCurrentThreadAffinity()
{
    setThreadNodeAffinity(m_numaMask);
}

void ThreadPool::setThreadNodeAffinity(void *numaMask)
{
    return;
}

/* static */
int ThreadPool::getNumaNodeCount()
{
    return 1;
}

/* static */
int ThreadPool::getCpuCount()
{
    return sysconf(_SC_NPROCESSORS_ONLN);
}

} // end namespace X265_NS
