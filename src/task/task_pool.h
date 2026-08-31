#ifndef TASK_POOL_H
#define TASK_POOL_H

#include <cassert>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include "rknn_api.h"
#include "SharedTypes.hpp"

class TaskPool
{
public:



    TaskPool()
    {
    }

    // 获取单例实例的静态方法
    static TaskPool &getInstance()
    {
        static TaskPool instance; // C++11 保证了静态局部变量的线程安全性

        return instance;
    }

    void init()
    {

    }

    
}

#endif