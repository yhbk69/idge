#ifndef MODEL_POOL_H
#define MODEL_POOL_H

#include <vector>
#include <iostream>
#include <mutex>
#include <queue>
#include <memory>
#include "SharedTypes.hpp"
#include "yolo11_model.hpp"

// rknnModel模型类, inputType模型输入类型, outputType模型输出类型

class ModelPool
{
private:
    AppConfig config; // 配置参数
    long long id;
    std::mutex idMtx, queueMtx;
    std::unordered_map<std::string, std::shared_ptr<YOLO11Model>> m_models;

protected:
public:
    ModelPool(const AppConfig &config)
    {
        this->config = config;
        this->id = 0;
    }
    std::shared_ptr<YOLO11Model> getModel(std::string modelId)
    {
        if (modelId.empty())
        {
            return NULL;
        }

        return m_models[modelId];
    }
    int init()
    {
        std::shared_ptr<YOLO11Model> detector1 = std::make_shared<YOLO11Model>(
            "/home/ubuntu/projects/idge/model/yolo11n.rknn",
            "/home/ubuntu/projects/idge/model/coco_80_labels_list.txt",
            RKNN_NPU_CORE_0);
        m_models["1"] = detector1;

        std::shared_ptr<YOLO11Model> detector2 = std::make_shared<YOLO11Model>(
            "/home/ubuntu/projects/idge/model/yolo11n.rknn",
            "/home/ubuntu/projects/idge/model/coco_80_labels_list.txt",
            RKNN_NPU_CORE_1

        );
        m_models["2"] = detector2;

        std::shared_ptr<YOLO11Model> detector3 = std::make_shared<YOLO11Model>(
            "/home/ubuntu/projects/idge/model/yolo11n.rknn",
            "/home/ubuntu/projects/idge/model/coco_80_labels_list.txt",
            RKNN_NPU_CORE_2);
        m_models["3"] = detector3;

        std::shared_ptr<YOLO11Model> detector4 = std::make_shared<YOLO11Model>(
            "/home/ubuntu/projects/idge/model/yolo11n.rknn",
            "/home/ubuntu/projects/idge/model/coco_80_labels_list.txt",
            RKNN_NPU_CORE_0);
        m_models["4"] = detector4;

        return 0;
    }
    // 模型推理
    ~ModelPool()
    {
    }
};

#endif
