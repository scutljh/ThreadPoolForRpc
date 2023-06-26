#include "threadpool.h"

#include <functional>
#include <thread>
#include <iostream>

const int TASKNUM_CEILING = 1024; // debug  INT32_MAX
const int THREADNUM_CEILING = 512;
const int THREADMAXIDLE = 60; // 单位:second

// -----------------task------------------------
Task::Task()
    : result_(nullptr)
{
}

void Task::exec()
{
    if (result_ != nullptr)
    {
        // 设置返回值
        result_->setVal(run()); // run还是多态执行
    }
}

void Task::setResult(Result *result)
{
    result_ = result;
}

//----------------task end----------------------------

//---------------------Thread-------------------

int Thread::ididx_ = 0;

Thread::Thread(ThreadFunc func)
    : threadfunc_(func), threadId_(ididx_++)
{
}

Thread::~Thread()
{
}

int Thread::getId() const
{
    return threadId_;
}

// 启动线程
void Thread::start()
{
    std::thread t(threadfunc_, threadId_);
    t.detach();
} // t下来销毁，由于线程对象t被分离了，它的执行状态与主线程无关，因此即使线程对象t被销毁，子线程仍然可以继续独立运行。

//---------------------Thread END-----------------------

//-------------------------ThreadPool----------------------
ThreadPool::ThreadPool()
    : threadsNum_(0)
    , taskNum_(0)
    , threadCeiling_(THREADNUM_CEILING)
    , taskCeiling_(TASKNUM_CEILING)
    , pattern_(tpPattern::FIXED_)
    , started_(false)
    , idleThreadsNum_(0)
    , curThreadNum_(0)
{
}

ThreadPool::~ThreadPool()
{
    started_ = false;
    // 等待线程池中所有线程返回
    std::unique_lock<std::mutex> lock(taskQueueMtx_);

    // 线程：执行任务 或 在wait上等待  //直接唤醒，然后通过一个标记位做出不同动作
    queueEmpty_.notify_all();
    exitCond_.wait(lock, [&]()
                   { return threads_.size() == 0; }); // 出现问题！！！别忘了通知！！
}

// for future
bool ThreadPool::PoolStatus() const
{
    return started_;
}

void ThreadPool::setPattren(tpPattern pattern)
{
    if (PoolStatus())
        return;
    pattern_ = pattern;
}

void ThreadPool::setTaskCeiling(uint16_t taskCeiling)
{
    if (PoolStatus())
        return;
    taskCeiling_ = taskCeiling;
}

void ThreadPool::setThreadCeiling(uint16_t threadCeiling)
{
    if(PoolStatus())
    {
        return;
    }
    if (pattern_ == tpPattern::CACHED_)
    {
        threadCeiling_ = threadCeiling;
    }
}

// 启动线程池
void ThreadPool::start(int threadsNum)
{
    started_ = true;
    threadsNum_ = threadsNum;
    curThreadNum_ = threadsNum;

    // 创建线程对象  但不是创建时启动
    for (int i = 0; i < threadsNum_; ++i)
    {
        // threads_.emplace_back(new Thread(std::bind(&ThreadPool::threadFunc,this)));
        std::unique_ptr<Thread> up(new Thread(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1)));
        int tid = up->getId();
        threads_.emplace(tid, std::move(up));
    }

    // 启动所有线程   threadfunc-- 等待任务队列中任务就绪，拿任务运行
    for (int i = 0; i < threadsNum_; ++i)
    {
        threads_[i]->start();
        idleThreadsNum_++; // 空闲线程数
    }
}

// 向线程池中的任务队列提交任务
Result ThreadPool::submitTask(Task *task)
{
    std::shared_ptr<Task> sp(task);
    std::unique_lock<std::mutex> lock(taskQueueMtx_);

    // 优化：任务队列满了，服务降级，用户提交任务最长不能阻塞超过1s。超过就判断失败并返回，不要把用户阻塞住

    // while(taskNum_ == taskCeiling_)
    // {
    //     queueFull_.wait(lock);
    // }

    // 任务队列满，等待消费
    // modern style -- lock,predicate_obj(overload)       //bool - waitfor超时时间为10s，最多等待10s
    if (!queueFull_.wait_for(lock, std::chrono::seconds(1), [&]()
                             { return taskNum_ < taskCeiling_; }))
    {
        // 条件不满足且超时   降级
        std::cerr << "task queue still full, bad submit" << std::endl;
        return Result(sp, false); // 任务提交失败的返回
    }

    // 任务队列有空余了 接着生产
    taskQueue_.emplace(sp);
    taskNum_++;

    queueEmpty_.notify_all(); // 绝对不空了，能来消费了

    // cached模式 任务处理比较紧急 适合：小而快的任务，耗时的任务会导致创建过多线程
    if (pattern_ == tpPattern::CACHED_ && taskNum_ > idleThreadsNum_ && curThreadNum_ < threadCeiling_)
    {
        std::cout << " extern threads " << std::endl;
        // 创建新线程对象添加到线程池中
        std::unique_ptr<Thread> nt(new Thread(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1)));
        int tid = nt->getId();
        threads_.emplace(tid, std::move(nt));
        threads_[tid]->start(); // 启动新线程

        curThreadNum_++;
        idleThreadsNum_++;
    }

    return Result(sp);
}

// 线程启动后，主要是从任务队列中消费任务
void ThreadPool::threadFunc(int threadID)
{
    auto lastTime = std::chrono::high_resolution_clock().now();

    // 线程不是处理一个任务就万事大吉了，轮询拿任务
    // while (started_)
    for (;;) // 有任务要接着做
    {
        std::shared_ptr<Task> task;
        {
            std::unique_lock<std::mutex> lock(taskQueueMtx_);

            // cached模式 为了防止新创建出的空闲线程太多，设定线程空闲60s后就回收
            // 时间计算： 当前时间 - 上一次线程执行时间 > 60s

            // 反复调用wait，每一秒返回一次  使用waitfor返回值cv_status区分  (此版本一定会等待对应时间返回，无需条件)
            while (/*started_ && */ taskNum_ == 0) // 任务队列中有任务了，才去消费
            {
                if (!started_) // 1和3类都到此  //1类是执行任务中的线程 3类是刚从上面进来的线程
                {
                    // 线程池退出，任务执行完后的线程也要回收，别循环了 .2类
                    threads_.erase(threadID);
                    curThreadNum_--;
                    std::cout << "thread " << std::this_thread::get_id() << "exit" << std::endl;
                    exitCond_.notify_all();
                    return;   //结束线程
                }

                if (pattern_ == tpPattern::CACHED_)
                {
                    // timeout no_timeout
                    if (std::cv_status::timeout == queueEmpty_.wait_for(lock, std::chrono::seconds(1)))
                    {
                        auto now = std::chrono::high_resolution_clock().now();
                        auto gap = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                        if (gap.count() >= THREADMAXIDLE && curThreadNum_ > threadsNum_) // 至少留下初始化线程个数个
                        {
                            // 脱离管理  因此需要有映射关系 要知道该threadfunc被哪个线程对象执行了  决定把池中的线程列表改成map维护
                            threads_.erase(threadID);
                            curThreadNum_--;
                            idleThreadsNum_--;

                            std::cout << "thread " << std::this_thread::get_id() << "destroyed" << std::endl;
                            return;
                        }
                    }
                }
                else // fixed
                {
                    queueEmpty_.wait(lock);
                }

                // // wake up
                // if (!started_) // 线程池要结束了 ,wait cond的都需要回收 .1类
                // {
                //     threads_.erase(threadID);
                //     curThreadNum_--;
                //     std::cout << "thread " << std::this_thread::get_id() << "exit" << std::endl;
                //     exitCond_.notify_all();
                //     return;
                // }

            }//  有任务了

            idleThreadsNum_--;
            std::cout << "tid" << std::this_thread::get_id() << "获取任务成功..." << std::endl;

            // 消费
            task = taskQueue_.front();
            taskQueue_.pop();
            --taskNum_;

            // extra 优化，通知其他线程还可以接着来拿了
            if (taskNum_ > 0)
            {
                queueEmpty_.notify_all();
            }

            // 简单一点，一有空位置就允许生产了
            queueFull_.notify_all(); // 不满了，能生产了
        }
        if (task != nullptr)
        {
            // task->run();  //返回值如何填？返回值来自于run,但run又是纯虚函数，都没有实现，肯定不能直接加。那就在task中在写封装一层！
            // 通过这个普通函数的封装，做比run更多的事情，用户层面仍只需要重写一个run即可
            task->exec();
        }
        idleThreadsNum_++;
        lastTime = std::chrono::high_resolution_clock().now();
    }
}
//  优化：线程真正执行任务队列中的任务时，释放锁  才更合理

//---------------------ThreadPool END------------------

Result::Result(std::shared_ptr<Task> task, bool isValid)
    : task_(task), isValid_(isValid) // 是否可用
{
    task_->setResult(this);  //result构造时完成双向绑定
}

void Result::setVal(Any ret)
{
    ret_ = std::move(ret);
    sem_.post();
}

// 用户api
Any Result::get()
{
    if (!isValid_) // 一般是提交失败进入
    {
        return "bad submit";
    }
    sem_.wait(); // task没执行完的话，直接阻塞用户
    return std::move(ret_);
}
