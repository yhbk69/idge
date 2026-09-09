// ============================================================================
// ModelPool.hpp - RKNN 模型池
// ============================================================================
//
// 功能：
//   管理多个 YOLO11Model 实例，每个实例绑定一个 NPU 核心。
//   通过 getModel(modelId) 获取指定模型实例。
//
// RK3588 NPU 核心分配：
//   - 模型 "1": NPU 核心 0（RKNN_NPU_CORE_0）
//   - 模型 "2": NPU 核心 1（RKNN_NPU_CORE_1）
//   - 模型 "3": NPU 核心 2（RKNN_NPU_CORE_2）
//   - 模型 "4": NPU 核心 0（RKNN_NPU_CORE_0，与模型1共享）
//
// 级联模型机制：
//   每个模型独立加载到不同的 NPU 核心，
//   推理时可以并行执行（每个线程绑定一个 NPU 核心）。
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
    AppConfig config;        // 配置参数（模型路径、标签路径等）
    long long id;            // 模型 ID 计数器
    std::mutex idMtx, queueMtx;  // 互斥锁（线程安全）

    // 模型实例映射表（key: 模型ID, value: YOLO11Model 实例）
    std::unordered_map<std::string, std::shared_ptr<YOLO11Model>> m_models;

public:
    ModelPool(const AppConfig &config)
    {
        this->config = config;
        this->id = 0;
    }

    // ============================================================================
    // getModel - 获取指定 ID 的模型实例
    // ============================================================================
    // 参数：
    //   - modelId: 模型 ID（"1"~"4"）
    //
    // 返回：
    //   - 成功：YOLO11Model 实例（shared_ptr）
    //   - 失败：NULL
    //
    // ============================================================================
    std::shared_ptr<YOLO11Model> getModel(std::string modelId)
    {
        if (modelId.empty()) {
            return NULL;
        }
        return m_models[modelId];
    }

    // ============================================================================
    // init - 初始化模型池（加载 4 个模型到不同 NPU 核心）
    // ============================================================================
    // 每个模型使用相同的权重文件（yolo11n.rknn），
    // 但绑定到不同的 NPU 核心以实现并行推理。
    //
    // 注意：实际生产中应从配置文件读取模型路径
    // ============================================================================
    int init()
    {
        // 模型 1: NPU 核心 0
        std::shared_ptr<YOLO11Model> detector1 = std::make_shared<YOLO11Model>(
            "model/yolo11n.rknn",
            "model/coco_80_labels_list.txt",
            RKNN_NPU_CORE_0);
        m_models["1"] = detector1;

        // 模型 2: NPU 核心 1
        std::shared_ptr<YOLO11Model> detector2 = std::make_shared<YOLO11Model>(
            "model/yolo11n.rknn",
            "model/coco_80_labels_list.txt",
            RKNN_NPU_CORE_1);
        m_models["2"] = detector2;

        // 模型 3: NPU 核心 2
        std::shared_ptr<YOLO11Model> detector3 = std::make_shared<YOLO11Model>(
            "model/yolo11n.rknn",
            "model/coco_80_labels_list.txt",
            RKNN_NPU_CORE_2);
        m_models["3"] = detector3;

        // 模型 4: NPU 核心 0（与模型1共享，适用于级联场景）
        std::shared_ptr<YOLO11Model> detector4 = std::make_shared<YOLO11Model>(
            "model/yolo11n.rknn",
            "model/coco_80_labels_list.txt",
            RKNN_NPU_CORE_0);
        m_models["4"] = detector4;

        return 0;
    }

    ~ModelPool() {}
};

#endif
