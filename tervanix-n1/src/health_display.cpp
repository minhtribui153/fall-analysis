#include "health_display.hpp"

#if defined(TERVANIX_BOARD_NESSO_N1)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nesso_board_io.hpp"

namespace
{

    constexpr char TAG[] = "health-ui";

    constexpr int LCD_WIDTH = 135;
    constexpr int LCD_HEIGHT = 240;
    constexpr int LCD_MEMORY_OFFSET_X = 52;
    constexpr int LCD_MEMORY_OFFSET_Y = 40;
    constexpr gpio_num_t LCD_MOSI = GPIO_NUM_21;
    constexpr gpio_num_t LCD_SCLK = GPIO_NUM_20;
    constexpr gpio_num_t LCD_CS = GPIO_NUM_17;
    constexpr gpio_num_t LCD_DC = GPIO_NUM_16;
    constexpr int LCD_SPI_HZ = 40 * 1000 * 1000;
    constexpr spi_host_device_t LCD_SPI_HOST = SPI2_HOST;

    constexpr std::uint16_t BLACK = 0x0000;
    constexpr std::uint16_t WHITE = 0xFFFF;
    constexpr std::uint16_t DIM = 0x7BEF;
    constexpr std::uint16_t CYAN = 0x07FF;
    constexpr std::uint16_t GREEN = 0x07E0;
    constexpr std::uint16_t YELLOW = 0xFFE0;
    constexpr std::uint16_t RED = 0xF800;
    constexpr std::uint16_t BLUE = 0x001F;

    constexpr int PAGE_COUNT = 4;
    constexpr int BATTERY_REFRESH_MS = 5000;
    constexpr int KEY_POLL_MS = 50;

    esp_lcd_panel_io_handle_t lcd_io = nullptr;
    esp_lcd_panel_handle_t lcd_panel = nullptr;
    bool display_ready = false;

    std::atomic<int> active_page{0};
    std::atomic<int> battery_percent{-1};
    std::atomic<int> charge_status{
        static_cast<int>(tervanix::board::ChargeStatus::Unknown)};
    std::atomic<bool> controls_running{false};

    std::uint16_t framebuffer[LCD_WIDTH * LCD_HEIGHT]{};

    struct Glyph {
        char c;
        std::uint16_t bits;
    };

    constexpr std::uint16_t G(const char *rows)
    {
        std::uint16_t value = 0;
        for (int i = 0; i < 15; ++i)
        {
            value <<= 1;
            if (rows[i] == '1')
            {
                value |= 1;
            }
        }
        return value;
    }

    constexpr Glyph FONT[] = {
        {'0', G("111101101101111")},
        {'1', G("010110010010111")},
        {'2', G("111001111100111")},
        {'3', G("111001111001111")},
        {'4', G("101101111001001")},
        {'5', G("111100111001111")},
        {'6', G("111100111101111")},
        {'7', G("111001001001001")},
        {'8', G("111101111101111")},
        {'9', G("111101111001111")},
        {'A', G("010101111101101")},
        {'B', G("110101110101110")},
        {'C', G("011100100100011")},
        {'D', G("110101101101110")},
        {'E', G("111100110100111")},
        {'F', G("111100110100100")},
        {'G', G("011100101101011")},
        {'H', G("101101111101101")},
        {'I', G("111010010010111")},
        {'J', G("001001001101010")},
        {'K', G("101101110101101")},
        {'L', G("100100100100111")},
        {'M', G("101111111101101")},
        {'N', G("101111111111101")},
        {'O', G("010101101101010")},
        {'P', G("110101110100100")},
        {'Q', G("010101101111011")},
        {'R', G("110101110101101")},
        {'S', G("011100010001110")},
        {'T', G("111010010010010")},
        {'U', G("101101101101111")},
        {'V', G("101101101101010")},
        {'W', G("101101111111101")},
        {'X', G("101101010101101")},
        {'Y', G("101101010010010")},
        {'Z', G("111001010100111")},
        {':', G("000010000010000")},
        {'-', G("000000111000000")},
        {'.', G("000000000000010")},
        {'/', G("001001010100100")},
        {'_', G("000000000000111")},
        {'%', G("101001010100101")},
        {' ', 0},
    };

    std::uint16_t glyph_bits(char c)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        for (const auto &glyph : FONT)
        {
            if (glyph.c == c)
            {
                return glyph.bits;
            }
        }
        return 0;
    }

    void clear(std::uint16_t color)
    {
        std::fill(std::begin(framebuffer), std::end(framebuffer), color);
    }

    void fill_rect(int x, int y, int w, int h, std::uint16_t color)
    {
        const int x0 = std::max(0, x);
        const int y0 = std::max(0, y);
        const int x1 = std::min(LCD_WIDTH, x + w);
        const int y1 = std::min(LCD_HEIGHT, y + h);

        for (int yy = y0; yy < y1; ++yy)
        {
            std::fill(
                framebuffer + yy * LCD_WIDTH + x0,
                framebuffer + yy * LCD_WIDTH + x1,
                color);
        }
    }

    void rect(int x, int y, int w, int h, std::uint16_t color)
    {
        fill_rect(x, y, w, 1, color);
        fill_rect(x, y + h - 1, w, 1, color);
        fill_rect(x, y, 1, h, color);
        fill_rect(x + w - 1, y, 1, h, color);
    }

    void draw_char(int x, int y, char c, std::uint16_t color, int scale)
    {
        const std::uint16_t bits = glyph_bits(c);

        for (int row = 0; row < 5; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                const int bit_index = 14 - (row * 3 + col);
                if ((bits >> bit_index) & 1U)
                {
                    fill_rect(
                        x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }

    void
    text(int x, int y, const char *value, std::uint16_t color, int scale = 2)
    {
        if (value == nullptr || value[0] == '\0')
        {
            value = "-";
        }

        const int advance = 4 * scale;
        while (*value != '\0' && x <= LCD_WIDTH - (3 * scale))
        {
            draw_char(x, y, *value, color, scale);
            x += advance;
            ++value;
        }
    }

    void centered_text(int y, const char *value, std::uint16_t color, int scale)
    {
        if (!value)
        {
            value = "-";
        }

        const int length = static_cast<int>(std::strlen(value));
        const int width = std::max(0, length * 4 * scale - scale);
        text(std::max(2, (LCD_WIDTH - width) / 2), y, value, color, scale);
    }

    void split_text(
        int y,
        const char *value,
        int chars_per_line,
        std::uint16_t color,
        int scale = 2)
    {
        if (!value || !value[0])
        {
            text(4, y, "-", color, scale);
            return;
        }

        char line[24]{};
        int offset = 0;
        int line_index = 0;
        const int length = static_cast<int>(std::strlen(value));

        while (offset < length && line_index < 4)
        {
            const int count = std::min(chars_per_line, length - offset);
            std::memcpy(line, value + offset, count);
            line[count] = '\0';
            text(4, y + line_index * (12 * scale / 2), line, color, scale);
            offset += count;
            ++line_index;
        }
    }

    std::uint16_t status_color(bool good, bool bad = false)
    {
        if (bad)
        {
            return RED;
        }
        return good ? GREEN : YELLOW;
    }

    void battery_widget()
    {
        const int percent = battery_percent.load();
        const auto charging =
            static_cast<tervanix::board::ChargeStatus>(charge_status.load());

        const bool is_charging =
            charging == tervanix::board::ChargeStatus::Charging;
        const bool is_full = charging == tervanix::board::ChargeStatus::Full;

        const std::uint16_t outline_color =
            (percent >= 0 && percent <= 15 && !is_charging) ? RED : WHITE;

        rect(91, 6, 37, 17, outline_color);
        fill_rect(128, 11, 3, 7, WHITE);

        if (percent >= 0)
        {
            const int fill = std::clamp(percent, 0, 100) * 31 / 100;
            if (fill > 0)
            {
                fill_rect(
                    94,
                    9,
                    fill,
                    11,
                    is_charging || is_full ? CYAN
                                           : (percent <= 15 ? RED : GREEN));
            }

            char value[16]{};
            std::snprintf(value, sizeof(value), "%d%%", percent);
            text(
                91,
                28,
                value,
                is_charging ? CYAN
                            : (is_full ? GREEN : (percent <= 15 ? RED : WHITE)),
                2);
        } else
        {
            text(91, 28, "--%", YELLOW, 2);
        }

        // Compact charge indicator beside the battery:
        //   +  charging
        //   F  full
        //   blank/discharging
        if (is_charging)
        {
            // Small lightning-bolt-like glyph made from rectangles so we don't
            // need another font symbol in the tiny bitmap font.
            fill_rect(84, 7, 3, 5, YELLOW);
            fill_rect(81, 12, 6, 3, YELLOW);
            fill_rect(83, 15, 3, 5, YELLOW);
        } else if (is_full)
        {
            text(80, 8, "F", GREEN, 2);
        }
    }

    void page_header(const char *title, int page)
    {
        text(4, 6, title, CYAN, 3);
        battery_widget();
        fill_rect(4, 48, 127, 2, DIM);

        char indicator[16]{};
        std::snprintf(
            indicator, sizeof(indicator), "%d/%d", page + 1, PAGE_COUNT);
        text(103, 225, indicator, DIM, 2);
        text(4, 225, "KEY1 NEXT", DIM, 2);
    }

    const char *display_state(const char *state)
    {
        if (!state)
        {
            return "WAITING";
        }

        if (std::strcmp(state, "falling_from_height") == 0)
        {
            return "FALL HEIGHT";
        }
        if (std::strcmp(state, "near_miss") == 0)
        {
            return "NEAR MISS";
        }
        if (std::strcmp(state, "stationary") == 0)
        {
            return "STATIONARY";
        }
        if (std::strcmp(state, "walking") == 0)
        {
            return "WALKING";
        }
        if (std::strcmp(state, "running") == 0)
        {
            return "RUNNING";
        }
        if (std::strcmp(state, "jumping") == 0)
        {
            return "JUMPING";
        }
        if (std::strcmp(state, "trip") == 0)
        {
            return "TRIP";
        }
        if (std::strcmp(state, "slip") == 0)
        {
            return "SLIP";
        }
        if (std::strcmp(state, "unknown") == 0)
        {
            return "UNKNOWN";
        }

        return state;
    }

    void draw_home(
        const tervanix::health::Snapshot &s,
        const tervanix::identity::IdentityStore &store)
    {
        page_header("HOME", 0);

        text(4, 57, "WORKER", DIM, 2);
        text(
            4,
            73,
            store.has_worker_context() ? store.worker_name() : "UNASSIGNED",
            WHITE,
            2);

        text(4, 96, "STATE", DIM, 2);
        centered_text(
            115,
            s.prediction_ready ? display_state(s.prediction_state) : "WAITING",
            s.prediction_ready ? CYAN : YELLOW,
            3);

        char conf[12]{};
        if (s.prediction_ready)
        {
            std::snprintf(
                conf, sizeof(conf), "%.0f%%", s.prediction_confidence * 100.0f);
        } else
        {
            std::strcpy(conf, "--%");
        }
        centered_text(151, conf, s.prediction_ready ? GREEN : YELLOW, 4);

        text(4, 188, "WIFI", status_color(s.wifi_connected), 2);
        text(
            44,
            188,
            s.wifi_connected ? "OK" : "WAIT",
            status_color(s.wifi_connected),
            2);

        text(4, 205, "MQTT", status_color(s.mqtt_connected), 2);
        text(
            44,
            205,
            s.mqtt_connected ? "OK" : "WAIT",
            status_color(s.mqtt_connected),
            2);
    }

    void draw_worker(
        const tervanix::health::Snapshot &s,
        const tervanix::identity::IdentityStore &store)
    {
        page_header("WORKER", 1);

        text(4, 57, "NAME", DIM, 2);
        text(
            4,
            74,
            store.has_worker_context() ? store.worker_name() : "-",
            WHITE,
            2);

        text(4, 99, "WORKER ID", DIM, 2);
        text(
            4,
            116,
            store.has_worker_context() ? store.worker_id() : "-",
            CYAN,
            2);

        text(4, 141, "DEVICE", DIM, 2);
        split_text(158, s.device_id, 14, WHITE, 2);

        if (store.has_uuid())
        {
            char uuid[37]{};
            std::snprintf(
                uuid,
                sizeof(uuid),
                "%02X%02X%02X%02X%02X%02X%02X%02X",
                store.uuid()[0],
                store.uuid()[1],
                store.uuid()[2],
                store.uuid()[3],
                store.uuid()[4],
                store.uuid()[5],
                store.uuid()[6],
                store.uuid()[7]);
            text(4, 196, "UUID", DIM, 2);
            text(44, 196, uuid, CYAN, 1);
        }
    }

    void draw_prediction(const tervanix::health::Snapshot &s)
    {
        page_header("PRED", 2);

        text(4, 57, "STATE", DIM, 2);
        centered_text(
            78,
            s.prediction_ready ? display_state(s.prediction_state) : "WAITING",
            s.prediction_ready ? CYAN : YELLOW,
            3);

        text(4, 116, "CONF", DIM, 2);
        char conf[12]{};
        if (s.prediction_ready)
        {
            std::snprintf(
                conf, sizeof(conf), "%.0f%%", s.prediction_confidence * 100.0f);
        } else
        {
            std::strcpy(conf, "--%");
        }
        centered_text(137, conf, s.prediction_ready ? GREEN : YELLOW, 4);

        text(4, 178, "MODEL", DIM, 2);
        text(
            60,
            178,
            s.model_ready ? "READY" : "WAIT",
            status_color(s.model_ready),
            2);

        text(4, 197, "IMU", DIM, 2);
        text(
            44,
            197,
            s.imu_ready ? "READY" : "WAIT",
            status_color(s.imu_ready),
            2);

        if (s.model_sha256 && s.model_sha256[0])
        {
            char hash[13]{};
            std::memcpy(hash, s.model_sha256, 12);
            hash[12] = '\0';
            text(4, 214, hash, DIM, 1);
        }
    }

    void draw_system(const tervanix::health::Snapshot &s)
    {
        page_header("SYSTEM", 3);

        int y = 58;
        auto row =
            [&](const char *label, const char *value, std::uint16_t color) {
                text(4, y, label, DIM, 2);
                text(60, y, value, color, 2);
                y += 23;
            };

        row("REG", s.registered ? "OK" : "WAIT", status_color(s.registered));
        row("TIME", s.time_synced ? "OK" : "WAIT", status_color(s.time_synced));
        row("WIFI",
            s.wifi_connected ? "OK" : "WAIT",
            status_color(s.wifi_connected));
        row("MQTT",
            s.mqtt_connected ? "OK" : "WAIT",
            status_color(s.mqtt_connected));

        row("AUTH",
            s.mqtt_auth_rejected ? "ERR"
                                 : (s.mqtt_auth_accepted ? "OK" : "WAIT"),
            status_color(s.mqtt_auth_accepted, s.mqtt_auth_rejected));

        text(4, y, "GW", DIM, 2);
        text(36, y, s.gateway_ip, WHITE, 2);
        y += 23;

        char pub[18]{};
        std::snprintf(
            pub,
            sizeof(pub),
            "PUB %llu",
            static_cast<unsigned long long>(s.published_states));
        text(4, y, pub, CYAN, 2);
    }

    void flush()
    {
        if (lcd_panel == nullptr)
        {
            return;
        }

        esp_lcd_panel_draw_bitmap(
            lcd_panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, framebuffer);
    }

    esp_err_t initialize_panel()
    {
        ESP_RETURN_ON_ERROR(
            tervanix::board::initialize_internal_i2c(), TAG, "I2C init failed");

        ESP_RETURN_ON_ERROR(
            tervanix::board::set_lcd_backlight(false),
            TAG,
            "Backlight off failed");
        ESP_RETURN_ON_ERROR(
            tervanix::board::set_lcd_reset(false),
            TAG,
            "LCD reset assert failed");
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_RETURN_ON_ERROR(
            tervanix::board::set_lcd_reset(true),
            TAG,
            "LCD reset release failed");
        vTaskDelay(pdMS_TO_TICKS(120));

        spi_bus_config_t bus_config{};
        bus_config.sclk_io_num = LCD_SCLK;
        bus_config.mosi_io_num = LCD_MOSI;
        bus_config.miso_io_num = -1;
        bus_config.quadwp_io_num = -1;
        bus_config.quadhd_io_num = -1;
        bus_config.max_transfer_sz =
            LCD_WIDTH * LCD_HEIGHT * sizeof(std::uint16_t);

        esp_err_t result =
            spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        {
            return result;
        }

        esp_lcd_panel_io_spi_config_t io_config{};
        io_config.dc_gpio_num = LCD_DC;
        io_config.cs_gpio_num = LCD_CS;
        io_config.pclk_hz = LCD_SPI_HZ;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        io_config.spi_mode = 0;
        io_config.trans_queue_depth = 10;

        ESP_RETURN_ON_ERROR(
            esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &lcd_io),
            TAG,
            "LCD SPI IO init failed");

        esp_lcd_panel_dev_config_t panel_config{};
        panel_config.reset_gpio_num = -1;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;

        ESP_RETURN_ON_ERROR(
            esp_lcd_new_panel_st7789(lcd_io, &panel_config, &lcd_panel),
            TAG,
            "ST7789 panel init failed");

        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_init(lcd_panel), TAG, "ST7789 init commands failed");
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_invert_color(lcd_panel, true),
            TAG,
            "LCD inversion failed");
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_set_gap(
                lcd_panel, LCD_MEMORY_OFFSET_X, LCD_MEMORY_OFFSET_Y),
            TAG,
            "LCD panel offset failed");
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_disp_on_off(lcd_panel, true),
            TAG,
            "LCD display-on failed");
        ESP_RETURN_ON_ERROR(
            tervanix::board::set_lcd_backlight(true),
            TAG,
            "Backlight on failed");

        return ESP_OK;
    }

    void control_task(void *)
    {
        uint8_t idle_keys = 0;
        uint8_t last_raw = 0;
        uint8_t stable_keys = 0;
        uint8_t previous_pressed = 0;
        int stable_samples = 0;

        bool baseline_ready = false;
        int64_t next_battery_ms = 0;

        while (controls_running.load())
        {
            uint8_t raw = 0;
            if (tervanix::board::read_user_keys(raw) == ESP_OK)
            {
                if (!baseline_ready)
                {
                    idle_keys = raw;
                    last_raw = raw;
                    stable_keys = raw;
                    baseline_ready = true;
                } else
                {
                    if (raw == last_raw)
                    {
                        ++stable_samples;
                    } else
                    {
                        last_raw = raw;
                        stable_samples = 0;
                    }

                    // 2 stable samples = about 100 ms debounce.
                    if (stable_samples >= 1 && stable_keys != raw)
                    {
                        stable_keys = raw;

                        // The idle level is learned at startup. XOR therefore
                        // converts either active-high or active-low wiring into
                        // a logical "pressed" bit.
                        const uint8_t pressed = static_cast<uint8_t>(
                            (stable_keys ^ idle_keys) & 0x03U);

                        const uint8_t new_presses =
                            static_cast<uint8_t>(pressed & ~previous_pressed);

                        if (new_presses & 0x01U)
                        {
                            active_page.store(
                                (active_page.load() + 1) % PAGE_COUNT);
                            ESP_LOGI(
                                TAG,
                                "KEY1 pressed -> page %d/%d",
                                active_page.load() + 1,
                                PAGE_COUNT);
                        }

                        if (new_presses & 0x02U)
                        {
                            active_page.store(
                                (active_page.load() + PAGE_COUNT - 1) %
                                PAGE_COUNT);
                            ESP_LOGI(
                                TAG,
                                "KEY2 pressed -> page %d/%d",
                                active_page.load() + 1,
                                PAGE_COUNT);
                        }

                        previous_pressed = pressed;
                    }
                }
            }

            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms >= next_battery_ms)
            {
                uint8_t percent = 0;
                if (tervanix::board::read_battery_percentage(percent) == ESP_OK)
                {
                    battery_percent.store(percent);
                } else
                {
                    battery_percent.store(-1);
                }

                tervanix::board::ChargeStatus status =
                    tervanix::board::ChargeStatus::Unknown;

                if (tervanix::board::read_charge_status(status) == ESP_OK)
                {
                    charge_status.store(static_cast<int>(status));
                } else
                {
                    charge_status.store(
                        static_cast<int>(
                            tervanix::board::ChargeStatus::Unknown));
                }

                next_battery_ms = now_ms + BATTERY_REFRESH_MS;
            }

            vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
        }

        vTaskDelete(nullptr);
    }

} // namespace

namespace tervanix::health
{

    esp_err_t initialize(const identity::IdentityStore &)
    {
        const esp_err_t result = initialize_panel();
        if (result != ESP_OK)
        {
            return result;
        }

        display_ready = true;
        controls_running.store(true);

        const BaseType_t task_result = xTaskCreate(
            control_task, "health-controls", 3072, nullptr, 3, nullptr);

        if (task_result != pdPASS)
        {
            controls_running.store(false);
            ESP_LOGW(TAG, "Could not start button/battery task");
        }

        clear(BLACK);
        centered_text(35, "TERVANIX", CYAN, 3);
        centered_text(78, "NESSO N1", WHITE, 3);
        centered_text(130, "STARTING", YELLOW, 3);
        centered_text(185, "KEY1 NEXT", DIM, 2);
        flush();

        ESP_LOGI(
            TAG,
            "Large Nesso UI ready: 4 pages, KEY1 next, KEY2 previous, BQ27220 "
            "battery");
        return ESP_OK;
    }

    void render(
        const Snapshot &snapshot,
        const identity::IdentityStore &identity_store)
    {
        if (!display_ready)
        {
            return;
        }

        clear(BLACK);

        switch (active_page.load())
        {
        case 0:
            draw_home(snapshot, identity_store);
            break;
        case 1:
            draw_worker(snapshot, identity_store);
            break;
        case 2:
            draw_prediction(snapshot);
            break;
        default:
            draw_system(snapshot);
            break;
        }

        flush();
    }

    bool available() { return display_ready; }

} // namespace tervanix::health

#else

namespace tervanix::health
{

    esp_err_t initialize(const identity::IdentityStore &) { return ESP_OK; }

    void render(const Snapshot &, const identity::IdentityStore &) {}

    bool available() { return false; }

} // namespace tervanix::health

#endif
