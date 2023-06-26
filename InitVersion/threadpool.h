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

// 表示任意类型的上帝类
class Any
{
public:
    Any() = default;
    ~Any() = default;

    Any(Any &&) = default;
    Any &operator=(Any &&) = default;

    template <typename T>
    Any(T data) : base_(new Derive<T>(data))
    {
    }

    // 拿到真实的data，返回值由使用者填写，使用者自己指定类型，显然还是模板
    // 基类转成派生类类型，属于向下转型，那么必须保证这个基类类型指针确实是指向了这么一个派生类类型指针 因此需要借助RTTI，只有dynamic_cast可支持
    template <typename T>
    T any_cast()
    {
        Derive<T>* de_ptr = dynamic_cast<Derive<T>*>(base_.get());
        if (de_ptr == nullptr)
        {
            // 转型失败
            throw "bad cast , unmatch type!";
        }
        return de_ptr->data_;
    }

private:
    Any(const Any &) = delete;
    void operator=(const Any &) = delete;

    // Any需要一个基类作为自己的成员
    class Base
    {
    public:
        virtual ~Base() = default;
    };
    // 基类的派生类包含真实数据类型
    template <typename T>
    class Derive : public Base
    {
    public:
        Derive(T data) : data_(data)
        {
        }
        T data_;
    };

private:
    std::unique_ptr<Base> base_;
};

//------------------------------------

//条件变量+互斥锁实现的信号量
class Semaphore
{
public:
    Semaphore(int count = 0)
        : resCount_(count),isExit_(false)
    {
    }
    ~Semaphore()
    {
        isExit_ = true;
    }

    // P
    void wait()
    {
        if(isExit_)
        {
            return;
        }
        std::unique_lock<std::mutex> lock(mtx_);
        // 等待信号量资源>0，反之阻塞
        cond_.wait(lock, [&]()
                   { return resCount_ > 0; });
        resCount_--;
    }

    // V
    void post()
    {
        if(isExit_)
        {
            return;
        }
        std::unique_lock<std::mutex> lock(mtx_);
        resCount_++;
        cond_.notify_all(); //
    }

private:
    std::atomic_bool isExit_;    //result对象析构，信号量也析构  //g++底层的c++11条件变量析构没做事，所以我们做的补充
    // 信号量资源计数
    int resCount_;

    std::mutex mtx_;
    std::condition_variable cond_;
};

//-----------------------------------------------
class Task;

// 用户提交任务后会得到一个任务的执行结果
// 涉及线程通信
class Result
{
public:
    Result(std::shared_ptr<Task> task,bool isValid = true);
    ~Result() = default;

    //set ret  获取任务执行的返回值
    void setVal(Any ret);
    //get ret  由用户调用获取任务返回值
    Any get();

private:
    //任务返回值
    Any ret_;
    Semaphore sem_;

    // 提交失败不用阻塞了，一个标志位
    std::atomic_bool isValid_; 
    std::shared_ptr<Task> task_; //要获取返回值的任务对象，保证task对象有效
};

//-----------------------------------------------

// 顶层任务抽象基类
class Task
{
public:
    Task();
    ~Task() = default;
    void exec();
    virtual Any run() = 0;

    void setResult(Result* result);
private:
    Result* result_;  //不能shared_ptr，循环引用!  Result的生命周期肯定强于task，设计上就限定了，直接裸指针
};

// ------------------------------------------

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

    Thread(ThreadFunc Functors);
    ~Thread();

    void start();

    int getId() const;

private:
    ThreadFunc threadfunc_;
    static int ididx_;
    int threadId_;  //保存线程id   简单的用一个静态int维护一下就行了
};

//---------------------------------------------------------

class ThreadPool
{
public:
    ThreadPool();
    ~ThreadPool();

    void start(int threadsNum = std::thread::hardware_concurrency());
    Result submitTask(Task *task);

    // setter
    void setPattren(tpPattern pattern);
    void setTaskCeiling(uint16_t taskCeiling);
    void setThreadCeiling(uint16_t threadCeiling);

private:
    // threadfunc defines here   为了线程函数能够使用线程池中的同步机制
    void threadFunc(int threadID);

    bool PoolStatus() const;  //true -- running

private:
    // 线程池工作模式
    tpPattern pattern_;
    // 线程列表
    std::unordered_map<int,std::unique_ptr<Thread>> threads_;
    //初始化线程池数量
    uint16_t threadsNum_;
    // 线程数阈值 cached
    uint16_t threadCeiling_;
    // 线程池真实线程数量
    std::atomic_int curThreadNum_;
    //空闲线程数量(cached模式下，如果空闲线程的数量达到一定阈值，那么要销毁一些)
    std::atomic_uint idleThreadsNum_;

    // 任务队列
    // 用裸指针，用户提交一个临时对象，出语句析构，那这里怎么办？ 无法保证
    // 设计的时候保持对象生命周期直到任务执行结束以后   智能指针
    std::queue<std::shared_ptr<Task>> taskQueue_;
    std::atomic_uint taskNum_;
    
    // 任务数阈值
    uint32_t taskCeiling_;

    //线程池启动状态，如果已经启动，则不允许再进行set
    std::atomic_bool started_;
    

    // 线程同步
    std::mutex taskQueueMtx_;
    std::condition_variable queueFull_;
    std::condition_variable queueEmpty_;

    std::condition_variable exitCond_;  //回收用

    // noncopyable
    ThreadPool(const ThreadPool &) = delete;
    void operator=(const ThreadPool &) = delete;
};
