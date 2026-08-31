#ifndef TASK_CONFIG_H
#define TASK_CONFIG_H


#include <string>
#include <rknn_api.h>

struct TaskConfig
{
    std::string modelPath;
    std::string labelPath;
    rknn_core_mask core_mask;
};

#endif
