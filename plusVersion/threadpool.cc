
#include <functional>
#include <future>
#include <iostream>
#include <thread>

using namespace std;

#include "threadpool.h"

int sum(int a,int b)
{
    this_thread::sleep_for(chrono::seconds(2));
    return a+b;
}

//适合高版本c++用户
int main()
{
    ThreadPool pool;

    pool.start(2);

    future<int> res = pool.submitTask(sum,1,2);
    pool.submitTask(sum,1,2);
    pool.submitTask(sum,1,2);
    pool.submitTask(sum,1,2);
    pool.submitTask(sum,1,2);
    std::cout<<res.get()<<std::endl;
}
