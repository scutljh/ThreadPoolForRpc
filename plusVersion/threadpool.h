#pragma once

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>
#include <iostream>

const int TASKNUM_CEILING = 1024; // debug  INT32_MAX
const int THREADNUM_CEILING = 512;
const int THREADMAXIDLE = 60; // 单位:second

// 线程池支持的模式
enum class tpPattern // 限制enum的使用，防止多枚举冲突
{
    FIXED_,  // 固定数量线程池
    CACHED_, // 动态增长线程池
};

//---------------------------------------------

class Thread
{
public:
    using ThreadFunc = std::function<void(int)>;

    Thread(ThreadFunc func)
        : threadfunc_(func), threadId_(ididx_++)
    {
    }
    ~Thread() = default;

    void start()
    {
        std::thread t(threadfunc_, threadId_);
        t.detach();
    }

    int getId() const
    {
        return threadId_;
    }

private:
    ThreadFunc threadfunc_;
    static int ididx_;
    int threadId_; // 保存线程id   简单的用一个静态int维护一下就行了
};
int Thread::ididx_ = 0;

//---------------------------------------------------------

class ThreadPool
{
public:
    ThreadPool()
        : threadsNum_(0), taskNum_(0), threadCeiling_(THREADNUM_CEILING), taskCeiling_(TASKNUM_CEILING), pattern_(tpPattern::FIXED_), started_(false), idleThreadsNum_(0), curThreadNum_(0)
    {
    }
    ~ThreadPool()
    {
        started_ = false;
        // 等待线程池中所有线程返回
        std::unique_lock<std::mutex> lock(taskQueueMtx_);

        // 线程：执行任务 或 在wait上等待  //直接唤醒，然后通过一个标记位做出不同动作
        queueEmpty_.notify_all();
        exitCond_.wait(lock, [&]()
                       { return threads_.size() == 0; }); // 出现问题！！！别忘了通知！！
    }

    void start(int threadsNum = std::thread::hardware_concurrency())
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

    // 第二版 使用可变惨模板编程  让此接口能够接受任意函数和任意的参数个数
    // Result submitTask(Task *task);
    // 涉及引用折叠
    template <typename Func, typename... Args>
    auto submitTask(Func &&func, Args &&...args) -> std::future<decltype(func(args...))>
    {

        using retType = decltype(func(args...)); // type !
        // 堆上生成一个任务对象
        auto task = std::make_shared<std::packaged_task<retType()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        std::future<retType> result = task->get_future(); // package_task独有的get_future方法，返回一个future<?> 对象

        std::unique_lock<std::mutex> lock(taskQueueMtx_);

        // 任务队列满，等待消费
        // modern style -- lock,predicate_obj(overload)       //bool - waitfor超时时间为10s，最多等待10s
        if (!queueFull_.wait_for(lock, std::chrono::seconds(1), [&](){ 
            return taskNum_ < taskCeiling_; 
        }))
        {
            std::cerr << "task queue still full, bad submit" << std::endl;
            auto tmp = std::make_shared<std::packaged_task<retType()>>([](){ 
                return retType(); 
            });
            (*tmp)(); //别忘记执行任务
            return tmp->get_future();
        }

        // 任务队列有空余了 接着生产
        // taskQueue_.emplace(sp);  Task是function<void()>  返回值void没有参数的函数对象 我们外套一层
        taskQueue_.emplace([task](){
            //套一层，对真实任务的封装
            (*task)(); 
        });
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

        return result;
    }

    // setter
    void setPattren(tpPattern pattern)
    {
        if (PoolStatus())
            return;
        pattern_ = pattern;
    }
    void setTaskCeiling(uint16_t taskCeiling)
    {
        if (PoolStatus())
            return;
        taskCeiling_ = taskCeiling;
    }
    void setThreadCeiling(uint16_t threadCeiling)
    {
        if (PoolStatus())
        {
            return;
        }
        if (pattern_ == tpPattern::CACHED_)
        {
            threadCeiling_ = threadCeiling;
        }
    }

private:
    // threadfunc defines here   为了线程函数能够使用线程池中的同步机制
    void threadFunc(int threadID)
    {
        auto lastTime = std::chrono::high_resolution_clock().now();

        // 线程不是处理一个任务就万事大吉了，轮询拿任务
        // while (started_)
        for (;;) // 有任务要接着做
        {
            Task task;  //以前的shared_ptr是用户传递的，所以为了保证生命周期才使用，现在的生命周期我们控制，不管了就
            {
                std::unique_lock<std::mutex> lock(taskQueueMtx_);

                // cached模式 为了防止新创建出的空闲线程太多，设定线程空闲60s后就回收
                // 时间计算： 当前时间 - 上一次线程执行时间 > 60s

                // 反复调用wait，每一秒返回一次  使用waitfor返回值cv_status区分  (此版本一定会等待对应时间返回，无需条件)
                while (/*started_ && */ taskNum_ == 0) // 任务队列中有任务了，才去消费
                {
                    if (!started_) // 1和3类都到此
                    {
                        // 线程池退出，任务执行完后的线程也要回收，别循环了 .2类
                        threads_.erase(threadID);
                        curThreadNum_--;
                        std::cout << "thread " << std::this_thread::get_id() << "exit" << std::endl;
                        exitCond_.notify_all();
                        return; // 结束线程
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
                }

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
                task(); //functors
            }
            idleThreadsNum_++;
            lastTime = std::chrono::high_resolution_clock().now();
        }
    }

    bool PoolStatus() const // true -- running
    {
        return started_;
    }

private:
    // 线程池工作模式
    tpPattern pattern_;
    // 线程列表
    std::unordered_map<int, std::unique_ptr<Thread>> threads_;
    // 初始化线程池数量
    uint16_t threadsNum_;
    // 线程数阈值 cached
    uint16_t threadCeiling_;
    // 线程池真实线程数量
    std::atomic_int curThreadNum_;
    // 空闲线程数量(cached模式下，如果空闲线程的数量达到一定阈值，那么要销毁一些)
    std::atomic_uint idleThreadsNum_;

    using Task = std::function<void()>;
    std::queue<Task> taskQueue_; // 生命周期不由用户了，也不用写shared_ptr了
    std::atomic_uint taskNum_;

    // 任务数阈值
    uint32_t taskCeiling_;

    // 线程池启动状态，如果已经启动，则不允许再进行set
    std::atomic_bool started_;

    // 线程同步
    std::mutex taskQueueMtx_;
    std::condition_variable queueFull_;
    std::condition_variable queueEmpty_;

    std::condition_variable exitCond_; // 回收用

    // noncopyable
    ThreadPool(const ThreadPool &) = delete;
    void operator=(const ThreadPool &) = delete;
};
