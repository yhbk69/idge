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
// ⚠⚠【核心语义警示：waitAndPop 名为 Pop 实为 Top（tryPop 已修复）】
//   tryPop 现为"取出即删除"（pop_heap + pop_back），符合命名语义；
//   但 waitAndPop 仍只读取堆顶 c_.front() 拷贝给调用方，不移除元素。
//   隐患：
//   1) 消费方循环调用 waitAndPop 会反复拿到同一最高优先级元素——
//      同一条检测结果被处理多次（重复告警/重复存图），且队列永远
//      "非空"，等待新数据的逻辑失效（忙等自旋风险）；
//   2) 想真正消费 waitAndPop 取出的堆顶必须额外手动调用 pop()，
//      二者不成对即产生"读了没删"（重复消费）或"删了没读"（丢数据）的错配；
//   3) 与标准库 priority_queue::pop()（只删不读）语义相反，
//      极易被熟悉 STL 的维护者误用。
//   需要"取出即删除"的阻塞语义时，应改用 BlockingQueue 或组合 top()+pop()。
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
    // 时间复杂度：名义 O(log n)，实际下方用的是 make_heap = O(n)（见警示）
    //
    // ⚠ 效率警示：两处 std::make_heap 都对整个容器重建堆，复杂度 O(n)，
    //   注释宣称的 O(log n) 需改用 push_heap（未满时）/ pop_heap+push_heap
    //   （替换时）才成立。max_size 较小时影响可忽略。
    // ⚠ 比较器不一致：满员判断用裸 operator<（*iterator_min < x），
    //   未走 cmp——若模板参数换了自定义 Compare（如 greater），
    //   "谁优先"在两处会互相矛盾；且 min_element 也是默认 < 语义。
    // ⚠【唤醒缺失】push 只加锁写数据，从不 cv_.notify_*()：
    //   正在 waitAndPop 中挂起的消费者不会因新元素入队而被唤醒，
    //   只有其他线程碰巧 notify 该 cv_ 才恢复——阻塞消费路径事实上
    //   不可依赖，建议本类仅与 tryPop 轮询搭配使用（当前工程用法）。
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

    // ============================================================================
    // waitAndPop: 等待并获取优先级最高的元素（阻塞）
    // ============================================================================
    // 如果队列为空，阻塞等待直到有元素被插入。
    // 一旦有元素可用，立即返回 true。
    //
    // 使用场景：
    //   - 消费者线程需要等待生产者生产数据
    //   - 程序启动时等待第一帧数据
    //
    // ⚠ 隐患（与文件头警示联动）：
    //   1) 不移除元素：队列非空时立即返回堆顶旧值，消费循环会
    //      对同一元素反复执行后处理（重复告警等）；
    //   2) push 不 notify cv_（见 push 警示），挂起在此处的线程
    //      不会被新数据唤醒；若队列被清空后又无人 push，
    //      本 wait 无谓词超时、无 shutdown 通道 → 停机时永久挂起，
    //      join 该消费者线程会死锁。
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

