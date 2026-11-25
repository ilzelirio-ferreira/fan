#include "app_pairing.h"
#include "app_portal.h"
#include "fan_control.h"

#include <atomic>
#include <stdio.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <platform/CHIPDeviceLayer.h>

using namespace esp_matter;
namespace fc = chip::app::Clusters::FanControl;
namespace fa = fc::Attributes;

static constexpr char kTag[] = "fan";
static constexpr gpio_num_t kButton = GPIO_NUM_33;
static constexpr gpio_num_t kRelayMsb = GPIO_NUM_35;
static constexpr gpio_num_t kRelayLsb = GPIO_NUM_34;
static constexpr int64_t kRelayReleaseUs = 30000;

static uint16_t fan_id;
// Matter callbacks and the scheduled relay work run in the CHIP task.
static uint8_t requested_speed;
static bool publish_pending = true;
static bool publishing;
static std::atomic<unsigned> button_presses{0};
static std::atomic<bool> work_scheduled{false};

static void init_gpio()
{
    ESP_ERROR_CHECK(gpio_set_level(kRelayMsb, 0));
    ESP_ERROR_CHECK(gpio_set_level(kRelayLsb, 0));
    gpio_config_t outputs = {};
    outputs.pin_bit_mask = (1ULL << kRelayMsb) | (1ULL << kRelayLsb);
    outputs.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&outputs));

    gpio_config_t button = {};
    button.pin_bit_mask = 1ULL << kButton;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&button));
}

static void drive_relays(uint8_t code)
{
    ESP_ERROR_CHECK(gpio_set_level(kRelayMsb, (code >> 1) & 1U));
    ESP_ERROR_CHECK(gpio_set_level(kRelayLsb, code & 1U));
}

static esp_err_t on_attribute(attribute::callback_type_t type, uint16_t endpoint_id,
                              uint32_t cluster_id, uint32_t attr_id, esp_matter_attr_val_t *value, void *)
{
    if (publishing || type != attribute::PRE_UPDATE || fan_id == 0 || endpoint_id != fan_id || cluster_id != fc::Id) {
        return ESP_OK;
    }

    const uint8_t raw = value->val.u8;
    switch (attr_id) {
    case fa::FanMode::Id:
        // Low, Medium and High use the same ordinals as the relay code.
        if (raw > static_cast<uint8_t>(fc::FanModeEnum::kOn)) {
            return ESP_ERR_INVALID_ARG;
        }
        requested_speed = raw == static_cast<uint8_t>(fc::FanModeEnum::kOn) ? 3 : raw;
        break;
    case fa::SpeedSetting::Id:
        if (raw > 3) {
            return ESP_ERR_INVALID_ARG;
        }
        requested_speed = raw;
        break;
    case fa::PercentSetting::Id:
        if (raw > 100) {
            return ESP_ERR_INVALID_ARG;
        }
        requested_speed = speed_from_percent(raw);
        break;
    default:
        return ESP_OK;
    }
    publish_pending = true;
    return ESP_OK;
}

static bool publish_speed(uint8_t speed)
{
    struct AttributeValue {
        uint32_t id;
        esp_matter_attr_val_t value;
    };
    const uint8_t percent = percent_from_speed(speed);
    AttributeValue values[] = {
        {fa::SpeedSetting::Id, esp_matter_nullable_uint8(speed)},
        {fa::PercentSetting::Id, esp_matter_nullable_uint8(percent)},
        {fa::FanMode::Id, esp_matter_enum8(speed)},
        {fa::SpeedCurrent::Id, esp_matter_uint8(speed)},
        {fa::PercentCurrent::Id, esp_matter_uint8(percent)},
    };

    publishing = true;
    bool success = true;
    for (auto &entry : values) {
        const esp_err_t err = attribute::update(fan_id, fc::Id, entry.id, &entry.value);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "Attribute 0x%lx: %s", static_cast<unsigned long>(entry.id), esp_err_to_name(err));
            success = false;
        }
    }
    publishing = false;
    return success;
}

static esp_err_t on_identify(identification::callback_type_t, uint16_t, uint8_t, uint8_t, void *)
{
    return ESP_OK;
}

static void on_matter_event(const ChipDeviceEvent *event, intptr_t)
{
    if (event->Type == chip::DeviceLayer::DeviceEventType::kCommissioningComplete) {
        ESP_LOGI(kTag, "Pareamento Matter concluido");
    }
    if (event->Type == chip::DeviceLayer::DeviceEventType::kFabricRemoved) {
        auto &server = chip::Server::GetInstance();
        auto &window = server.GetCommissioningWindowManager();
        if (server.GetFabricTable().FabricCount() == 0 && !window.IsCommissioningWindowOpen()) {
            const CHIP_ERROR err = window.OpenBasicCommissioningWindow(
                chip::System::Clock::Seconds16(300), chip::CommissioningWindowAdvertisement::kDnssdOnly);
            if (err != CHIP_NO_ERROR) {
                ESP_LOGE(kTag, "Falha ao reabrir o pareamento: %" CHIP_ERROR_FORMAT, err.Format());
            }
        }
    }
}

static void process_fan(intptr_t)
{
    static uint8_t relay_code = 0;
    static int64_t relay_ready_us = 0;
    const int64_t now = esp_timer_get_time();

    const unsigned presses = button_presses.exchange(0);
    if (presses) {
        requested_speed = next_speed(requested_speed, presses);
        publish_pending = true;
    }

    // Drop outgoing contacts before enabling new ones on a transition such as 01 -> 10.
    const uint8_t retained = relay_code & requested_speed;
    if (retained != relay_code) {
        drive_relays(retained);
        relay_code = retained;
        relay_ready_us = now + kRelayReleaseUs;
    }
    if (relay_code != requested_speed && now >= relay_ready_us) {
        drive_relays(requested_speed);
        relay_code = requested_speed;
    }
    if (publish_pending && relay_code == requested_speed) {
        publish_pending = !publish_speed(relay_code);
    }
    work_scheduled.store(false);
}

extern "C" void app_main()
{
    init_gpio();
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(app_pairing_init());
    ESP_ERROR_CHECK(app_portal_load());

    node::config_t node_config;
    snprintf(node_config.root_node.basic_information.node_label,
             sizeof(node_config.root_node.basic_information.node_label), "%s", app_portal_module_name());
    auto *node = node::create(&node_config, on_attribute, on_identify);
    ESP_ERROR_CHECK(node ? ESP_OK : ESP_FAIL);

    endpoint::fan::config_t config;
    config.fan_control.fan_mode_sequence = static_cast<uint8_t>(fc::FanModeSequenceEnum::kOffLowMedHigh);
    auto *fan = endpoint::fan::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
    ESP_ERROR_CHECK(fan ? ESP_OK : ESP_FAIL);
    fan_id = endpoint::get_id(fan);

    auto *control = cluster::get(fan_id, fc::Id);
    ESP_ERROR_CHECK(control ? ESP_OK : ESP_FAIL);
    cluster::fan_control::feature::multi_speed::config_t speeds;
    speeds.speed_max = 3;
    ESP_ERROR_CHECK(cluster::fan_control::feature::multi_speed::add(control, &speeds));
    ESP_ERROR_CHECK(app_portal_add_channel(fan, 0));
    ESP_ERROR_CHECK(esp_matter::start(on_matter_event));
    ESP_ERROR_CHECK(app_portal_start());

#if CONFIG_ENABLE_CHIP_SHELL
    console::wifi_register_commands();
    console::factoryreset_register_commands();
    console::init();
#endif

    FanButton button;
    auto &platform = chip::DeviceLayer::PlatformMgr();
    while (true) {
        if (button.sample(gpio_get_level(kButton) == 0, esp_timer_get_time())) {
            button_presses.fetch_add(1);
        }
        if (!work_scheduled.exchange(true) &&
            platform.ScheduleWork(process_fan, 0) != CHIP_NO_ERROR) {
            work_scheduled.store(false);
        }
        vTaskDelay(pdMS_TO_TICKS(10) + 1);
    }
}
