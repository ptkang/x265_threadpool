#include "threadpool.h"

namespace X265_NS {
// x265 private namespace
class ThreadMainJp : public JobProvider
{
public:
    int         m_numThreadPools;
    ThreadPool  *m_threadPool;
    x265_param  m_param;

    ThreadMainJp(int numThreadPools = 0);
    ~ThreadMainJp();
    void create();
    void stopJobs();
    void findJob(int workerThreadId);
};

ThreadMainJp::ThreadMainJp(int numThreadPools) : m_numThreadPools(numThreadPools)
{
    memset(&m_param, 0, sizeof(x265_param));
    m_param.logLevel = X265_LOG_DEBUG;
    m_param.numaPools = NULL;
    m_param.frameNumThreads = 8;
    m_param.lookaheadThreads = 0;
}

void ThreadMainJp::create()
{
    m_threadPool = ThreadPool::allocThreadPools(&m_param, m_numThreadPools, 0);
    if (m_numThreadPools) {
        m_pool = m_threadPool;
        m_jpId = m_threadPool[0].m_numProviders++;
        m_threadPool[0].m_jpTable[m_jpId] = this;
        for (int i = 0; i < m_numThreadPools; i++)
        {
            m_threadPool[i].start();
        }
    }
}
void ThreadMainJp::stopJobs()
{
    if (m_numThreadPools) {
        for (int i = 0; i < m_numThreadPools; i++)
        {
            m_threadPool[i].stopWorkers();
        }
    }
}

ThreadMainJp::~ThreadMainJp()
{
    m_pool = NULL;
    m_jpId = m_threadPool[0].m_numProviders--;
    m_threadPool[0].m_jpTable[m_jpId] = NULL;
    delete [] m_threadPool;
}

void ThreadMainJp::findJob(int workerThreadId)
{
    printf("%s(%d):workerThreadId=%d\n", __func__, __LINE__, workerThreadId);
}


class ThreadMainBg : public BondedTaskGroup
{
public:
    ThreadMainJp    &m_threadMainJp;

    ThreadMainBg(ThreadMainJp &tjp): m_threadMainJp(tjp) {};

    void processTasks(int workerThreadId);
};

void ThreadMainBg::processTasks(int workerThreadId)
{
    printf("%s(%d):workerThreadId=%d\n", __func__, __LINE__, workerThreadId);
}
}

using namespace X265_NS;

int main(int argc, char *argv[])
{
    int bondcnt = 0;
    ThreadMainJp threadMainJp;
    threadMainJp.create();

    ThreadMainBg threadMainBg(threadMainJp);

    threadMainBg.m_jobTotal = 10;
    threadMainBg.m_jobAcquired = 0;
    threadMainBg.tryBondPeers(threadMainJp, threadMainBg.m_jobTotal);
    threadMainBg.processTasks(-1);
    threadMainBg.waitForExit();
    threadMainJp.stopJobs();

    return 0;
}
