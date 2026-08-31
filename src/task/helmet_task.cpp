
#include <math.h>
#include <chrono>
#include <functional>
#include "helmet_task.h"
#include "ThreadPool.hpp"
#include "DmaBufferPool.h"
#include "easy_timer.h"

void HelmetTask::run(image_buffer_t& image, DetectContext context) 
{
    
    object_detect_result_list results1;

    std::shared_ptr<YOLO11Model> model = dpool::context->getModel("1");
    
    auto t1 = chrono::system_clock::now();
    model->detect(&image, &results1, true);
    // auto t2 = chrono::system_clock::now();
    // std::cout << "thread Id:" << std::this_thread::get_id() << " step 1 detect time : " << chrono::duration_cast<chrono::microseconds>(t2 - t1).count() / 1000.0 << std::endl;
    
    // std::shared_ptr<YOLO11Model> model2 = dpool::context->getModel("2");
    // model2->detect(&image, &results1, true);
    // auto t3 = chrono::system_clock::now();
    // //std::cout << "step 2 detect time : " << chrono::duration_cast<chrono::microseconds>(t3 - t2).count() / 1000.0 << std::endl;

    // std::shared_ptr<YOLO11Model> model3 = dpool::context->getModel("3");
    // model3->detect(&image, &results1, true);

    // std::shared_ptr<YOLO11Model> model4 = dpool::context->getModel("4");
    // model4->detect(&image, &results1, true);
    // auto t4 = chrono::system_clock::now();
    // //std::cout << "step all detect time : " << chrono::duration_cast<chrono::microseconds>(t4 - t1).count() / 1000.0 << std::endl;

    // DetectResult r;
    // r.time = t2.time_since_epoch().count();
    // context.detectResultQueue->push(r);

    context.dmaBufferPool->release(image.dmaBuffer);

}

void HelmetTask::runWithDma(DmaBuffer* detectFrame, DetectContext& context)
{
    // TIMER timer;
    // timer.tik();
    // std::shared_ptr<RknnModel> model = dpool::context->getRknnModel("1");
    
    // model->setInputDmaBuf(detectFrame);
    // timer.tok();
    // timer.print_time("setInputDmaBuf");

    // timer.tik();
    // model->run();
    // // void* out_data = model->getOutputPtr(0);
    // timer.tok();
    // timer.print_time("model->run()");
    // vector<vector<float>> result = model->getResult();
    // DetectResult r;
    // r.time = chrono::system_clock::now().time_since_epoch().count();
    // r.result = result;
    // context.detectResultQueue->push(r);
    // model->destroyInputDma();
    // int out_w = 8400;
    // int out_h = 84;
    // context.dmaBufferPool->release(detectFrame);
    //detectFrame->release();
    //delete detectFrame;
    //auto detections = postproc.process

}


