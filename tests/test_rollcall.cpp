/* test_rollcall —— 人员点名全链离线自测（进程内识别器 + 外部 exe 对照回归）
 *
 * 覆盖链路：
 *   A) 对照回归：同一张 test.jpg 分别经外部 face_recognition exe（fork+execv
 *      + JSON，即内化前的生产链路）与进程内 InProcessFaceRecognizer 推理，
 *      断言人脸数一致、按 bbox IoU 同名配对后特征余弦 > 0.99——
 *      证明 SCRFD/ArcFace 移植未改变识别语义（方案 P2.3 的回归兜底）。
 *   B) 业务全链：建注册任务 -> processPhotos 检测去重 -> saveTaskResult 落库
 *      -> 回读一致性 -> 双图去重 -> matchCancellation 一对一匹配 ->
 *      confirmCancellation 落库 -> 注销回读。
 * 数据全部写在 /tmp 临时工作区，不碰正式 data/roll_call_data。
 * 用法：仓库根目录执行 ./build/build_rk3588_linux/test_rollcall
 * 退出码：0=PASS，1=FAIL（stdout 给出失败环节）。
 */
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "face_recognizer.h"
#include "in_process_face_recognizer.h"
#include "roll_call_service.h"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::cout << "  [ok]   " << what << std::endl;
    } else {
        std::cout << "  [FAIL] " << what << std::endl;
        ++g_failures;
    }
}

double bboxIou(const cv::Rect& a, const cv::Rect& b) {
    const cv::Rect inter = a & b;
    const double inter_area = inter.area();
    const double denom = (double)a.area() + (double)b.area() - inter_area;
    return denom > 0.0 ? inter_area / denom : 0.0;
}

} // namespace

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    const QString workspace = QDir::currentPath();
    const QString test_image = workspace + "/assets/test/test.jpg";
    const QString face_dir = workspace + "/model/face";
    std::cout << "[test_rollcall] workspace=" << workspace.toStdString() << std::endl;

    if (!QFileInfo::exists(test_image) ||
        !QFileInfo::exists(face_dir + "/detection.rknn") ||
        !QFileInfo::exists(face_dir + "/recognition.rknn") ||
        !QFileInfo::exists(face_dir + "/face_recognition")) {
        std::cout << "  [FAIL] 前置资源缺失：需在仓库根运行，且 model/face 下备齐"
                     "检测/识别权重与 face_recognition 对照工具" << std::endl;
        return 1;
    }

    // 临时工作区：db、任务目录与 test.jpg 副本都在 /tmp，跑完即删。
    // 副本同时隔离 exe 的 <image>_faces.json 残留（不污染 assets/）
    const QString tmp_root = QStringLiteral("/tmp/rollcall_check_%1").arg(QCoreApplication::applicationPid());
    QDir().mkpath(tmp_root);
    const QString image_copy = tmp_root + "/test.jpg";
    QFile::copy(test_image, image_copy);
    std::cout << "[test_rollcall] tmp workspace=" << tmp_root.toStdString() << std::endl;

    const std::string img = image_copy.toUtf8().toStdString();
    const float det_th = 0.6f, nms_th = 0.4f;   // 与 RollCallService::initialize 内同一阈值

    // =========================================================================
    // A. 对照回归：外部 exe vs 进程内识别器
    // =========================================================================
    FaceRecognitionWrapper exe_wrapper;
    exe_wrapper.setExecutablePath((face_dir + "/face_recognition").toUtf8().toStdString());
    exe_wrapper.setDetectionModel((face_dir + "/detection.rknn").toUtf8().toStdString());
    exe_wrapper.setRecognitionModel((face_dir + "/recognition.rknn").toUtf8().toStdString());
    exe_wrapper.setThresholds(det_th, nms_th);
    const FaceDetectionResult ref = exe_wrapper.detectAndExtract(img);

    InProcessFaceRecognizer recognizer;
    check(recognizer.init((face_dir + "/detection.rknn").toStdString(),
                          (face_dir + "/recognition.rknn").toStdString(),
                          RKNN_NPU_CORE_AUTO),
          "InProcessFaceRecognizer::init");
    recognizer.setThresholds(det_th, nms_th);
    const FaceDetectionResult cur = recognizer.detectAndExtract(img);

    check(!ref.faces.empty() && ref.faces.size() == cur.faces.size(),
          "人脸数与外部 exe 一致（exe=" + std::to_string(ref.faces.size()) +
          ", 进程内=" + std::to_string(cur.faces.size()) + "）");
    if (g_failures) { std::cout << "FAIL: 推理链未通过，终止" << std::endl;
                     QDir(tmp_root).removeRecursively(); return 1; }

    // 按 IoU 贪心配对（两侧同为 score 降序，理论上同序号即同脸；配对口径更稳，
    // 且能同时校验框与特征）：每对断言 IoU>0.9 且特征余弦>0.99
    std::vector<bool> used(cur.faces.size(), false);
    int paired = 0;
    for (const auto& r : ref.faces) {
        int best = -1;
        double best_iou = 0.9;   // 以配对下限为初值：单遍扫描同时完成取最大与过滤
        for (size_t j = 0; j < cur.faces.size(); ++j) {
            if (used[j]) continue;
            const double iou = bboxIou(r.bbox, cur.faces[j].bbox);
            if (iou > best_iou) { best_iou = iou; best = (int)j; }
        }
        if (best < 0) continue;
        used[best] = true;
        ++paired;
        const float sim = FaceRecognitionWrapper::calculateSimilarity(
            r.feature, cur.faces[best].feature);
        std::cout << "    pair[" << paired << "] iou=" << best_iou
                  << " cosine=" << sim << std::endl;
        check(sim > 0.99f, "同名脸特征余弦 > 0.99（exe vs 进程内）");
    }
    check(paired == (int)ref.faces.size(), "每张 exe 检出脸都能在进程内结果中同名配对");

    // =========================================================================
    // B. 业务全链（RollCallService，进程内识别器）
    // =========================================================================
    RollCallService service;
    check(service.initialize((face_dir + "/detection.rknn").toStdString(),
                             (face_dir + "/recognition.rknn").toStdString(),
                             (tmp_root + "/roll_call.db").toUtf8().toStdString(),
                             (tmp_root + "/roll_call_data").toUtf8().toStdString()),
          "RollCallService::initialize");
    if (g_failures) { std::cout << "FAIL: 服务初始化未通过，终止" << std::endl;
                     QDir(tmp_root).removeRecursively(); return 1; }

    // 1. 注册：单图检测 + 落库 + 回读
    const int task_id = service.createRegistrationTask("点名离线自测");
    check(task_id > 0, "createRegistrationTask");
    const auto reg = service.processPhotos(task_id, {img});
    const int expected_faces = (int)ref.faces.size();
    check(reg.total_unique_count == expected_faces,
          "processPhotos 唯一人脸数 = 对照基准（" + std::to_string(expected_faces) + "）");
    if (!reg.photos.empty()) {
        const auto& photo = reg.photos.front();
        check(!photo.processed_path.empty() &&
              QFileInfo(QString::fromUtf8(photo.processed_path.c_str())).size() > 0,
              "processed_*.png 画框图生成且非空");
        check((int)photo.face_image_paths.size() == expected_faces,
              "人脸裁剪图逐唯一脸落盘");
    }
    check(service.saveTaskResult(reg), "saveTaskResult 落库");
    auto unique_in_db = service.getDatabase()->getUniqueFacesByTask(task_id);
    check((int)unique_in_db.size() == expected_faces &&
          service.getTaskInfo(task_id).registered_count == expected_faces,
          "回读：库内唯一记录数与任务 registered_count 一致");

    // 2. 跨照片去重：同图两张 -> 第二张全部判重、唯一数不变
    const auto dup = service.processPhotos(task_id, {img, img});
    bool second_all_dup = dup.photos.size() == 2;
    if (second_all_dup) {
        for (const auto& f : dup.photos[1].faces) second_all_dup = second_all_dup && f.is_duplicate;
        second_all_dup = second_all_dup && dup.photos[1].unique_count == 0;
    }
    check(second_all_dup && dup.total_unique_count == expected_faces,
          "重复照片全局去重：第二张全判重、唯一总数不变");

    // 3. 注销比对：同图即全体在册人员现身 -> 一对一全命中
    const auto match = service.matchCancellation(task_id, {img});
    int matched = 0, cancelled_matched = 0;
    float min_similarity = 1.0f;
    for (const auto& m : match.matches) {
        if (m.status == 1) { ++matched; ++cancelled_matched; min_similarity = std::min(min_similarity, m.similarity); }
    }
    check(matched == expected_faces, "matchCancellation 一对一全命中（status=1）");
    check(min_similarity > 0.99f, "命中相似度≈1（同人同照，实测最低 " + std::to_string(min_similarity) + "）");

    // 4. 注销确认落库 + 回读
    check(service.confirmCancellation(task_id, cancelled_matched, match), "confirmCancellation 落库");
    const Task after = service.getTaskInfo(task_id);
    check(after.is_cancelled == 1 && after.cancelled_count == expected_faces,
          "回读：任务 is_cancelled=1、cancelled_count 与命中数一致");
    const auto matches_in_db = service.getDatabase()->getCancellationMatchesByTask(task_id);
    check((int)matches_in_db.size() == (int)match.matches.size(),
          "回读：cancellation_matches 行数与比对结果一致");

    QDir(tmp_root).removeRecursively();
    if (g_failures == 0) { std::cout << "PASS" << std::endl; return 0; }
    std::cout << "FAIL: " << g_failures << " 项未通过" << std::endl;
    return 1;
}
