// ============================================================================
// ModelPool.hpp - RKNN 模型池
// ============================================================================
//
// 功能：
//   管理多个 YOLO11Model 实例，每个实例绑定一个 NPU 核心。
//   通过 getModel(modelId) 获取指定模型实例。
//
// 注意：实际模型加载在 ThreadPool.hpp 的 ExecuteContext 中，
//       这里只保留 getModel/getClassNames 接口供外部使用。
//
// 【借还语义的现状澄清（重要，勿被"池"字误导）】
//   早期设计为"借出-归还"式模型池（独占租借，用完 returnModel 放回空闲队列，
//   <queue> 等头文件即该版本的残留）。当前实现已退化为**只读注册表**：
//     - addModel 在初始化阶段一次性登记 "ID → shared_ptr<YOLO11Model>"；
//     - getModel 返回共享指针的**副本**，多个调用方可同时持有同一实例，
//       没有独占借出、也没有归还动作与引用计数之外的生命周期管理；
//     - 因此它不提供"池容量/排队等待"语义，"借了不还"在该模型下不存在，
//       真正的约束是"同一实例并发推理不安全"(rknn_context 串行，见
//       YOLO11Model::detect 的线程安全警示)——靠"每任务绑每模型"的使用
//       纪律保证，而非池本身强制。
//   线程安全补充：m_models 无锁。当前用法是"启动期 addModel、运行期只读
//   getModel"（无并发写），成立；若将来支持运行中热加载模型，需补 mutex
//   或改为启动后 const 化。
// ============================================================================

#ifndef MODEL_POOL_H
#define MODEL_POOL_H

#include <vector>
#include <iostream>
#include <mutex>
#include <queue>      // 遗留自"借出-归还"旧版实现，现未使用，保留以兼容包含链
#include <memory>
#include "SharedTypes.hpp"
#include "yolo11_model.hpp"

// 取用链路（现状说明）：生产代码实际使用的是 ThreadPool.hpp 中
// ExecuteContext 自带的同名模型表，经 thread_local 的 dpool::context 按
// "当前工作线程"注入（HelmetTask::run 即如此取模型"1"）；本类作为独立
// 的纯查表容器保留，接口与之一致（getModel/getClassNames 只读、无写锁，
// 运行期禁止并发 addModel）
class ModelPool
{
private:
    // 模型实例映射表（key: 模型ID "1"~"5", value: YOLO11Model 实例）
    std::unordered_map<std::string, std::shared_ptr<YOLO11Model>> m_models;

public:
    ModelPool() {}

    // ============================================================================
    // getModel - 获取指定 ID 的模型实例（只读查表，未命中返回 nullptr；
    //            返回的是共享指针副本，不代表独占借出，调用方无需归还）
    // ============================================================================
    std::shared_ptr<YOLO11Model> getModel(std::string modelId)
    {
        if (modelId.empty()) {
            return nullptr;
        }
        auto it = m_models.find(modelId);
        if (it == m_models.end()) {
            return nullptr;
        }
        return it->second;
    }

    // ============================================================================
    // getClassNames - 获取指定 ID 模型的类别名称列表
    // 返回整表值拷贝（调用方多为 UI 一次性取标签，忽略拷贝成本）；
    // 依赖前置 count() 判存，operator[] 此处不会隐式插入空项
    // ============================================================================
    std::vector<std::string> getClassNames(const std::string &modelId)
    {
        if (m_models.count(modelId)) {
            return m_models[modelId]->getClassNames();
        }
        return {};
    }

    // ============================================================================
    // getModelCount - 获取已加载的模型数量
    // ============================================================================
    size_t getModelCount() const
    {
        return m_models.size();
    }

    // ============================================================================
    // addModel - 添加模型实例（由外部调用；限启动期使用，同 ID 重复添加
    //            会直接覆盖旧实例——shared_ptr 语义下旧实例随最后一个引用
    //            释放而销毁，若有推理线程仍持有副本则继续存活至其用完）
    // ============================================================================
    void addModel(const std::string &modelId, std::shared_ptr<YOLO11Model> model)
    {
        m_models[modelId] = model;
    }

    // 析构不主动等待使用方：池只持有 shared_ptr 引用，
    // 模型实例随各 PpeTask 的最后引用自然销毁
    ~ModelPool() {}
};

#endif
