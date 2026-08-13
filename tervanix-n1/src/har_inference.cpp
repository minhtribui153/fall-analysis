#include "har_inference.hpp"
#if defined(TERVANIX_BOARD_NESSO_N1)
#include "esp_log.h"
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace
{
    constexpr char TAG[] = "har";
    constexpr int N = 50, A = 8, SLIDE = 25, C = 9;
    constexpr std::size_t ARENA = 40 * 1024;
    const char *CLASSES[C] = {
        "FFH",
        "jumping",
        "nearmiss",
        "running",
        "slip",
        "stationary",
        "trip",
        "unlabeled",
        "walking"};
    alignas(16) std::uint8_t arena[ARENA]{};
    float buf[N][A]{};
    int idx = 0;
    std::int64_t last_ms = -1;
    std::uint64_t seq = 0;
    const tflite::Model *model = nullptr;
    tflite::MicroInterpreter *interp = nullptr;
    TfLiteTensor *input = nullptr;
    TfLiteTensor *output = nullptr;
    float threshold = .60f;
    char sha[65]{};
    bool ok = false;
    const char *norm(const char *s)
    {
        if (std::strcmp(s, "FFH") == 0)
            return "falling_from_height";
        if (std::strcmp(s, "nearmiss") == 0)
            return "near_miss";
        if (std::strcmp(s, "unlabeled") == 0)
            return "unknown";
        return s;
    }
    const char *fallback(float am, float gm)
    {
        if (am <= 1.05f && gm <= 15.f)
            return "stationary";
        if (am > 2.3f && gm > 150.f)
            return "falling_from_height";
        if (gm > 250.f)
            return "trip";
        if (am > 2.4f)
            return "jumping";
        if (am > 1.2f && am <= 2.f && gm < 150.f)
            return "walking";
        return "unknown";
    }
    std::int8_t q(float v, const TfLiteTensor *t)
    {
        float s = t->params.scale == 0 ? 1.f : t->params.scale;
        int x = (int)std::lround(v / s) + t->params.zero_point;
        return (std::int8_t)std::clamp(x, -128, 127);
    }
} // namespace
namespace tervanix::har
{
    esp_err_t initialize(const model::Image &image)
    {
        if (!image.data || !image.size)
            return ESP_ERR_INVALID_ARG;
        if (image.sample_rate_hz != 25 || image.window_samples != N ||
            image.slide_samples != SLIDE || image.num_axes != A)
            return ESP_ERR_NOT_SUPPORTED;
        model = tflite::GetModel(image.data);
        if (!model || model->version() != TFLITE_SCHEMA_VERSION)
            return ESP_ERR_INVALID_VERSION;
        static tflite::AllOpsResolver resolver;
        static tflite::MicroInterpreter static_interp(
            model, resolver, arena, ARENA);
        interp = &static_interp;
        if (interp->AllocateTensors() != kTfLiteOk)
            return ESP_ERR_NO_MEM;
        input = interp->input(0);
        output = interp->output(0);
        if (!input || !output || input->type != kTfLiteInt8 ||
            output->type != kTfLiteInt8 || input->bytes < N * A ||
            output->bytes < C)
            return ESP_ERR_INVALID_SIZE;
        idx = 0;
        last_ms = -1;
        seq = 0;
        threshold = image.min_confidence;
        std::strncpy(sha, image.sha256, sizeof(sha) - 1);
        ok = true;
        ESP_LOGI(TAG, "TFLite HAR ready 25Hz window=50 slide=25 model=%s", sha);
        return ESP_OK;
    }
    bool
    add_sample(const sensor::ImuSample &s, std::int64_t ms, Prediction &out)
    {
        if (!ok)
            return false;
        if (last_ms >= 0 && ms - last_ms < 40)
            return false;
        last_ms = ms;
        float am = std::sqrt(s.ax * s.ax + s.ay * s.ay + s.az * s.az),
              gm = std::sqrt(s.gx * s.gx + s.gy * s.gy + s.gz * s.gz);
        float *r = buf[idx];
        r[0] = s.ax;
        r[1] = s.ay;
        r[2] = s.az;
        r[3] = s.gx;
        r[4] = s.gy;
        r[5] = s.gz;
        r[6] = am;
        r[7] = gm;
        ++idx;
        if (idx < N)
            return false;
        int ti = 0;
        float amax = 0, gmax = 0;
        for (int i = 0; i < N; ++i)
        {
            amax = std::max(amax, buf[i][6]);
            gmax = std::max(gmax, buf[i][7]);
            for (int j = 0; j < A; ++j)
                input->data.int8[ti++] = q(buf[i][j], input);
        }
        if (interp->Invoke() != kTfLiteOk)
        {
            idx = 0;
            return false;
        }
        int best = 0;
        float prob = -1000.f;
        for (int i = 0; i < C; ++i)
        {
            float p = (static_cast<int>(output->data.int8[i]) -
                       output->params.zero_point) *
                      output->params.scale;
            if (p > prob)
            {
                prob = p;
                best = i;
            }
        }
        const char *state = norm(CLASSES[best]);
        if (prob < threshold)
            state = fallback(amax, gmax);
        out = {};
        out.prediction_sequence = ++seq;
        out.source_uptime_ms = ms;
        std::strncpy(out.state, state, sizeof(out.state) - 1);
        out.confidence = prob;
        std::strncpy(out.model_sha256, sha, sizeof(out.model_sha256) - 1);
        for (int i = 0; i < N - SLIDE; ++i)
            std::memcpy(buf[i], buf[i + SLIDE], sizeof(buf[i]));
        idx = N - SLIDE;
        ESP_LOGI(
            TAG,
            "prediction #%llu %s %.3f",
            (unsigned long long)out.prediction_sequence,
            out.state,
            out.confidence);
        return true;
    }
    bool ready() { return ok; }
} // namespace tervanix::har
#endif
