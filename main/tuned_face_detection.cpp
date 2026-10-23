#include "tuned_face_detection.h"

#include <atomic>
#include <list>

#include "dl_detect_define.hpp"
#include "esp_log.h"
#include "human_face_detect_mnp01.hpp"
#include "human_face_detect_msr01.hpp"
#include "who_ai_utils.hpp"

namespace {

// Slightly lower confidence cutoffs admit more marginal face detections.
constexpr float STAGE_ONE_SCORE_THRESHOLD = 0.18F;
constexpr float STAGE_ONE_NMS_THRESHOLD = 0.30F;
constexpr int STAGE_ONE_TOP_K = 10;
constexpr float STAGE_ONE_RESIZE_SCALE = 0.50F;

constexpr float STAGE_TWO_SCORE_THRESHOLD = 0.28F;
constexpr float STAGE_TWO_NMS_THRESHOLD = 0.30F;
constexpr int STAGE_TWO_TOP_K = 10;

const char *TAG = "tuned_face_detection";
std::atomic<bool> s_face_detected{false};
HumanFaceDetectMSR01 *s_stage_one = nullptr;
HumanFaceDetectMNP01 *s_stage_two = nullptr;

} // namespace

bool tuned_face_detect(camera_fb_t *frame)
{
    if (!frame) {
        return false;
    }
    if (!s_stage_one) {
        // Created once, on first use, and kept for later sensor sessions.
        s_stage_one = new HumanFaceDetectMSR01(STAGE_ONE_SCORE_THRESHOLD,
                                               STAGE_ONE_NMS_THRESHOLD,
                                               STAGE_ONE_TOP_K,
                                               STAGE_ONE_RESIZE_SCALE);
        s_stage_two = new HumanFaceDetectMNP01(STAGE_TWO_SCORE_THRESHOLD,
                                               STAGE_TWO_NMS_THRESHOLD,
                                               STAGE_TWO_TOP_K);
        ESP_LOGI(TAG, "Thresholds: stage1 %.2f, stage2 %.2f; resize scale %.2f",
                 STAGE_ONE_SCORE_THRESHOLD, STAGE_TWO_SCORE_THRESHOLD,
                 STAGE_ONE_RESIZE_SCALE);
    }

    std::list<dl::detect::result_t> &candidates = s_stage_one->infer(
        reinterpret_cast<uint16_t *>(frame->buf),
        {static_cast<int>(frame->height), static_cast<int>(frame->width), 3});
    std::list<dl::detect::result_t> &results = s_stage_two->infer(
        reinterpret_cast<uint16_t *>(frame->buf),
        {static_cast<int>(frame->height), static_cast<int>(frame->width), 3},
        candidates);

    if (results.empty()) {
        return false;
    }
    draw_detection_result(reinterpret_cast<uint16_t *>(frame->buf),
                          frame->height, frame->width, results);
    print_detection_result(results);
    s_face_detected.store(true, std::memory_order_release);
    return true;
}

bool get_tuned_face_detected()
{
    return s_face_detected.exchange(false, std::memory_order_acq_rel);
}
