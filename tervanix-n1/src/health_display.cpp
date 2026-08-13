#include "health_display.hpp"

#if defined(TERVANIX_BOARD_NESSO_N1)

#include <algorithm>
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

    // Official Nesso N1 LCD wiring.
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

    esp_lcd_panel_io_handle_t lcd_io = nullptr;
    esp_lcd_panel_handle_t lcd_panel = nullptr;
    bool display_ready = false;

    // Persistent frame buffer. The Nesso screen is only 64.8 KiB in RGB565.
    std::uint16_t framebuffer[LCD_WIDTH * LCD_HEIGHT]{};

    struct Glyph {
        char c;
        std::uint16_t bits; // 3x5, row-major, bit 14 is top-left.
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

    void pixel(int x, int y, std::uint16_t color)
    {
        if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        {
            return;
        }
        framebuffer[y * LCD_WIDTH + x] = color;
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

    void draw_char(int x, int y, char c, std::uint16_t color, int scale = 1)
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
    text(int x, int y, const char *value, std::uint16_t color, int scale = 1)
    {
        if (value == nullptr)
        {
            value = "-";
        }

        const int advance = 4 * scale;
        while (*value != '\0' && x < LCD_WIDTH - (3 * scale))
        {
            draw_char(x, y, *value, color, scale);
            x += advance;
            ++value;
        }
    }

    void label_value(
        int y,
        const char *label,
        const char *value,
        std::uint16_t value_color = WHITE)
    {
        text(4, y, label, DIM);
        text(31, y, value, value_color);
    }

    void
    status_box(int x, int y, const char *label, bool good, bool bad = false)
    {
        const std::uint16_t color = bad ? RED : (good ? GREEN : YELLOW);
        rect(x, y, 62, 17, color);
        text(x + 4, y + 6, label, color);
        text(x + 34, y + 6, bad ? "ERR" : (good ? "OK" : "WAIT"), color);
    }

    void uuid_string(
        const tervanix::identity::NodeUuid &uuid,
        char *out,
        std::size_t out_size)
    {
        std::snprintf(
            out,
            out_size,
            "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%"
            "02X",
            uuid[0],
            uuid[1],
            uuid[2],
            uuid[3],
            uuid[4],
            uuid[5],
            uuid[6],
            uuid[7],
            uuid[8],
            uuid[9],
            uuid[10],
            uuid[11],
            uuid[12],
            uuid[13],
            uuid[14],
            uuid[15]);
    }

    esp_err_t initialize_panel()
    {
        ESP_RETURN_ON_ERROR(
            tervanix::board::initialize_internal_i2c(), TAG, "I2C init failed");

        // Nesso reset/backlight are not ESP32 GPIOs. They are E1.P1/E1.P6 on
        // the board's PI4IOE5V6408 at address 0x44.
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
            ESP_LOGE(
                TAG, "LCD SPI bus init failed: %s", esp_err_to_name(result));
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
            esp_lcd_new_panel_io_spi(
                (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &lcd_io),
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

    void flush()
    {
        if (lcd_panel == nullptr)
        {
            return;
        }
        esp_lcd_panel_draw_bitmap(
            lcd_panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, framebuffer);
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

        clear(BLACK);
        text(8, 8, "TERVANIX N1", CYAN, 2);
        text(8, 24, "HEALTH CONSOLE", WHITE);
        text(8, 36, "STARTING...", YELLOW);
        flush();

        display_ready = true;
        ESP_LOGI(TAG, "Native Nesso N1 ST7789 health display ready: 135x240");
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

        text(4, 5, "TERVANIX N1", CYAN, 2);
        text(4, 18, "HEALTH", WHITE);
        fill_rect(4, 26, 127, 1, DIM);

        status_box(4, 32, "BLE", snapshot.registered || snapshot.ble_connected);
        status_box(69, 32, "WIFI", snapshot.wifi_connected);
        status_box(4, 52, "MQTT", snapshot.mqtt_connected);
        status_box(
            69,
            52,
            "AUTH",
            snapshot.mqtt_auth_accepted,
            snapshot.mqtt_auth_rejected);

        label_value(
            75,
            "REG",
            snapshot.registered ? "REGISTERED" : "WAITING",
            snapshot.registered ? GREEN : YELLOW);
        label_value(
            84,
            "TIME",
            snapshot.time_synced ? "SYNCED" : "WAITING",
            snapshot.time_synced ? GREEN : YELLOW);
        label_value(
            93,
            "IMU",
            snapshot.imu_ready ? "OK" : "WAITING",
            snapshot.imu_ready ? GREEN : YELLOW);
        label_value(
            102,
            "WID",
            identity_store.has_worker_context() ? identity_store.worker_id()
                                                : "-");
        label_value(
            111,
            "NAME",
            identity_store.has_worker_context() ? identity_store.worker_name()
                                                : "-");
        label_value(120, "DEV", snapshot.device_id);
        label_value(129, "GW", snapshot.gateway_ip);

        text(4, 141, "WORKER UUID", DIM);
        if (identity_store.has_uuid())
        {
            char uuid[37]{};
            uuid_string(identity_store.uuid(), uuid, sizeof(uuid));
            char first[19]{};
            char second[19]{};
            std::memcpy(first, uuid, 18);
            std::memcpy(second, uuid + 18, 18);
            text(4, 150, first, CYAN);
            text(4, 159, second, CYAN);
        } else
        {
            text(4, 150, "NOT ASSIGNED", YELLOW);
        }

        char line[48]{};
        std::snprintf(
            line,
            sizeof(line),
            "MODEL:%s",
            snapshot.model_ready ? "OK" : "WAIT");
        text(4, 172, line, snapshot.model_ready ? GREEN : YELLOW);
        if (snapshot.prediction_ready && snapshot.prediction_state)
        {
            std::snprintf(
                line,
                sizeof(line),
                "STATE:%s %.0f%%",
                snapshot.prediction_state,
                snapshot.prediction_confidence * 100.0f);
            text(4, 181, line, CYAN);
        } else
        {
            text(4, 181, "STATE:WAITING", YELLOW);
        }
        if (snapshot.model_sha256 && snapshot.model_sha256[0])
        {
            std::snprintf(
                line, sizeof(line), "HASH:%.12s", snapshot.model_sha256);
            text(4, 190, line, DIM);
        }
        std::snprintf(
            line,
            sizeof(line),
            "PUB:%llu S:%llu ST:%llu",
            static_cast<unsigned long long>(snapshot.published_batches),
            static_cast<unsigned long long>(snapshot.published_samples),
            static_cast<unsigned long long>(snapshot.published_states));
        text(4, 199, line, WHITE);
        fill_rect(4, 209, 127, 1, DIM);
        if (snapshot.mqtt_auth_rejected)
        {
            text(4, 216, "MQTT AUTH REJECTED", RED);
        } else if (!snapshot.registered)
        {
            text(4, 216, "BLE REGISTRATION", YELLOW);
            text(4, 225, "REQUIRED", YELLOW);
        } else if (!snapshot.wifi_connected)
        {
            text(4, 216, "WAITING FOR WIFI", YELLOW);
        } else if (!snapshot.time_synced)
        {
            text(4, 216, "SYNCING PI TIME", YELLOW);
        } else if (!snapshot.mqtt_connected)
        {
            text(4, 216, "CONNECTING MQTT", YELLOW);
        } else
        {
            text(4, 216, "SYSTEM ONLINE", GREEN);
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
