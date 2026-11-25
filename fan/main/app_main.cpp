#include "app_pairing.h"
#include "app_portal.h"
#include "fan_control.h"

#include <atomic>
#include <stdio.h>

#include <app/clusters/fan-control-server/CodegenIntegration.h>
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
using chip::Protocols::InteractionModel::Status;

static constexpr char kTag[] = "fan";
static constexpr gpio_num_t kButton = GPIO_NUM_33;
static constexpr gpio_num_t kRelayMsb = GPIO_NUM_35;
static constexpr gpio_num_t kRelayLsb = GPIO_NUM_34;
static constexpr int64_t kRelayReleaseUs = 30000;

static uint16_t fan_id;
// Matter callbacks and the scheduled relay work run in the CHIP task.
static uint8_t requested_speed;
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

class FanDelegate final : public fc::Delegate {
public:
    Status HandleStep(fc::StepDirectionEnum, bool, bool) override
    {
        return Status::UnsupportedCommand;
    }

    void OnFanDriveStateChanged(const fc::FanDriveState &state) override
    {
        if (!state.speedSetting.IsNull()) {
            requested_speed = state.speedSetting.Value();
        }
    }
};

static FanDelegate fan_delegate;

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
    static int last_reported_speed = -1;
    static int64_t relay_ready_us = 0;
    static chip::app::Clusters::FanControlCluster *control = nullptr;
    if (!control) {
        control = fc::FindClusterOnEndpoint(fan_id);
        ESP_ERROR_CHECK(control ? ESP_OK : ESP_FAIL);
        fc::SetDefaultDelegate(fan_id, &fan_delegate);
        // The SDK persists FanMode; the motor must start off after any reboot.
        ESP_ERROR_CHECK(control->SetFanMode(fc::FanModeEnum::kOff) == Status::Success ? ESP_OK : ESP_FAIL);
    }
    const int64_t now = esp_timer_get_time();

    const unsigned presses = button_presses.exchange(0);
    if (presses) {
        const uint8_t speed = next_speed(requested_speed, presses);
        const Status status = control->SetSpeedSetting(chip::app::DataModel::MakeNullable(speed));
        if (status != Status::Success) {
            ESP_LOGW(kTag, "Falha ao atualizar velocidade Matter: %u", static_cast<unsigned>(status));
        }
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
    if (relay_code == requested_speed && control->GetSpeedCurrent() != relay_code) {
        control->SetSpeedCurrent(relay_code);
        control->SetPercentCurrent(percent_from_speed(relay_code));
    }
    if (relay_code == requested_speed && relay_code != last_reported_speed) {
        ESP_LOGI(kTag, "Estado V%u (%u%%) | Rele A=%u | Rele B=%u",
                 static_cast<unsigned>(relay_code), static_cast<unsigned>(percent_from_speed(relay_code)),
                 (relay_code >> 1) & 1U, relay_code & 1U);
        last_reported_speed = relay_code;
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
    auto *node = node::create(&node_config, nullptr, on_identify);
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
