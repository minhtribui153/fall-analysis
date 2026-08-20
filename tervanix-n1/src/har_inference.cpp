#include "har_inference.hpp"

#if defined(TERVANIX_BOARD_NESSO_N1) || defined(TERVANIX_BOARD_ESP32_DEV)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace
{

    constexpr char TAG[] = "har";
    constexpr int MODEL_N = 50;
    constexpr int AXES = 8;
    constexpr int kOpCount = 9;
    constexpr int MAX_CAPTURE_SAMPLES = 64;
    constexpr std::size_t ARENA_BYTES = 40 * 1024;

    alignas(16) std::uint8_t arena[ARENA_BYTES]{};
    float capture[MAX_CAPTURE_SAMPLES][AXES]{};

    int capture_count = 0;
    int capture_head = 0;
    int required_capture_samples = 38;
    int sample_period_ms = 40;
    int prediction_interval_ms = 1000;
    int acquisition_window_ms = 1500;

    std::int64_t last_accepted_ms = -1;
    std::int64_t last_prediction_ms = -1;
    std::uint64_t prediction_sequence = 0;

    const tflite::Model *tf_model = nullptr;
    tflite::MicroInterpreter *interpreter = nullptr;
    TfLiteTensor *input = nullptr;
    TfLiteTensor *output = nullptr;

    float threshold = 0.60f;
    char model_sha256[65]{};
    int class_count = 0;
    char classes[tervanix::model::MAX_CLASSES]
                [tervanix::model::MAX_CLASS_NAME]{};
    bool initialized = false;

    const char *normalize_class(const char *value)
    {
        if (std::strcmp(value, "FFH") == 0)
        {
            return "falling_from_height";
        }
        if (std::strcmp(value, "nearmiss") == 0)
        {
            return "near_miss";
        }
        if (std::strcmp(value, "unlabeled") == 0)
        {
            return "unknown";
        }
        return value;
    }

    const char *fallback(float accel_magnitude, float gyro_magnitude)
    {
        if (accel_magnitude <= 1.05f && gyro_magnitude <= 15.0f)
        {
            return "stationary";
        }
        if (accel_magnitude > 2.3f && gyro_magnitude > 150.0f)
        {
            return "falling_from_height";
        }
        if (gyro_magnitude > 250.0f)
        {
            return "trip";
        }
        if (accel_magnitude > 2.4f)
        {
            return "jumping";
        }
        if (accel_magnitude > 1.2f && accel_magnitude <= 2.0f &&
            gyro_magnitude < 150.0f)
        {
            return "walking";
        }
        return "unknown";
    }

    int tensor_element_count(const TfLiteTensor *tensor)
    {
        if (!tensor || !tensor->dims)
        {
            return 0;
        }

        int count = 1;
        for (int i = 0; i < tensor->dims->size; ++i)
        {
            count *= tensor->dims->data[i];
        }
        return count;
    }

    const char *tensor_type_name(TfLiteType type)
    {
        switch (type)
        {
        case kTfLiteFloat32:
            return "float32";
        case kTfLiteInt8:
            return "int8";
        case kTfLiteUInt8:
            return "uint8";
        case kTfLiteInt16:
            return "int16";
        case kTfLiteInt32:
            return "int32";
        default:
            return "other";
        }
    }

    void log_tensor(const char *name, const TfLiteTensor *tensor)
    {
        if (!tensor)
        {
            ESP_LOGE(TAG, "%s tensor is null", name);
            return;
        }

        char dims[80]{};
        int used = 0;

        if (tensor->dims)
        {
            for (int i = 0; i < tensor->dims->size; ++i)
            {
                const int written = std::snprintf(
                    dims + used,
                    sizeof(dims) - static_cast<std::size_t>(used),
                    "%s%d",
                    i == 0 ? "[" : ",",
                    tensor->dims->data[i]);

                if (written <= 0)
                {
                    break;
                }

                used += written;
                if (used >= static_cast<int>(sizeof(dims)) - 2)
                {
                    break;
                }
            }

            if (used < static_cast<int>(sizeof(dims)) - 1)
            {
                dims[used++] = ']';
                dims[used] = '\0';
            }
        } else
        {
            std::strcpy(dims, "[]");
        }

        ESP_LOGI(
            TAG,
            "%s tensor: type=%s(%d) dims=%s elements=%d bytes=%u "
            "scale=%.9g zero_point=%ld",
            name,
            tensor_type_name(tensor->type),
            static_cast<int>(tensor->type),
            dims,
            tensor_element_count(tensor),
            static_cast<unsigned>(tensor->bytes),
            static_cast<double>(tensor->params.scale),
            static_cast<long>(tensor->params.zero_point));
    }

    bool supported_input_type(TfLiteType type)
    {
        return type == kTfLiteInt8 || type == kTfLiteUInt8 ||
               type == kTfLiteFloat32;
    }

    bool supported_output_type(TfLiteType type)
    {
        return type == kTfLiteInt8 || type == kTfLiteUInt8 ||
               type == kTfLiteFloat32;
    }

    bool write_input_value(int index, float value)
    {
        if (!input || index < 0 || index >= tensor_element_count(input))
        {
            return false;
        }

        switch (input->type)
        {
        case kTfLiteFloat32:
            input->data.f[index] = value;
            return true;

        case kTfLiteInt8: {
            const float scale =
                input->params.scale == 0.0f ? 1.0f : input->params.scale;

            const int quantized = static_cast<int>(std::lround(value / scale)) +
                                  input->params.zero_point;

            input->data.int8[index] =
                static_cast<std::int8_t>(std::clamp(quantized, -128, 127));
            return true;
        }

        case kTfLiteUInt8: {
            const float scale =
                input->params.scale == 0.0f ? 1.0f : input->params.scale;

            const int quantized = static_cast<int>(std::lround(value / scale)) +
                                  input->params.zero_point;

            input->data.uint8[index] =
                static_cast<std::uint8_t>(std::clamp(quantized, 0, 255));
            return true;
        }

        default:
            return false;
        }
    }

    float read_output_value(int index)
    {
        if (!output || index < 0 || index >= tensor_element_count(output))
        {
            return -1000.0f;
        }

        switch (output->type)
        {
        case kTfLiteFloat32:
            return output->data.f[index];

        case kTfLiteInt8:
            return (static_cast<int>(output->data.int8[index]) -
                    output->params.zero_point) *
                   output->params.scale;

        case kTfLiteUInt8:
            return (static_cast<int>(output->data.uint8[index]) -
                    output->params.zero_point) *
                   output->params.scale;

        default:
            return -1000.0f;
        }
    }

    int chronological_index(int logical_index)
    {
        // capture_head always points at the next insertion position.  Before
        // the ring is full, samples are simply 0..capture_count-1.
        if (capture_count < required_capture_samples)
        {
            return logical_index;
        }

        const int oldest =
            (capture_head - required_capture_samples + MAX_CAPTURE_SAMPLES) %
            MAX_CAPTURE_SAMPLES;

        return (oldest + logical_index) % MAX_CAPTURE_SAMPLES;
    }

    bool resample_capture_into_model(float &max_accel, float &max_gyro)
    {
        max_accel = 0.0f;
        max_gyro = 0.0f;

        // The deployed model has a fixed [50,8] input.  Runtime acquisition is
        // intentionally 1.5 s at 25 Hz (= 38 real samples).  We linearly
        // interpolate those 38 chronological samples into the model's 50 rows,
        // preserving the requested acquisition duration without changing the
        // model tensor shape.
        int tensor_index = 0;

        for (int out_row = 0; out_row < MODEL_N; ++out_row)
        {
            const float position =
                (required_capture_samples <= 1)
                    ? 0.0f
                    : static_cast<float>(out_row) *
                          static_cast<float>(required_capture_samples - 1) /
                          static_cast<float>(MODEL_N - 1);

            const int left = static_cast<int>(std::floor(position));
            const int right = std::min(left + 1, required_capture_samples - 1);
            const float alpha = position - static_cast<float>(left);

            const int left_index = chronological_index(left);
            const int right_index = chronological_index(right);

            float row[AXES]{};
            for (int axis = 0; axis < AXES; ++axis)
            {
                row[axis] = capture[left_index][axis] * (1.0f - alpha) +
                            capture[right_index][axis] * alpha;
                if (!write_input_value(tensor_index++, row[axis]))
                {
                    ESP_LOGE(TAG, "Failed to write HAR input tensor");
                    return false;
                }
            }

            max_accel = std::max(max_accel, row[6]);
            max_gyro = std::max(max_gyro, row[7]);
        }

        return true;
    }

} // namespace

namespace tervanix::har
{

    esp_err_t initialize(const model::Image &image)
    {
        if (!image.data || !image.size)
        {
            return ESP_ERR_INVALID_ARG;
        }

        if (image.sample_rate_hz <= 0 || image.window_samples != MODEL_N ||
            image.num_axes != AXES)
        {
            ESP_LOGE(
                TAG,
                "Unsupported model tensor config rate=%d window=%d axes=%d",
                image.sample_rate_hz,
                image.window_samples,
                image.num_axes);
            return ESP_ERR_NOT_SUPPORTED;
        }

        if (image.inference_sample_period_ms < 10 ||
            image.inference_sample_period_ms > 1000 ||
            image.inference_window_ms < image.inference_sample_period_ms ||
            image.prediction_interval_ms < image.inference_sample_period_ms)
        {
            ESP_LOGE(TAG, "Invalid signed inference timing configuration");
            return ESP_ERR_INVALID_ARG;
        }

        sample_period_ms = image.inference_sample_period_ms;
        acquisition_window_ms = image.inference_window_ms;
        prediction_interval_ms = image.prediction_interval_ms;

        // ceil(window / period): 1500 / 40 => 38 samples.
        required_capture_samples =
            (image.inference_window_ms + sample_period_ms - 1) /
            sample_period_ms;

        if (required_capture_samples < 2 ||
            required_capture_samples > MAX_CAPTURE_SAMPLES)
        {
            ESP_LOGE(
                TAG,
                "Capture window needs %d samples; supported range is 2..%d",
                required_capture_samples,
                MAX_CAPTURE_SAMPLES);
            return ESP_ERR_NOT_SUPPORTED;
        }

        tf_model = tflite::GetModel(image.data);
        if (!tf_model || tf_model->version() != TFLITE_SCHEMA_VERSION)
        {
            return ESP_ERR_INVALID_VERSION;
        }

        static tflite::MicroMutableOpResolver<kOpCount> resolver;
        static bool resolver_ready = false;

        if (!resolver_ready)
        {
            if (resolver.AddExpandDims() != kTfLiteOk ||
                resolver.AddConv2D() != kTfLiteOk ||
                resolver.AddReshape() != kTfLiteOk ||
                resolver.AddMaxPool2D() != kTfLiteOk ||
                resolver.AddShape() != kTfLiteOk ||
                resolver.AddStridedSlice() != kTfLiteOk ||
                resolver.AddPack() != kTfLiteOk ||
                resolver.AddFullyConnected() != kTfLiteOk ||
                resolver.AddSoftmax() != kTfLiteOk)
            {
                ESP_LOGE(
                    TAG, "Failed to register required TFLite Micro operators");
                return ESP_FAIL;
            }
            resolver_ready = true;
        }

        static tflite::MicroInterpreter static_interpreter(
            tf_model, resolver, arena, ARENA_BYTES);
        interpreter = &static_interpreter;

        if (interpreter->AllocateTensors() != kTfLiteOk)
        {
            ESP_LOGE(
                TAG,
                "AllocateTensors failed; arena=%u bytes",
                static_cast<unsigned>(ARENA_BYTES));
            return ESP_ERR_NO_MEM;
        }

        input = interpreter->input(0);
        output = interpreter->output(0);

        log_tensor("input", input);
        log_tensor("output", output);

        const int input_elements = tensor_element_count(input);
        const int output_elements = tensor_element_count(output);

        if (!input || !output || !supported_input_type(input->type) ||
            !supported_output_type(output->type) ||
            input_elements < MODEL_N * AXES || image.class_count <= 0 ||
            image.class_count > tervanix::model::MAX_CLASSES ||
            output_elements != image.class_count)
        {
            ESP_LOGE(
                TAG,
                "HAR model/manifest mismatch: input_elements=%d required=%d "
                "output_elements=%d manifest_classes=%d",
                input_elements,
                MODEL_N * AXES,
                output_elements,
                image.class_count);
            ESP_LOGE(
                TAG,
                "TFLite output semantics cannot be inferred safely; "
                "the signed manifest must contain exactly one class label "
                "per output element.");
            return ESP_ERR_INVALID_SIZE;
        }

        capture_count = 0;
        capture_head = 0;
        last_accepted_ms = -1;
        last_prediction_ms = -1;
        prediction_sequence = 0;
        threshold = image.min_confidence;
        class_count = image.class_count;

        for (int i = 0; i < class_count; ++i)
        {
            std::strncpy(
                classes[i],
                image.classes[i],
                tervanix::model::MAX_CLASS_NAME - 1);
            classes[i][tervanix::model::MAX_CLASS_NAME - 1] = '\0';
        }

        std::strncpy(model_sha256, image.sha256, sizeof(model_sha256) - 1);
        model_sha256[sizeof(model_sha256) - 1] = '\0';

        initialized = true;

        for (int i = 0; i < class_count; ++i)
        {
            ESP_LOGI(TAG, "HAR class[%d]=%s", i, classes[i]);
        }

        ESP_LOGI(
            TAG,
            "HAR ready: acquisition=%d ms @ %d ms/sample (%d real samples), "
            "model_window=%d, classes=%d, predict_every=%d ms, ops=%d",
            image.inference_window_ms,
            sample_period_ms,
            required_capture_samples,
            MODEL_N,
            class_count,
            prediction_interval_ms,
            kOpCount);
        return ESP_OK;
    }

    bool add_sample(
        const sensor::ImuSample &sample,
        std::int64_t source_uptime_ms,
        Prediction &out)
    {
        if (!initialized)
        {
            return false;
        }

        if (last_accepted_ms >= 0 &&
            source_uptime_ms - last_accepted_ms < sample_period_ms)
        {
            return false;
        }
        last_accepted_ms = source_uptime_ms;

        const float accel_magnitude = std::sqrt(
            sample.ax * sample.ax + sample.ay * sample.ay +
            sample.az * sample.az);
        const float gyro_magnitude = std::sqrt(
            sample.gx * sample.gx + sample.gy * sample.gy +
            sample.gz * sample.gz);

        float *row = capture[capture_head];
        row[0] = sample.ax;
        row[1] = sample.ay;
        row[2] = sample.az;
        row[3] = sample.gx;
        row[4] = sample.gy;
        row[5] = sample.gz;
        row[6] = accel_magnitude;
        row[7] = gyro_magnitude;

        capture_head = (capture_head + 1) % MAX_CAPTURE_SAMPLES;
        capture_count = std::min(capture_count + 1, required_capture_samples);

        if (capture_count < required_capture_samples)
        {
            return false;
        }

        if (last_prediction_ms >= 0 &&
            source_uptime_ms - last_prediction_ms < prediction_interval_ms)
        {
            return false;
        }

        float max_accel = 0.0f;
        float max_gyro = 0.0f;
        if (!resample_capture_into_model(max_accel, max_gyro))
        {
            return false;
        }

        if (interpreter->Invoke() != kTfLiteOk)
        {
            ESP_LOGE(TAG, "TFLite Invoke failed");
            return false;
        }

        int best = 0;
        float probability = -1000.0f;

        for (int i = 0; i < class_count; ++i)
        {
            const float candidate = read_output_value(i);

            if (candidate > probability)
            {
                probability = candidate;
                best = i;
            }
        }

        const char *state = normalize_class(classes[best]);
        if (probability < threshold)
        {
            state = fallback(max_accel, max_gyro);
        }

        out = {};
        out.prediction_sequence = ++prediction_sequence;
        out.source_uptime_ms = source_uptime_ms;
        out.confidence = probability;

        std::strncpy(out.state, state, sizeof(out.state) - 1);
        std::strncpy(
            out.model_sha256, model_sha256, sizeof(out.model_sha256) - 1);

        last_prediction_ms = source_uptime_ms;

        ESP_LOGI(
            TAG,
            "prediction #%llu %s %.3f after %d real samples / %d ms",
            static_cast<unsigned long long>(out.prediction_sequence),
            out.state,
            out.confidence,
            required_capture_samples,
            acquisition_window_ms);
        return true;
    }

    bool ready() { return initialized; }

} // namespace tervanix::har

#endif
