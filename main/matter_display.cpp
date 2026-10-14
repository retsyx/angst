#include "matter_display.h"
#include "matter_identity.h"
#include "matter_bootstrap.h"
#include "matter_provisioning.h"

#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_matter.h"
#include "esp_matter_console.h"
#include "esp_matter_providers.h"
#include <app/persistence/AttributePersistence.h>
#include <app/persistence/DefaultAttributePersistenceProvider.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceError.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/KvsPersistentStorageDelegate.h>

#if !CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER || !CONFIG_CUSTOM_DEVICE_INSTANCE_INFO_PROVIDER
#error "Matter requires the generated commissioning and device information providers"
#endif
#if CONFIG_ENABLE_TEST_SETUP_PARAMS || CONFIG_CUSTOM_NETWORK_CONFIG || !CONFIG_ENABLE_CHIPOBLE || !CONFIG_ENABLE_WIFI_STATION
#error "Enable BLE and Wi-Fi commissioning and disable test setup parameters/custom networking"
#endif
#if CONFIG_USE_MINIMAL_MDNS
#error "Disable USE_MINIMAL_MDNS so Matter and the web service share the Espressif mDNS responder"
#endif
#if CONFIG_EXAMPLE_DAC_PROVIDER
static_assert(DISPLAY_MATTER_VENDOR_ID >= 0xFFF1 && DISPLAY_MATTER_VENDOR_ID <= 0xFFF3 &&
              DISPLAY_MATTER_PRODUCT_ID >= 0x8000 && DISPLAY_MATTER_PRODUCT_ID <= 0x801F,
              "Example attestation supports VID 0xFFF1-0xFFF3 and PID 0x8000-0x801F; use issued credentials for other IDs");
static_assert(DISPLAY_MATTER_VENDOR_ID == CONFIG_DEVICE_VENDOR_ID &&
              DISPLAY_MATTER_PRODUCT_ID == CONFIG_DEVICE_PRODUCT_ID,
              "Generated VID/PID must match the configured attestation credentials");
#endif

using namespace esp_matter;
using namespace chip::app::Clusters;

namespace {
constexpr char TAG[] = "matter_display";
constexpr EventBits_t SERVER_READY_BIT = BIT0;
EventGroupHandle_t server_startup_events;
uint16_t light_endpoint_id;
portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
bool power_on = true;
uint8_t light_level = 254;
bool identifying = false;
int64_t effect_until = 0;
matter_provisioning_identity_t provisioned_identity{};
bool identity_ready = false;

esp_err_t load_provisioned_identity()
{
    esp_err_t error = matter_provisioning_load(nullptr, false, &provisioned_identity);
    if (error != ESP_ERR_NOT_FOUND) return error;

    // A build's generated constants are first-provisioning inputs, never OTA
    // defaults. Missing identity on an existing device must not replace it.
    char existing_id[chip::DeviceLayer::ConfigurationManager::kMaxUniqueIDLength + 1]{};
    const CHIP_ERROR existing_error = chip::DeviceLayer::ConfigurationMgr().GetUniqueId(
        existing_id, sizeof(existing_id));
    const bool existing_identity = existing_error == CHIP_NO_ERROR;
    if (!existing_identity && existing_error != CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND &&
        existing_error != CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND) return ESP_FAIL;

    bool fresh_install = false;
    error = matter_bootstrap_check(&fresh_install);
    if (error != ESP_OK) return error;
    if (!fresh_install || existing_identity) {
        ESP_LOGE(TAG, "Existing device has no saved Matter identity; refusing to replace its pairing credentials");
        return ESP_ERR_INVALID_STATE;
    }

    static_assert(sizeof(DISPLAY_MATTER_SERIAL_NUMBER) <= sizeof(provisioned_identity.serial_number));
    static_assert(sizeof(DISPLAY_MATTER_SPAKE2P_SALT) == sizeof(provisioned_identity.salt));
    static_assert(sizeof(DISPLAY_MATTER_SPAKE2P_VERIFIER) == sizeof(provisioned_identity.verifier));
    static_assert(sizeof(DISPLAY_MATTER_ROTATING_ID) == sizeof(provisioned_identity.rotating_id));
    matter_provisioning_identity_t seed{};
    seed.discriminator = DISPLAY_MATTER_DISCRIMINATOR;
    seed.iterations = DISPLAY_MATTER_SPAKE2P_ITERATIONS;
    seed.vendor_id = DISPLAY_MATTER_VENDOR_ID;
    seed.product_id = DISPLAY_MATTER_PRODUCT_ID;
    std::memcpy(seed.serial_number, DISPLAY_MATTER_SERIAL_NUMBER, sizeof(DISPLAY_MATTER_SERIAL_NUMBER));
    std::memcpy(seed.salt, DISPLAY_MATTER_SPAKE2P_SALT, sizeof(seed.salt));
    std::memcpy(seed.verifier, DISPLAY_MATTER_SPAKE2P_VERIFIER, sizeof(seed.verifier));
    std::memcpy(seed.rotating_id, DISPLAY_MATTER_ROTATING_ID, sizeof(seed.rotating_id));
    return matter_provisioning_load(&seed, true, &provisioned_identity);
}

bool same_identity(const matter_provisioning_identity_t &a, const matter_provisioning_identity_t &b)
{
    return a.discriminator == b.discriminator && a.iterations == b.iterations &&
        a.vendor_id == b.vendor_id && a.product_id == b.product_id &&
        std::strcmp(a.serial_number, b.serial_number) == 0 &&
        std::memcmp(a.salt, b.salt, sizeof(a.salt)) == 0 &&
        std::memcmp(a.verifier, b.verifier, sizeof(a.verifier)) == 0 &&
        std::memcmp(a.rotating_id, b.rotating_id, sizeof(a.rotating_id)) == 0;
}

CHIP_ERROR copy_string(char *buffer, size_t capacity, const char *value)
{
    size_t length = std::strlen(value) + 1;
    if (capacity < length) return CHIP_ERROR_BUFFER_TOO_SMALL;
    std::memcpy(buffer, value, length);
    return CHIP_NO_ERROR;
}

class CommissioningData final : public chip::DeviceLayer::CommissionableDataProvider {
public:
    CHIP_ERROR GetSetupDiscriminator(uint16_t &value) override
    { value = provisioned_identity.discriminator; return CHIP_NO_ERROR; }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetSpake2pIterationCount(uint32_t &value) override
    { value = provisioned_identity.iterations; return CHIP_NO_ERROR; }
    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan &buffer) override
    { return chip::CopySpanToMutableSpan(chip::ByteSpan(provisioned_identity.salt), buffer); }
    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan &buffer, size_t &length) override
    {
        length = sizeof(provisioned_identity.verifier);
        return chip::CopySpanToMutableSpan(chip::ByteSpan(provisioned_identity.verifier), buffer);
    }
    // Firmware stores the verifier; the passcode stays in the generated onboarding files.
    CHIP_ERROR GetSetupPasscode(uint32_t &) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
};

class DeviceInformation final : public chip::DeviceLayer::DeviceInstanceInfoProvider {
public:
    CHIP_ERROR GetVendorName(char *b, size_t n) override
    { return copy_string(b, n, DISPLAY_MATTER_VENDOR_NAME); }
    CHIP_ERROR GetVendorId(uint16_t &v) override { v = provisioned_identity.vendor_id; return CHIP_NO_ERROR; }
    CHIP_ERROR GetProductName(char *b, size_t n) override
    { return copy_string(b, n, DISPLAY_MATTER_PRODUCT_NAME); }
    CHIP_ERROR GetProductId(uint16_t &v) override { v = provisioned_identity.product_id; return CHIP_NO_ERROR; }
    CHIP_ERROR GetPartNumber(char *b, size_t n) override { return copy_string(b, n, "Angst-AMOLED-S3"); }
    CHIP_ERROR GetProductURL(char *b, size_t n) override { return copy_string(b, n, "https://github.com/retsyx/angst"); }
    CHIP_ERROR GetProductLabel(char *b, size_t n) override { return copy_string(b, n, DISPLAY_MATTER_NODE_LABEL); }
    CHIP_ERROR GetSerialNumber(char *b, size_t n) override
    { return copy_string(b, n, provisioned_identity.serial_number); }
    CHIP_ERROR GetManufacturingDate(uint16_t &, uint8_t &, uint8_t &) override
    { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetHardwareVersion(uint16_t &v) override
    { v = DISPLAY_MATTER_HARDWARE_VERSION; return CHIP_NO_ERROR; }
    CHIP_ERROR GetHardwareVersionString(char *b, size_t n) override
    { return copy_string(b, n, DISPLAY_MATTER_HARDWARE_VERSION_STRING); }
    CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan &buffer) override
    { return chip::CopySpanToMutableSpan(chip::ByteSpan(provisioned_identity.rotating_id), buffer); }
};

CommissioningData commissioning_data;
DeviceInformation device_information;

CHIP_ERROR initialize_identity_storage()
{
    // Native Basic Information owns these values outside ESP-Matter's legacy
    // attribute database. Seed them before cluster startup; saved values win.
    auto &configuration = chip::DeviceLayer::ConfigurationMgr();
    char unique_id[chip::DeviceLayer::ConfigurationManager::kMaxUniqueIDLength + 1];
    CHIP_ERROR error = configuration.GetUniqueId(unique_id, sizeof(unique_id));
    if (error == CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND || error == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND) {
        ReturnErrorOnFailure(configuration.StoreUniqueId(provisioned_identity.serial_number,
                                                         std::strlen(provisioned_identity.serial_number)));
    } else {
        ReturnErrorOnFailure(error);
    }

    chip::KvsPersistentStorageDelegate storage;
    ReturnErrorOnFailure(storage.Init(&chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr()));
    chip::app::DefaultAttributePersistenceProvider provider;
    ReturnErrorOnFailure(provider.Init(&storage));
    const chip::app::ConcreteAttributePath path(0, BasicInformation::Id,
                                               BasicInformation::Attributes::NodeLabel::Id);
    constexpr size_t max_label_length = BasicInformation::Attributes::NodeLabel::TypeInfo::MaxLength();
    uint8_t saved_label[max_label_length + 1];
    chip::MutableByteSpan saved_span(saved_label);
    error = provider.ReadValue(path, saved_span);
    if (error == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND) {
        chip::app::Storage::String<max_label_length> label;
        VerifyOrReturnError(label.SetContent(chip::CharSpan::fromCharString(DISPLAY_MATTER_NODE_LABEL)),
                            CHIP_ERROR_INVALID_ARGUMENT);
        chip::app::AttributePersistence persistence(provider);
        return persistence.StoreString(path, label);
    }
    // An intentionally empty controller label is a saved value, too.
    return error;
}

esp_err_t attribute_changed(attribute::callback_type_t type, uint16_t endpoint_id,
                            uint32_t cluster_id, uint32_t attribute_id,
                            esp_matter_attr_val_t *value, void *)
{
    if (endpoint_id != light_endpoint_id || !value) return ESP_OK;
    bool is_level = cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id;
    bool is_power = cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id;
    if (type == attribute::PRE_UPDATE && is_level && !value->is_null() && value->val.u8 > 254)
        return ESP_ERR_INVALID_ARG;
    if (type != attribute::POST_UPDATE) return ESP_OK;

    portENTER_CRITICAL(&state_lock);
    if (is_power) power_on = value->val.b;
    if (is_level && !value->is_null()) light_level = value->val.u8;
    portEXIT_CRITICAL(&state_lock);
    return ESP_OK;
}

esp_err_t identify(identification::callback_type_t type, uint16_t endpoint_id,
                   uint8_t effect, uint8_t, void *)
{
    if (endpoint_id != light_endpoint_id) return ESP_OK;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&state_lock);
    if (type == identification::START) identifying = true;
    if (type == identification::STOP) { identifying = false; effect_until = 0; }
    if (type == identification::EFFECT) {
        if (effect == static_cast<uint8_t>(Identify::EffectIdentifierEnum::kStopEffect)) {
            effect_until = 0;
        } else if (effect == static_cast<uint8_t>(Identify::EffectIdentifierEnum::kFinishEffect)) {
            if (effect_until > now + 500000) effect_until = now + 500000;
        } else {
            effect_until = now + 2000000;
        }
    }
    portEXIT_CRITICAL(&state_lock);
    return ESP_OK;
}

void matter_event(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t)
{
    using namespace chip::DeviceLayer;
    switch (event->Type) {
    case DeviceEventType::kServerReady:
        if (server_startup_events) xEventGroupSetBits(server_startup_events, SERVER_READY_BIT);
        break;
    case DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Matter commissioned; dimmable display endpoint %u", light_endpoint_id);
        break;
    case DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Pair Angst using the generated onboarding.txt or qrcode.png");
        break;
    case DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "Commissioning timed out; retry using the generated pairing code");
        break;
    case DeviceEventType::kFabricRemoved:
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            auto &manager = chip::Server::GetInstance().GetCommissioningWindowManager();
            if (!manager.IsCommissioningWindowOpen()) {
                CHIP_ERROR error = manager.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(900),
                    chip::CommissioningWindowAdvertisement::kAllSupported);
                if (error != CHIP_NO_ERROR) ESP_LOGE(TAG, "Reopen pairing window: %" CHIP_ERROR_FORMAT, error.Format());
            }
        }
        break;
    default: break;
    }
}
} // namespace

esp_err_t matter_display_start(void)
{
    if (esp_matter::is_started()) return ESP_ERR_INVALID_STATE;
    esp_err_t provisioning_error = load_provisioned_identity();
    if (provisioning_error != ESP_OK) {
        ESP_LOGE(TAG, "Cannot load the original Matter pairing identity: %s", esp_err_to_name(provisioning_error));
        return provisioning_error;
    }
#if CONFIG_EXAMPLE_DAC_PROVIDER
    if (provisioned_identity.vendor_id != CONFIG_DEVICE_VENDOR_ID ||
        provisioned_identity.product_id != CONFIG_DEVICE_PRODUCT_ID) {
        ESP_LOGE(TAG, "Saved Matter identity does not match this firmware's attestation credentials");
        return ESP_ERR_INVALID_STATE;
    }
#endif
    CHIP_ERROR identity_error = initialize_identity_storage();
    if (identity_error != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Matter identity storage: %" CHIP_ERROR_FORMAT, identity_error.Format());
        return ESP_FAIL;
    }
    esp_matter::set_custom_commissionable_data_provider(&commissioning_data);
    esp_matter::set_custom_device_instance_info_provider(&device_information);

    node::config_t node_config;
    std::strcpy(node_config.root_node.basic_information.node_label, DISPLAY_MATTER_NODE_LABEL);
    std::strcpy(node_config.root_node.basic_information.unique_id, provisioned_identity.serial_number);
    node_t *node = node::create(&node_config, attribute_changed, identify);
    if (!node) return ESP_ERR_NO_MEM;
    cluster_t *basic = cluster::get(uint16_t{0}, BasicInformation::Id);
    if (!basic ||
        !cluster::basic_information::attribute::create_serial_number(basic, nullptr, 0) ||
        !cluster::basic_information::attribute::create_product_label(basic, nullptr, 0) ||
        !cluster::basic_information::attribute::create_part_number(basic, nullptr, 0) ||
        !cluster::basic_information::attribute::create_product_url(basic, nullptr, 0)) return ESP_ERR_NO_MEM;

    endpoint::dimmable_light::config_t config;
    config.on_off.on_off = true;
    config.on_off_lighting.start_up_on_off = nullptr; // Restore on/off across boots.
    config.level_control.current_level = 254;
    config.level_control.on_level = nullptr; // Turning on restores the previous dim level.
    config.level_control_lighting.start_up_current_level = nullptr;
    endpoint_t *light = endpoint::dimmable_light::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
    if (!light) return ESP_ERR_NO_MEM;
    light_endpoint_id = endpoint::get_id(light);
    attribute_t *level_attribute = attribute::get(light_endpoint_id, LevelControl::Id,
                                                  LevelControl::Attributes::CurrentLevel::Id);
    if (!level_attribute) return ESP_FAIL;
    ESP_ERROR_CHECK(attribute::set_deferred_persistence(level_attribute));

    // Keep the event group alive for the registered callback's lifetime. A
    // readiness event can arrive before start() returns or while we wait.
    server_startup_events = xEventGroupCreate();
    if (!server_startup_events) return ESP_ERR_NO_MEM;
    esp_err_t error = esp_matter::start(matter_event);
    if (error != ESP_OK) return error;
    // The pinned SDK can return ESP_OK after Server::Init() fails. Require its
    // actual completion event before the caller can confirm a pending OTA.
    // Wait without the CHIP lock so the event task can deliver readiness.
    EventBits_t events = xEventGroupWaitBits(server_startup_events, SERVER_READY_BIT,
        pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (!(events & SERVER_READY_BIT)) {
        ESP_LOGE(TAG, "Matter server initialization did not complete within 30 seconds");
        return ESP_ERR_TIMEOUT;
    }
    // Read the persisted attributes with the CHIP lock before the GUI consumes them.
    {
        lock::ScopedChipStackLock lock(portMAX_DELAY);
        esp_matter_attr_val_t value;
        ESP_ERROR_CHECK(attribute::get_val(level_attribute, &value));
        portENTER_CRITICAL(&state_lock);
        if (!value.is_null()) light_level = value.val.u8;
        portEXIT_CRITICAL(&state_lock);
        attribute_t *power = attribute::get(light_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
        ESP_ERROR_CHECK(attribute::get_val(power, &value));
        portENTER_CRITICAL(&state_lock);
        power_on = value.val.b;
        portEXIT_CRITICAL(&state_lock);
    }
#if CONFIG_ENABLE_CHIP_SHELL
    ESP_ERROR_CHECK(esp_matter::console::diagnostics_register_commands());
    ESP_ERROR_CHECK(esp_matter::console::wifi_register_commands());
    ESP_ERROR_CHECK(esp_matter::console::factoryreset_register_commands());
    ESP_ERROR_CHECK(esp_matter::console::init());
#endif
    ESP_LOGI(TAG, "%s / %s, firmware %s (%lu), dimmable light endpoint %u",
        DISPLAY_MATTER_VENDOR_NAME, DISPLAY_MATTER_PRODUCT_NAME, DISPLAY_MATTER_SOFTWARE_VERSION_STRING,
        (unsigned long)DISPLAY_MATTER_SOFTWARE_VERSION, light_endpoint_id);
    identity_ready = true;
    return ESP_OK;
}

esp_err_t matter_display_prepare_ota(void)
{
    if (!identity_ready) return ESP_ERR_INVALID_STATE;
    matter_provisioning_identity_t saved{};
    const esp_err_t error = matter_provisioning_load(nullptr, false, &saved);
    if (error != ESP_OK) return error;
    return same_identity(saved, provisioned_identity) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

uint8_t matter_display_brightness(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&state_lock);
    bool on = power_on;
    uint8_t level = light_level;
    bool blink = identifying || now < effect_until;
    portEXIT_CRITICAL(&state_lock);
    // Identify flashes the panel without changing or persisting the user's dim level.
    if (blink) return (now / 500000) % 2 ? 255 : 0;
    return on ? static_cast<uint8_t>((static_cast<unsigned>(level) * 255 + 127) / 254) : 0;
}
