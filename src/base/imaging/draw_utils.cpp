/**
 * @file draw_utils.cpp
 * @brief base/imaging 域 —— OpenCV Mat 上的检测框绘制与人脸区域裁剪
 *
 * 所属模块：src/base/imaging（无状态工具，可被任意上层直接包含）。
 * 核心职责：把"点名/盘点结果怎么标在图上"这条业务口径固化成两色画法——
 *           绿色实线 = 唯一/正常，黄色虚线 = 重复命中；再加一个防越界的人脸裁剪。
 *
 * 与同目录 image_drawing 的分工（两条不相交的路，别混用）：
 *   - 本文件：输入是 cv::Mat（BGR），走 OpenCV rectangle/line；
 *   - image_drawing：输入是裸 buffer（RGB888/RGBA8888/YUV420SP），走距离场软件绘制，
 *     供 RKNN 输出缓冲与零拷贝 DMA 帧直接画框。
 * 二者互不包含，选哪条只取决于手上的像素载体是什么。
 */
/* 蔡超添加
封装绿色实线框、黄色虚线框的绘制。
主要用于人员点名和设备盘点模块
视频视频统一用绿色实线框
照片比对识别到且不重复的就是绿色实线框  重复的就是黄色虚线框 
*/
#include "draw_utils.h"

/**
 * @brief 画绿色实线框（= 这张脸/这个目标只命中了一次，正常）
 * @param image 目标图，**原地修改**；必须是 8UC3/8UC4 的 BGR 语义 Mat，
 *              否则 OpenCV 会用 cv::Scalar(0,255,0) 的通道数去匹配，颜色不对或抛异常
 * @param box   像素矩形，允许越出图像边界（cv::rectangle 自己裁剪，不会崩）
 * @param thickness 线宽（像素），默认 2；传 -1 会变成 cv::FILLED，即整块区域涂绿，
 *              调用方若从 UI 拿线宽必须保证 >0
 * @note 非线程安全：直接写调用方传入的 Mat 数据，同一 Mat 并发画两个框会互相踩踏。
 */
void DrawUtils::drawSolidGreenBox(cv::Mat& image, const cv::Rect& box, int thickness) {
    cv::rectangle(image, box, cv::Scalar(0, 255, 0), thickness);
}

/**
 * @brief 画黄色虚线框（= 同一次比对里命中了重复人脸，需要人工复核）
 * @param image/box/thickness 同 drawSolidGreenBox()
 * @note dash/gap 用 drawDashedRect() 的默认值（10/5），本函数没有透传入口——
 *       小脸（几十像素）下 10px 实段会把框画成近似实线，与"重复"的视觉约定退化，
 *       目前点名人脸普遍 >80px，未构成问题；要改就先给这里加参数而不是动默认值。
 */
void DrawUtils::drawDashedYellowBox(cv::Mat& image, const cv::Rect& box, int thickness) {
    drawDashedRect(image, box, cv::Scalar(0, 255, 255), thickness);
}

/**
 * @brief 虚线矩形：按"实段 dash_length + 空段 gap_length"的周期沿四条边逐段画线
 * @param image    目标图，原地修改
 * @param rect     矩形（像素坐标）；width/height <=0 时四条边的循环体一次都不执行，
 *                 即静默不画，不报错
 * @param color    cv::Scalar 线段颜色
 * @param thickness 线宽（像素）
 * @param dash_length/gap_length 实段与空段长度，默认 10 与 5
 *        【数字来源】这两个值是沿用的绘制习惯数而非硬件/协议约束：一个周期 15px，
 *        在 1080p 预览里约等于"看得清是虚线、又不会碎成点"；改小会让虚线接近实线，
 *        失去与实线框（唯一命中）的区分意义。
 * @note 底边与右边各回退 1 像素（`+height-1` / `+width-1`），避免 thickness>1 时
 *       画到相邻框的行上；上下边用同一个 min 上界裁剪，故末段可能短于 dash_length。
 * @note 复杂度 O(周长/(dash+gap))，每次一张图几十个框可忽略；纯 CPU 写像素，
 *       不要放到零拷贝 DMA 帧上（那条路走 image_drawing，且需要先做缓存同步）。
 */
void DrawUtils::drawDashedRect(cv::Mat& image, const cv::Rect& rect,
                               const cv::Scalar& color, int thickness,
                               int dash_length, int gap_length) {
    int pattern_length = dash_length + gap_length;
    
    // 上边
    for (int x = rect.x; x < rect.x + rect.width; x += pattern_length) {
        int end_x = std::min(x + dash_length, rect.x + rect.width);
        cv::line(image, cv::Point(x, rect.y), cv::Point(end_x, rect.y), color, thickness);
    }
    
    // 下边
    for (int x = rect.x; x < rect.x + rect.width; x += pattern_length) {
        int end_x = std::min(x + dash_length, rect.x + rect.width);
        int y = rect.y + rect.height - 1;
        cv::line(image, cv::Point(x, y), cv::Point(end_x, y), color, thickness);
    }
    
    // 左边
    for (int y = rect.y; y < rect.y + rect.height; y += pattern_length) {
        int end_y = std::min(y + dash_length, rect.y + rect.height);
        cv::line(image, cv::Point(rect.x, y), cv::Point(rect.x, end_y), color, thickness);
    }
    
    // 右边
    for (int y = rect.y; y < rect.y + rect.height; y += pattern_length) {
        int end_y = std::min(y + dash_length, rect.y + rect.height);
        int x = rect.x + rect.width - 1;
        cv::line(image, cv::Point(x, y), cv::Point(x, end_y), color, thickness);
    }
}

/**
 * @brief 裁剪人脸/目标区域，并保证不越出图像边界
 * @param image 源图（只读）
 * @param box   待裁剪矩形；**允许是模型输出的越界框**（贴边目标常见），
 *              本函数先与 cv::Rect(0,0,cols,rows) 求交再裁
 * @return 深拷贝（clone）出来的新 Mat，可安全脱离 image 存活；
 *         与原图完全无交集、或 image 为空、或 box 尺寸为 0 时返回**空 Mat**——
 *         调用方必须判 .empty()，直接拿返回值做 imshow/imwrite/特征提取会拿到 0 像素图
 * @note 为什么一定要 clone：`image(safe_box)` 只是共享 data 的视图，源图一释放就是
 *       悬垂指针；点名链路会把裁出来的脸存进注册表用的 Mat，所以这里强制复制。
 *       代价是每张脸一份 w*h*3 的堆内存，人脸尺度下可忽略。
 */
cv::Mat DrawUtils::cropFaceRegion(const cv::Mat& image, const cv::Rect& box) {
    // 确保裁剪区域在图像范围内
    cv::Rect safe_box = box & cv::Rect(0, 0, image.cols, image.rows);
    if (safe_box.area() == 0) {
        return cv::Mat();
    }
    return image(safe_box).clone();
}