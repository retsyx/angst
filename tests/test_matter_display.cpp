#include "matter_mock_idf.h"

#include <cstdio>

#include "../main/matter_display.cpp"

static int64_t mock_time;
static bool started;
static bool stack_locked;
static bool restore_saved;
static bool restore_null;
static bool saved_label_exists;
static char saved_label[33];
static bool saved_unique_id_exists;
static char saved_unique_id[33];
static unsigned label_writes;
static unsigned unique_id_writes;
static bool fail_label_read;
static bool fail_unique_id_read;
enum class StartupMode { ReadyBeforeReturn, ReadyWhileWaiting, NeverReady,
                         UnrelatedEvent, SdkError, SdkErrorAfterReady };
static StartupMode startup_mode = StartupMode::ReadyBeforeReturn;
static bool fail_event_group_allocation;
static MockEventGroup mock_event_group;
static unsigned event_group_creations;
static unsigned readiness_waits;
static unsigned persisted_attribute_reads;
static void (*device_event_callback)(const chip::DeviceLayer::ChipDeviceEvent *, intptr_t);
static esp_matter::node_t mock_node;
static esp_matter::cluster_t mock_basic_information;
static esp_matter::attribute_t mock_information_attributes[4];
static bool information_attributes_created[4];
static esp_matter::endpoint_t mock_endpoint;
static esp_matter::attribute_t mock_level;
static esp_matter::attribute_t mock_power;
static esp_matter::attribute::callback_t update_callback;
static esp_matter::identification::callback_t identify_callback;
static matter_provisioning_identity_t saved_identity;
static bool provisioning_exists = true;
static esp_err_t provisioning_error;
static esp_err_t seed_error;
static bool bootstrap_fresh;
static esp_err_t bootstrap_error;
static unsigned provisioning_reads, provisioning_seeds, bootstrap_checks;
static unsigned provider_registrations, node_creations;
static char created_node_unique_id[33];
static chip::DeviceLayer::CommissionableDataProvider *registered_commissioning;
static chip::DeviceLayer::DeviceInstanceInfoProvider *registered_information;

static void use_persisted_identity_a()
{
    saved_identity.discriminator = 3577;
    saved_identity.iterations = 17000;
    saved_identity.vendor_id = 0xfff2;
    saved_identity.product_id = 0x8001;
    std::strcpy(saved_identity.serial_number, "PERSISTED-IDENTITY-A");
    for (size_t i = 0; i < sizeof(saved_identity.salt); ++i) saved_identity.salt[i] = static_cast<uint8_t>(31 + i);
    for (size_t i = 0; i < sizeof(saved_identity.verifier); ++i) saved_identity.verifier[i] = static_cast<uint8_t>(79 + i);
    for (size_t i = 0; i < sizeof(saved_identity.rotating_id); ++i) saved_identity.rotating_id[i] = static_cast<uint8_t>(127 + i);
}

esp_err_t matter_provisioning_load(const matter_provisioning_identity_t *seed,
                                 bool allow_seed, matter_provisioning_identity_t *identity)
{
    assert(identity && !stack_locked);
    if (seed || allow_seed) {
        assert(seed && allow_seed && !started && !provider_registrations && !node_creations);
        assert(seed->discriminator == DISPLAY_MATTER_DISCRIMINATOR);
        assert(seed->iterations == DISPLAY_MATTER_SPAKE2P_ITERATIONS);
        assert(seed->vendor_id == DISPLAY_MATTER_VENDOR_ID && seed->product_id == DISPLAY_MATTER_PRODUCT_ID);
        assert(!std::strcmp(seed->serial_number, DISPLAY_MATTER_SERIAL_NUMBER));
        assert(!std::memcmp(seed->salt, DISPLAY_MATTER_SPAKE2P_SALT, sizeof(seed->salt)));
        assert(!std::memcmp(seed->verifier, DISPLAY_MATTER_SPAKE2P_VERIFIER, sizeof(seed->verifier)));
        assert(!std::memcmp(seed->rotating_id, DISPLAY_MATTER_ROTATING_ID, sizeof(seed->rotating_id)));
        ++provisioning_seeds;
        if (seed_error) return seed_error;
        assert(!provisioning_exists);
        saved_identity = *seed;
        provisioning_exists = true;
    } else {
        ++provisioning_reads;
        if (provisioning_error) return provisioning_error;
        if (!provisioning_exists) return ESP_ERR_NOT_FOUND;
    }
    *identity = saved_identity;
    return ESP_OK;
}

esp_err_t matter_bootstrap_check(bool *fresh_install)
{
    assert(fresh_install && !started && !provider_registrations && !node_creations);
    ++bootstrap_checks;
    if (bootstrap_error) return bootstrap_error;
    *fresh_install = bootstrap_fresh;
    return ESP_OK;
}

int64_t esp_timer_get_time() { return mock_time; }

static void send_device_event(chip::DeviceLayer::DeviceEventType type)
{
    assert(device_event_callback);
    const chip::DeviceLayer::ChipDeviceEvent event = { type };
    device_event_callback(&event, 0);
}

EventGroupHandle_t xEventGroupCreate()
{
    assert(!started && !stack_locked);
    ++event_group_creations;
    return fail_event_group_allocation ? nullptr : &mock_event_group;
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
    assert(group == &mock_event_group);
    group->bits |= bits;
    return group->bits;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                              int clear_on_exit, int wait_for_all, TickType_t ticks)
{
    // Waiting with either lock would prevent the CHIP task from reporting readiness.
    assert(group == &mock_event_group && !stack_locked && state_lock == 0);
    assert(bits == BIT0 && clear_on_exit == pdFALSE && wait_for_all == pdTRUE);
    assert(ticks > 0 && ticks <= pdMS_TO_TICKS(30000));
    ++readiness_waits;
    if (startup_mode == StartupMode::ReadyWhileWaiting) {
        assert(group->bits == 0);
        send_device_event(chip::DeviceLayer::DeviceEventType::kServerReady);
    }
    if (!(group->bits & bits)) mock_time += static_cast<int64_t>(ticks) * 1000;
    return group->bits;
}

namespace chip {
namespace DeviceLayer {
ConfigurationManager &ConfigurationMgr() { static ConfigurationManager configuration; return configuration; }
CHIP_ERROR ConfigurationManager::GetUniqueId(char *buffer, size_t capacity)
{
    assert(buffer && capacity >= sizeof(saved_unique_id));
    if (fail_unique_id_read) return CHIP_ERROR_INTERNAL;
    if (!saved_unique_id_exists) return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND;
    std::strcpy(buffer, saved_unique_id);
    return CHIP_NO_ERROR;
}
CHIP_ERROR ConfigurationManager::StoreUniqueId(const char *value, size_t size)
{
    assert(!saved_unique_id_exists && size == std::strlen(value) && size < sizeof(saved_unique_id));
    std::strcpy(saved_unique_id, value);
    saved_unique_id_exists = true;
    ++unique_id_writes;
    return CHIP_NO_ERROR;
}
namespace PersistedStorage {
KeyValueStoreManager &KeyValueStoreMgr() { static KeyValueStoreManager storage; return storage; }
}
}
namespace app {
static void expect_label_path(const ConcreteAttributePath &path)
{
    assert(path.mEndpointId == 0 && path.mClusterId == Clusters::BasicInformation::Id &&
           path.mAttributeId == Clusters::BasicInformation::Attributes::NodeLabel::Id);
}
CHIP_ERROR DefaultAttributePersistenceProvider::ReadValue(const ConcreteAttributePath &path, MutableByteSpan &buffer)
{
    expect_label_path(path);
    assert(initialized);
    if (fail_label_read) return CHIP_ERROR_INTERNAL;
    if (!saved_label_exists) return CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND;
    size_t size = std::strlen(saved_label);
    assert(buffer.size >= size + 1);
    // Model the SDK's length-prefixed short-string storage, including empty values.
    buffer.data[0] = static_cast<uint8_t>(size);
    std::memcpy(buffer.data + 1, saved_label, size);
    buffer.size = size + 1;
    return CHIP_NO_ERROR;
}
CHIP_ERROR AttributePersistence::StoreLabel(const ConcreteAttributePath &path, const char *value)
{
    expect_label_path(path);
    assert(!saved_label_exists && std::strlen(value) < sizeof(saved_label));
    std::strcpy(saved_label, value);
    saved_label_exists = true;
    ++label_writes;
    return CHIP_NO_ERROR;
}
}
}

namespace esp_matter {
bool is_started() { return started; }
void set_custom_commissionable_data_provider(chip::DeviceLayer::CommissionableDataProvider *provider)
{
    assert(provider && provisioning_exists && !provisioning_error && !seed_error);
    registered_commissioning = provider;
    ++provider_registrations;
}
void set_custom_device_instance_info_provider(chip::DeviceLayer::DeviceInstanceInfoProvider *provider)
{
    assert(provider && provisioning_exists && !provisioning_error && !seed_error);
    registered_information = provider;
    ++provider_registrations;
}
esp_err_t start(void (*callback)(const chip::DeviceLayer::ChipDeviceEvent *, intptr_t))
{
    assert(callback && !started);
    assert(event_group_creations == 1);
    device_event_callback = callback;
    started = true;
    if (startup_mode == StartupMode::ReadyBeforeReturn ||
        startup_mode == StartupMode::SdkErrorAfterReady) {
        send_device_event(chip::DeviceLayer::DeviceEventType::kServerReady);
    } else if (startup_mode == StartupMode::UnrelatedEvent) {
        send_device_event(chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened);
        send_device_event(chip::DeviceLayer::DeviceEventType::kCommissioningComplete);
    }
    if (startup_mode == StartupMode::SdkError || startup_mode == StartupMode::SdkErrorAfterReady)
        return ESP_FAIL;
    return ESP_OK;
}
namespace node {
node_t *create(config_t *config, attribute::callback_t update, identification::callback_t identify)
{
    assert(config && update && identify);
    assert(provider_registrations == 2 && provisioning_exists);
    std::strcpy(created_node_unique_id, config->root_node.basic_information.unique_id);
    assert(!std::strcmp(created_node_unique_id, saved_identity.serial_number));
    ++node_creations;
    update_callback = update;
    identify_callback = identify;
    return &mock_node;
}
}
namespace cluster {
cluster_t *get(uint16_t endpoint, uint32_t cluster)
{
    assert(endpoint == 0 && cluster == chip::app::Clusters::BasicInformation::Id);
    return &mock_basic_information;
}
namespace basic_information::attribute {
static attribute_t *create_information_attribute(cluster_t *cluster, const char *value,
                                                  uint16_t size, size_t index)
{
    assert(cluster == &mock_basic_information && !value && size == 0);
    assert(!information_attributes_created[index]);
    information_attributes_created[index] = true;
    return &mock_information_attributes[index];
}
attribute_t *create_serial_number(cluster_t *cluster, const char *value, uint16_t size)
{ return create_information_attribute(cluster, value, size, 0); }
attribute_t *create_product_label(cluster_t *cluster, const char *value, uint16_t size)
{ return create_information_attribute(cluster, value, size, 1); }
attribute_t *create_part_number(cluster_t *cluster, const char *value, uint16_t size)
{ return create_information_attribute(cluster, value, size, 2); }
attribute_t *create_product_url(cluster_t *cluster, const char *value, uint16_t size)
{ return create_information_attribute(cluster, value, size, 3); }
}
}
namespace endpoint::dimmable_light {
endpoint_t *create(node_t *node, config_t *config, unsigned flags, void *context)
{
    assert(node && config && flags == ENDPOINT_FLAG_NONE && !context);
    assert(config->on_off.on_off);
    assert(!config->level_control.current_level.null && config->level_control.current_level.value == 254);
    assert(config->on_off_lighting.start_up_on_off.null);
    assert(config->level_control_lighting.start_up_current_level.null);
    assert(config->level_control.on_level.null);
    // ESP-Matter restores nonvolatile values when creating these attributes.
    mock_level.value.val.u8 = restore_saved ? 127 : config->level_control.current_level.value;
    mock_level.value.null = restore_null;
    mock_power.value.val.b = !restore_saved;
    return &mock_endpoint;
}
}
namespace attribute {
attribute_t *get(uint16_t endpoint, uint32_t cluster, uint32_t attribute)
{
    assert(endpoint == 1 && attribute == 0);
    if (cluster == chip::app::Clusters::LevelControl::Id) return &mock_level;
    assert(cluster == chip::app::Clusters::OnOff::Id);
    return &mock_power;
}
esp_err_t get_val(attribute_t *attribute, esp_matter_attr_val_t *value)
{
    assert(stack_locked && attribute && value);
    assert(mock_event_group.bits & BIT0);
    ++persisted_attribute_reads;
    *value = attribute->value;
    return ESP_OK;
}
esp_err_t set_deferred_persistence(attribute_t *attribute)
{
    assert(attribute == &mock_level);
    attribute->deferred = true;
    return ESP_OK;
}
}
namespace lock {
ScopedChipStackLock::ScopedChipStackLock(uint32_t ticks)
{
    assert(ticks == portMAX_DELAY && !stack_locked);
    stack_locked = true;
}
ScopedChipStackLock::~ScopedChipStackLock() { stack_locked = false; }
}
}

static esp_err_t set_level(uint8_t level, bool null = false,
                          esp_matter::attribute::callback_type_t type = esp_matter::attribute::POST_UPDATE,
                          uint16_t endpoint = 1)
{
    esp_matter_attr_val_t value;
    value.val.u8 = level;
    value.null = null;
    return update_callback(type, endpoint, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &value, nullptr);
}

static void set_power(bool on,
                      esp_matter::attribute::callback_type_t type = esp_matter::attribute::POST_UPDATE)
{
    esp_matter_attr_val_t value;
    value.val.b = on;
    assert(update_callback(type, 1, OnOff::Id,
                           OnOff::Attributes::OnOff::Id, &value, nullptr) == ESP_OK);
}

static void effect(Identify::EffectIdentifierEnum identifier)
{
    assert(identify_callback(esp_matter::identification::EFFECT, 1,
                             static_cast<uint8_t>(identifier), 0, nullptr) == ESP_OK);
}

static void check_identity_providers()
{
    assert(registered_commissioning && registered_information && provider_registrations == 2);
    uint16_t discriminator = 0;
    uint32_t iterations = 0;
    assert(registered_commissioning->GetSetupDiscriminator(discriminator) == CHIP_NO_ERROR);
    assert(discriminator == saved_identity.discriminator);
    assert(registered_commissioning->SetSetupDiscriminator(5) == CHIP_ERROR_NOT_IMPLEMENTED);
    assert(registered_commissioning->GetSpake2pIterationCount(iterations) == CHIP_NO_ERROR);
    assert(iterations == saved_identity.iterations);
    uint8_t bytes[128]{};
    chip::MutableByteSpan salt(bytes);
    assert(registered_commissioning->GetSpake2pSalt(salt) == CHIP_NO_ERROR);
    assert(salt.size == sizeof(saved_identity.salt));
    assert(!std::memcmp(bytes, saved_identity.salt, salt.size));
    chip::MutableByteSpan verifier(bytes);
    size_t verifier_length = 0;
    assert(registered_commissioning->GetSpake2pVerifier(verifier, verifier_length) == CHIP_NO_ERROR);
    assert(verifier_length == sizeof(saved_identity.verifier) && verifier.size == verifier_length);
    assert(!std::memcmp(bytes, saved_identity.verifier, verifier_length));
    uint8_t tiny_bytes[1]{0x55};
    chip::MutableByteSpan tiny(tiny_bytes);
    assert(registered_commissioning->GetSpake2pSalt(tiny) == CHIP_ERROR_BUFFER_TOO_SMALL);
    assert(registered_commissioning->GetSpake2pVerifier(tiny, verifier_length) == CHIP_ERROR_BUFFER_TOO_SMALL);
    assert(tiny_bytes[0] == 0x55);
    uint32_t passcode = 123;
    assert(registered_commissioning->GetSetupPasscode(passcode) == CHIP_ERROR_NOT_IMPLEMENTED);
    assert(passcode == 123);
    assert(registered_commissioning->SetSetupPasscode(passcode) == CHIP_ERROR_NOT_IMPLEMENTED);

    char text[128];
    uint16_t value = 0;
    assert(registered_information->GetVendorName(text, sizeof(text)) == CHIP_NO_ERROR);
    assert(!std::strcmp(text, DISPLAY_MATTER_VENDOR_NAME));
    assert(registered_information->GetVendorId(value) == CHIP_NO_ERROR && value == saved_identity.vendor_id);
    assert(registered_information->GetProductName(text, sizeof(text)) == CHIP_NO_ERROR);
    assert(!std::strcmp(text, DISPLAY_MATTER_PRODUCT_NAME));
    assert(registered_information->GetProductId(value) == CHIP_NO_ERROR && value == saved_identity.product_id);
    assert(registered_information->GetPartNumber(text, sizeof(text)) == CHIP_NO_ERROR);
    assert(!std::strcmp(text, "Angst-AMOLED-S3"));
    assert(registered_information->GetProductURL(text, sizeof(text)) == CHIP_NO_ERROR);
    assert(!std::strcmp(text, "https://github.com/retsyx/angst"));
    assert(registered_information->GetProductLabel(text, sizeof(text)) == CHIP_NO_ERROR);
    assert(!std::strcmp(text, DISPLAY_MATTER_NODE_LABEL));
    assert(registered_information->GetSerialNumber(text, sizeof(text)) == CHIP_NO_ERROR);
    assert(!std::strcmp(text, saved_identity.serial_number));
    std::strcpy(text, "unchanged");
    assert(registered_information->GetSerialNumber(text, 1) == CHIP_ERROR_BUFFER_TOO_SMALL);
    assert(!std::strcmp(text, "unchanged"));
    uint16_t year = 0;
    uint8_t month = 0, day = 0;
    assert(registered_information->GetManufacturingDate(year, month, day) == CHIP_ERROR_NOT_IMPLEMENTED);
    assert(registered_information->GetHardwareVersion(value) == CHIP_NO_ERROR && value == DISPLAY_MATTER_HARDWARE_VERSION);
    assert(registered_information->GetHardwareVersionString(text, sizeof(text)) == CHIP_NO_ERROR);
    assert(!std::strcmp(text, DISPLAY_MATTER_HARDWARE_VERSION_STRING));
    chip::MutableByteSpan rotating(bytes);
    assert(registered_information->GetRotatingDeviceIdUniqueId(rotating) == CHIP_NO_ERROR);
    assert(rotating.size == sizeof(saved_identity.rotating_id));
    assert(!std::memcmp(bytes, saved_identity.rotating_id, rotating.size));
}

static void check_ota_identity_guard()
{
    const matter_provisioning_identity_t original = saved_identity;
    const unsigned seeds_before = provisioning_seeds;
    const unsigned bootstrap_before = bootstrap_checks;
    assert(matter_display_prepare_ota() == ESP_OK);
    ++saved_identity.discriminator;
    assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
    saved_identity = original;
    ++saved_identity.iterations;
    assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
    saved_identity = original;
    ++saved_identity.vendor_id;
    assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
    saved_identity = original;
    ++saved_identity.product_id;
    assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
    saved_identity = original;
    saved_identity.serial_number[0] ^= 1;
    assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
    saved_identity = original;
    saved_identity.salt[sizeof(saved_identity.salt) - 1] ^= 1;
    assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
    saved_identity = original;
    saved_identity.verifier[sizeof(saved_identity.verifier) - 1] ^= 1;
    assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
    saved_identity = original;
    saved_identity.rotating_id[sizeof(saved_identity.rotating_id) - 1] ^= 1;
    assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
    saved_identity = original;
    provisioning_exists = false;
    assert(matter_display_prepare_ota() == ESP_ERR_NOT_FOUND);
    provisioning_exists = true;
    provisioning_error = ESP_ERR_INVALID_RESPONSE;
    assert(matter_display_prepare_ota() == ESP_ERR_INVALID_RESPONSE);
    provisioning_error = ESP_FAIL;
    assert(matter_display_prepare_ota() == ESP_FAIL);
    provisioning_error = ESP_OK;
    assert(matter_display_prepare_ota() == ESP_OK);
    assert(provisioning_seeds == seeds_before && bootstrap_checks == bootstrap_before);
    check_identity_providers();
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const auto is = [argv](const char *scenario) { return std::strcmp(argv[1], scenario) == 0; };
    use_persisted_identity_a();
    restore_saved = std::strcmp(argv[1], "restored") == 0;
    restore_null = std::strcmp(argv[1], "null") == 0;
    bool empty_label = std::strcmp(argv[1], "empty_label") == 0;
    fail_label_read = std::strcmp(argv[1], "label_read_error") == 0;
    fail_unique_id_read = std::strcmp(argv[1], "unique_id_read_error") == 0;
    bool label_only = std::strcmp(argv[1], "saved_label") == 0;
    bool unique_id_only = std::strcmp(argv[1], "saved_unique_id") == 0;
    if (std::strcmp(argv[1], "ready_while_waiting") == 0) startup_mode = StartupMode::ReadyWhileWaiting;
    if (std::strcmp(argv[1], "server_never_ready") == 0) startup_mode = StartupMode::NeverReady;
    if (std::strcmp(argv[1], "unrelated_startup_event") == 0) startup_mode = StartupMode::UnrelatedEvent;
    if (std::strcmp(argv[1], "sdk_start_error") == 0) startup_mode = StartupMode::SdkError;
    if (std::strcmp(argv[1], "sdk_error_after_ready") == 0) startup_mode = StartupMode::SdkErrorAfterReady;
    fail_event_group_allocation = std::strcmp(argv[1], "event_group_allocation_error") == 0;
    saved_label_exists = restore_saved || empty_label || label_only;
    if (saved_label_exists && !empty_label) std::strcpy(saved_label, "Bedroom Price");
    saved_unique_id_exists = restore_saved || empty_label || unique_id_only || fail_label_read;
    if (saved_unique_id_exists) std::strcpy(saved_unique_id, "existing-device-uid");
    if (is("fresh") || is("seed_error")) {
        provisioning_exists = false;
        bootstrap_fresh = true;
    }
    if (is("provisioning_corrupt")) provisioning_error = ESP_ERR_INVALID_RESPONSE;
    if (is("provisioning_error")) provisioning_error = ESP_FAIL;
    if (is("seed_error")) seed_error = ESP_FAIL;
    if (is("bootstrap_error")) {
        provisioning_exists = false;
        bootstrap_error = ESP_FAIL;
    }
    if (is("existing_identity_absent") || is("existing_id_even_fresh")) {
        provisioning_exists = false;
        saved_unique_id_exists = true;
        std::strcpy(saved_unique_id, "EXISTING-DEVICE-ID");
        bootstrap_fresh = is("existing_id_even_fresh");
    }
    if (is("existing_identity_absent_after_reset")) {
        provisioning_exists = false;
        saved_unique_id_exists = false;
    }
    if (is("existing_record_overrides")) {
        saved_unique_id_exists = true;
        std::strcpy(saved_unique_id, "existing-device-uid");
    }
    bool had_label = saved_label_exists;
    bool had_unique_id = saved_unique_id_exists;
    bool had_provisioning = provisioning_exists;
    assert(matter_display_brightness() == 255);
    assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
    assert(!provisioning_reads && !provisioning_seeds && !bootstrap_checks);
    esp_err_t expected_identity_failure = provisioning_error;
    if (!had_provisioning) {
        if (bootstrap_error) {
            expected_identity_failure = bootstrap_error;
        } else if (!bootstrap_fresh || saved_unique_id_exists) {
            expected_identity_failure = ESP_ERR_INVALID_STATE;
        }
        if (!expected_identity_failure && seed_error) expected_identity_failure = seed_error;
    }
    if (expected_identity_failure) {
        assert(matter_display_start() == expected_identity_failure && !started);
        assert(!label_writes && !unique_id_writes && !provider_registrations && !node_creations);
        assert(!event_group_creations && !readiness_waits && !persisted_attribute_reads);
        assert(provisioning_exists == had_provisioning);
        assert(provisioning_reads == 1);
        assert(provisioning_seeds == (is("seed_error") ? 1u : 0u));
        assert(bootstrap_checks == (had_provisioning ? 0u : 1u));
        assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
        std::printf("Matter display tests passed: %s\n", argv[1]);
        return 0;
    }
    if (fail_label_read || fail_unique_id_read) {
        assert(matter_display_start() == ESP_FAIL && !started);
        assert(!label_writes && !unique_id_writes);
        assert(!event_group_creations && !readiness_waits && !persisted_attribute_reads);
        assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
        std::printf("Matter display tests passed: %s\n", argv[1]);
        return 0;
    }
    if (fail_event_group_allocation) {
        assert(matter_display_start() == ESP_ERR_NO_MEM && !started);
        assert(event_group_creations == 1 && !readiness_waits && !persisted_attribute_reads);
        assert(!device_event_callback && !stack_locked);
        assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
        std::printf("Matter display tests passed: %s\n", argv[1]);
        return 0;
    }
    if (startup_mode == StartupMode::SdkError || startup_mode == StartupMode::SdkErrorAfterReady) {
        assert(matter_display_start() == ESP_FAIL);
        assert(!readiness_waits && !persisted_attribute_reads && !stack_locked);
        assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
        std::printf("Matter display tests passed: %s\n", argv[1]);
        return 0;
    }
    if (startup_mode == StartupMode::NeverReady || startup_mode == StartupMode::UnrelatedEvent) {
        // Model the SDK returning success even when Server::Init() failed.
        assert(matter_display_start() == ESP_ERR_TIMEOUT);
        assert(started && readiness_waits == 1 && !persisted_attribute_reads && !stack_locked);
        assert(mock_event_group.bits == 0 && mock_time > 0);
        // A callback arriving after the timeout must still have a live event group.
        send_device_event(chip::DeviceLayer::DeviceEventType::kServerReady);
        assert(mock_event_group.bits == BIT0 && !persisted_attribute_reads);
        assert(matter_display_prepare_ota() == ESP_ERR_INVALID_STATE);
        std::printf("Matter display tests passed: %s\n", argv[1]);
        return 0;
    }
    assert(matter_display_start() == ESP_OK);
    assert(provisioning_reads == 1 && provisioning_seeds == (had_provisioning ? 0u : 1u));
    assert(bootstrap_checks == (had_provisioning ? 0u : 1u));
    check_identity_providers();
    check_ota_identity_guard();
    assert(readiness_waits == 1 && persisted_attribute_reads == 2);
    assert(saved_label_exists && saved_unique_id_exists);
    assert(label_writes == (had_label ? 0u : 1u));
    assert(unique_id_writes == (had_unique_id ? 0u : 1u));
    assert(std::strcmp(saved_label, had_label ? (empty_label ? "" : "Bedroom Price") : DISPLAY_MATTER_NODE_LABEL) == 0);
    assert(std::strcmp(saved_unique_id, had_unique_id ? "existing-device-uid" : saved_identity.serial_number) == 0);
    assert(!std::strcmp(created_node_unique_id, saved_identity.serial_number));
    for (bool created : information_attributes_created) assert(created);
    assert(mock_level.deferred && !stack_locked);
    assert(matter_display_brightness() == (restore_saved ? 0 : 255));
    if (restore_saved) {
        set_power(true);
        assert(matter_display_brightness() == 128);
    }
    assert(matter_display_start() == ESP_ERR_INVALID_STATE);

    assert(set_level(127) == ESP_OK && matter_display_brightness() == 128);
    assert(set_level(64, false, esp_matter::attribute::PRE_UPDATE) == ESP_OK);
    assert(matter_display_brightness() == 128);
    assert(set_level(255, false, esp_matter::attribute::PRE_UPDATE) == ESP_ERR_INVALID_ARG);
    assert(set_level(255, true) == ESP_OK && matter_display_brightness() == 128);
    assert(set_level(32, false, esp_matter::attribute::POST_UPDATE, 2) == ESP_OK);
    assert(matter_display_brightness() == 128);
    set_power(false, esp_matter::attribute::PRE_UPDATE);
    assert(matter_display_brightness() == 128);
    assert(update_callback(esp_matter::attribute::POST_UPDATE, 1, LevelControl::Id,
                           LevelControl::Attributes::CurrentLevel::Id, nullptr, nullptr) == ESP_OK);
    assert(matter_display_brightness() == 128);

    set_power(false);
    assert(matter_display_brightness() == 0);
    set_power(true);
    assert(matter_display_brightness() == 128);
    set_power(false);
    assert(set_level(64) == ESP_OK && matter_display_brightness() == 0);
    set_power(true);
    assert(matter_display_brightness() == 64);
    assert(set_level(254) == ESP_OK && matter_display_brightness() == 255);

    // Identification overrides power temporarily, then restores the saved state.
    set_power(false);
    mock_time = 1000000;
    effect(Identify::EffectIdentifierEnum::kBlink);
    assert(matter_display_brightness() == 0);
    mock_time = 1500000;
    assert(matter_display_brightness() == 255);
    effect(Identify::EffectIdentifierEnum::kStopEffect);
    assert(matter_display_brightness() == 0);
    effect(Identify::EffectIdentifierEnum::kFinishEffect);
    assert(matter_display_brightness() == 0);
    effect(Identify::EffectIdentifierEnum::kBlink);
    effect(Identify::EffectIdentifierEnum::kFinishEffect);
    mock_time += 500000;
    assert(matter_display_brightness() == 0);

    assert(identify_callback(esp_matter::identification::START, 1, 0, 0, nullptr) == ESP_OK);
    mock_time += 500000;
    assert(matter_display_brightness() == 255);
    assert(identify_callback(esp_matter::identification::STOP, 1, 0, 0, nullptr) == ESP_OK);
    assert(matter_display_brightness() == 0);
    set_power(true);
    assert(matter_display_brightness() == 255);
    assert(state_lock == 0);
    std::printf("Matter display tests passed: %s\n", argv[1]);
    return 0;
}
