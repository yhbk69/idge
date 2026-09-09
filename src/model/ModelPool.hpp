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
// ============================================================================

#ifndef MODEL_POOL_H
#define MODEL_POOL_H

#include <vector>
#include <iostream>
#include <mutex>
#include <queue>
#include <memory>
#include "SharedTypes.hpp"
#include "yolo11_model.hpp"

class ModelPool
{
private:
    // 模型实例映射表（key: 模型ID "1"~"5", value: YOLO11Model 实例）
    std::unordered_map<std::string, std::shared_ptr<YOLO11Model>> m_models;

public:
    ModelPool() {}

    // ============================================================================
    // getModel - 获取指定 ID 的模型实例
    // ============================================================================
    std::shared_ptr<YOLO11Model> getModel(std::string modelId)
    {
        if (modelId.empty()) {
            return NULL;
        }
        return m_models[modelId];
    }

    // ============================================================================
    // getClassNames - 获取指定 ID 模型的类别名称列表
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
    int getModelCount() const
    {
        return m_models.size();
    }

    // ============================================================================
    // addModel - 添加模型实例（由外部调用）
    // ============================================================================
    void addModel(const std::string &modelId, std::shared_ptr<YOLO11Model> model)
    {
        m_models[modelId] = model;
    }

    ~ModelPool() {}
};

#endif
