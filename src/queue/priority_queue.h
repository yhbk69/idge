// Copyright 2016  Junbo Zhang
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache License 2.0 for the specific language governing permissions and
// limitations under the License.

// ============================================================================
// PriorityQueue - 固定大小的优先级队列
// ============================================================================
//
// 作用：
//   保持队列中优先级最高的 N 个元素。
//   当队列满时，新元素会替换优先级最低的旧元素。
//
// 使用场景：
//   - 检测结果队列：只保留最新的 N 次检测结果
//   - 帧缓冲队列：只保留最新的 N 帧
//
// 特点：
//   - 线程安全（使用 mutex 保护）
//   - 自动维护优先级（使用堆数据结构）
//   - 队列满时自动淘汰低优先级元素
//   - 非阻塞 tryPop：立即返回，不等待
//   - 阻塞 waitAndPop：等待直到有数据
//
// ============================================================================

#ifndef FIXED_SIZE_PRIORITY_QUEUE_H_
#define FIXED_SIZE_PRIORITY_QUEUE_H_

#include <iostream>
#include <algorithm>
#include <vector>
#include <mutex>
#include <condition_variable>

/// A priority queue with fixed size. When the maximum size was reached,
/// the element with the lowest priority would be removed automatically.
template<typename T, typename Compare = std::less<T> >
class PriorityQueue
{
  public:
    PriorityQueue() : max_size_(0) {}
    PriorityQueue(size_t max_size) : max_size_(max_size) {}

    typedef typename std::vector<T>::iterator iterator;
    iterator begin() { return c_.begin(); }
    iterator end() { return c_.end(); }

    // ============================================================================
    // push: 插入元素
    // ============================================================================
    // 如果队列未满，直接插入并重新建堆。
    // 如果队列已满，比较新元素与最小元素：
    //   - 新元素更大：替换最小元素，重新建堆
    //   - 新元素更小：丢弃新元素
    //
    // 时间复杂度：O(log n)
    // ============================================================================
    inline void push(const T &x) {
      std::unique_lock<std::mutex> lock(mtx_);
      if(c_.size() == max_size_) {
        // 队列已满，找到最小元素
        typename std::vector<T>::iterator iterator_min = std::min_element(c_.begin(), c_.end(), cmp);
        if(*iterator_min < x) {
          // 新元素更大，替换最小元素
          *iterator_min = x;
          std::make_heap(c_.begin(), c_.end(), cmp);  // 重新建堆
        }
        // 否则丢弃新元素
      }
      else {
        // 队列未满，直接插入
        c_.push_back(x);
        std::make_heap(c_.begin(), c_.end(), cmp);  // 重新建堆
      }
    }

    // ============================================================================
    // pop: 移除优先级最高的元素
    // ============================================================================
    // 注意：这个 pop 移除的是堆顶元素（优先级最高的）
    // 如果需要移除优先级最低的，应该使用其他方法
    //
    // 时间复杂度：O(log n)
    // ============================================================================
    inline void pop() {
      std::unique_lock<std::mutex> lock(mtx_);
      if (c_.empty())
        return;
      std::pop_heap(c_.begin(), c_.end(), cmp);
      c_.pop_back();
    }

    // ============================================================================
    // tryPop: 尝试获取优先级最高的元素（非阻塞）
    // ============================================================================
    // 如果队列为空，立即返回 false。
    // 如果队列非空，将队首元素拷贝到 value，返回 true。
    //
    // 注意：这个方法不会移除元素，只是读取
    // ============================================================================
    inline bool  tryPop(T& value) const {
      std::unique_lock<std::mutex> lock(mtx_);
      if (c_.empty()) {
            return false; 
        }
      value = c_.front();
      return true;
    }

    // ============================================================================
    // waitAndPop: 等待并获取优先级最高的元素（阻塞）
    // ============================================================================
    // 如果队列为空，阻塞等待直到有元素被插入。
    // 一旦有元素可用，立即返回 true。
    //
    // 使用场景：
    //   - 消费者线程需要等待生产者生产数据
    //   - 程序启动时等待第一帧数据
    // ============================================================================
    inline bool waitAndPop(T& value) {
      std::unique_lock<std::mutex> lock(mtx_);
      cv_.wait(lock, [this]{ return !c_.empty(); });
      value = c_.front();
      // printf("queue size: %zu\n", queue_.size());
      return true;
    }

    inline const bool empty() const {
      std::unique_lock<std::mutex> lock(mtx_);
      return c_.empty();
    }

    inline const size_t size() const {
      std::unique_lock<std::mutex> lock(mtx_);
      return c_.size();
    }

    // ============================================================================
    // enlarge_max_size: 动态增大最大容量
    // ============================================================================
    // 只能增大，不能缩小。
    // 用于运行时动态调整队列大小。
    // ============================================================================
    inline void enlarge_max_size(size_t max_size) {
      std::unique_lock<std::mutex> lock(mtx_);
      if (max_size_ < max_size)
        max_size_ = max_size;
    }

  protected:
    std::vector<T> c_;           // 底层容器（使用 vector 实现堆）
    size_t max_size_;            // 最大容量
    Compare cmp;                 // 比较函数（默认 std::less，堆顶是最大元素）

  private:
    mutable std::mutex mtx_;     // 互斥锁（保护并发访问）
    std::condition_variable cv_; // 条件变量（用于 waitAndPop）
    // 禁止堆分配（这个对象应该在栈上或作为成员变量）
    void * operator new   (size_t);
    void * operator new[] (size_t);
    void   operator delete   (void *);
    void   operator delete[] (void*);
};

#endif  // FIXED_SIZE_PRIORITY_QUEUE_H_

