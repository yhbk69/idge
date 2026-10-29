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
//
// ⚠ 本类**只有非阻塞出口**：原先的 waitAndPop（名为 Pop 实为 Top：只读堆顶不删除，
//   且 push 从不 notify、无 shutdown 通道，停机可致消费者永久挂起）在全工程零调用的
//   前提下已删除，连同其 condition_variable 成员。若将来确实需要"阻塞消费"语义，
//   请改用同目录的 BlockingQueue，别在本类上补一个名字带 Pop 却不删元素的函数。
//
// 【内部结构】c_ 是 std::vector 维护的堆：c_.front() 恒为堆顶（最大元素），
//   但其余元素顺序无序，begin()/end() 暴露的是未排序的裸迭代器，
//   且遍历期间不加锁——与 push 并发迭代属数据竞争（UB），
//   只应在确定单线程静默期使用。
// ============================================================================

#ifndef FIXED_SIZE_PRIORITY_QUEUE_H_
#define FIXED_SIZE_PRIORITY_QUEUE_H_

#include <iostream>
#include <algorithm>
#include <vector>
#include <mutex>

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
    // 时间复杂度：名义 O(log n)，实际下方用的是 make_heap = O(n)（见警示）
    //
    // ⚠ 效率警示：两处 std::make_heap 都对整个容器重建堆，复杂度 O(n)，
    //   注释宣称的 O(log n) 需改用 push_heap（未满时）/ pop_heap+push_heap
    //   （替换时）才成立。max_size 较小时影响可忽略。
    // ⚠ 比较器不一致：满员判断用裸 operator<（*iterator_min < x），
    //   未走 cmp——若模板参数换了自定义 Compare（如 greater），
    //   "谁优先"在两处会互相矛盾；且 min_element 也是默认 < 语义。
    // ⚠ 出队只有 tryPop 一条路（非阻塞轮询）：本类没有阻塞等待出口，
    //   生产者入队不会唤醒任何消费者——消费方必须自己轮询。需要
    //   "入队即唤醒"的场景请用同目录的 BlockingQueue。
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
    // tryPop: 尝试获取并移除优先级最高的元素（非阻塞）
    // ============================================================================
    // 如果队列为空，立即返回 false。
    // 如果队列非空，将队首元素拷贝到 value 并移除该元素，返回 true。
    //
    // 注意：这个方法会移除元素，确保每次调用获取不同的结果
    //   （历史上曾不移除元素，develop 分支已修复为 pop_heap+pop_back）
    // ============================================================================
    inline bool  tryPop(T& value) {
      std::unique_lock<std::mutex> lock(mtx_);
      if (c_.empty()) {
            return false; 
        }
      value = c_.front();
      std::pop_heap(c_.begin(), c_.end(), cmp);
      c_.pop_back();
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
    // 禁止堆分配（这个对象应该在栈上或作为成员变量）
    void * operator new   (size_t);
    void * operator new[] (size_t);
    void   operator delete   (void *);
    void   operator delete[] (void*);
};

#endif  // FIXED_SIZE_PRIORITY_QUEUE_H_

