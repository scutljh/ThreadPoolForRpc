#include <iostream>
#include <chrono> //todo
#include <thread>

#include "threadpool.h"

class Mytask : public Task
{
public:
    Mytask(int be, int en) : begin_(be), end_(en)
    {
    }
    Any run() override
    {
        std::cout << "tid" << std::this_thread::get_id() << "begin !" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));

        uint64_t sum = 0;
        for (uint64_t i = begin_; i <= end_; ++i)
        {
            sum += i;
        }
 
        std::cout << "tid" << std::this_thread::get_id() << "end !" << std::endl;
        return sum;
    }

private:
    int begin_;
    int end_;
};

int main()
{
    { // ok
        ThreadPool pool;
        pool.setPattren(tpPattern::CACHED_);
        pool.start(2);

        //result对象也是局部对象，要析构
        Result res = pool.submitTask(new Mytask(1, 10000000));
        Result res2 = pool.submitTask(new Mytask(100000001, 200000000));
        
        pool.submitTask(new Mytask(100000001, 200000000));
        pool.submitTask(new Mytask(100000001, 200000000));
        pool.submitTask(new Mytask(100000001, 200000000));
        

        uint64_t sum1 = res.get().any_cast<uint64_t>();
        std::cout << sum1 << std::endl;
    }  //出作用域result对象也要析构  （其中的信号量要析构，锁和条件变量也跟着析构）

    std::cout << "main over" << std::endl;
    getchar();

#if 0


        { // 复现了，死锁
        ThreadPool pool;
        pool.setPattren(tpPattern::CACHED_);

        pool.start(4);

        Result res1 = pool.submitTask(new Mytask(1, 100000000));
        Result res2 = pool.submitTask(new Mytask(100000001, 200000000));
        Result res3 = pool.submitTask(new Mytask(200000001, 300000000));
        pool.submitTask(new Mytask(200000001, 300000000));

        pool.submitTask(new Mytask(200000001, 300000000));
        pool.submitTask(new Mytask(200000001, 300000000));

        // 拿取返回值
        uint64_t sum1 = res1.get().any_cast<uint64_t>();
        uint64_t sum2 = res2.get().any_cast<uint64_t>();
        uint64_t sum3 = res3.get().any_cast<uint64_t>();

        std::cout << (sum1 + sum2 + sum3) << std::endl;
    }

    // main thread stop
    getchar();
#endif
}
