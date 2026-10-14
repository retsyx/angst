#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#define CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER 1
#define CONFIG_CUSTOM_DEVICE_INSTANCE_INFO_PROVIDER 1
#define CONFIG_ENABLE_TEST_SETUP_PARAMS 0
#define CONFIG_CUSTOM_NETWORK_CONFIG 0
#define CONFIG_ENABLE_CHIPOBLE 1
#define CONFIG_ENABLE_WIFI_STATION 1
#define CONFIG_ENABLE_CHIP_SHELL 0

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
constexpr esp_err_t ESP_ERR_NOT_FOUND = 0x105;
constexpr esp_err_t ESP_ERR_NO_MEM = 0x101;
constexpr esp_err_t ESP_ERR_TIMEOUT = 0x107;
constexpr esp_err_t ESP_ERR_INVALID_RESPONSE = 0x108;
#define ESP_ERROR_CHECK(call) assert((call) == ESP_OK)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
using portMUX_TYPE = unsigned;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portMAX_DELAY UINT32_MAX
#define BIT0 1u
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(milliseconds) (milliseconds)
#define portENTER_CRITICAL(lock) (++*(lock))
#define portEXIT_CRITICAL(lock) (--*(lock))
int64_t esp_timer_get_time();

using EventBits_t = uint32_t;
using TickType_t = uint32_t;
struct MockEventGroup { EventBits_t bits = 0; };
using EventGroupHandle_t = MockEventGroup *;
EventGroupHandle_t xEventGroupCreate();
EventBits_t xEventGroupSetBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t, EventBits_t, int, int, TickType_t);

class CHIP_ERROR {
public:
    constexpr explicit CHIP_ERROR(int value = 0) : code(value) {}
    constexpr bool operator!=(CHIP_ERROR other) const { return code != other.code; }
    constexpr bool operator==(CHIP_ERROR other) const { return code == other.code; }
    int Format() const { return code; }
private:
    int code;
};
constexpr CHIP_ERROR CHIP_NO_ERROR;
constexpr CHIP_ERROR CHIP_ERROR_BUFFER_TOO_SMALL(1);
constexpr CHIP_ERROR CHIP_ERROR_NOT_IMPLEMENTED(2);
constexpr CHIP_ERROR CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND(3);
constexpr CHIP_ERROR CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND(4);
constexpr CHIP_ERROR CHIP_ERROR_INVALID_ARGUMENT(5);
constexpr CHIP_ERROR CHIP_ERROR_INTERNAL(6);
#define CHIP_ERROR_FORMAT "d"
#define ReturnErrorOnFailure(call) do { CHIP_ERROR result = (call); \
    if (result != CHIP_NO_ERROR) return result; } while (0)
#define VerifyOrReturnError(condition, error) do { if (!(condition)) return (error); } while (0)

namespace chip {
struct ByteSpan {
    const uint8_t *data;
    size_t size;
    template<size_t N> explicit ByteSpan(const uint8_t (&bytes)[N]) : data(bytes), size(N) {}
};
struct MutableByteSpan {
    uint8_t *data;
    size_t size;
    template<size_t N> explicit MutableByteSpan(uint8_t (&bytes)[N]) : data(bytes), size(N) {}
};
struct CharSpan {
    const char *data;
    size_t size;
    static CharSpan fromCharString(const char *value) { return { value, std::strlen(value) }; }
};
inline CHIP_ERROR CopySpanToMutableSpan(ByteSpan source, MutableByteSpan &target)
{
    if (source.size > target.size) return CHIP_ERROR_BUFFER_TOO_SMALL;
    std::memcpy(target.data, source.data, source.size);
    target.size = source.size;
    return CHIP_NO_ERROR;
}
namespace DeviceLayer {
class ConfigurationManager {
public:
    static constexpr size_t kMaxUniqueIDLength = 32;
    CHIP_ERROR GetUniqueId(char *, size_t);
    CHIP_ERROR StoreUniqueId(const char *, size_t);
};
ConfigurationManager &ConfigurationMgr();
namespace PersistedStorage {
struct KeyValueStoreManager {};
KeyValueStoreManager &KeyValueStoreMgr();
}
class CommissionableDataProvider {
public:
    virtual ~CommissionableDataProvider() = default;
    virtual CHIP_ERROR GetSetupDiscriminator(uint16_t &) = 0;
    virtual CHIP_ERROR SetSetupDiscriminator(uint16_t) = 0;
    virtual CHIP_ERROR GetSpake2pIterationCount(uint32_t &) = 0;
    virtual CHIP_ERROR GetSpake2pSalt(MutableByteSpan &) = 0;
    virtual CHIP_ERROR GetSpake2pVerifier(MutableByteSpan &, size_t &) = 0;
    virtual CHIP_ERROR GetSetupPasscode(uint32_t &) = 0;
    virtual CHIP_ERROR SetSetupPasscode(uint32_t) = 0;
};
class DeviceInstanceInfoProvider {
public:
    virtual ~DeviceInstanceInfoProvider() = default;
    virtual CHIP_ERROR GetVendorName(char *, size_t) = 0;
    virtual CHIP_ERROR GetVendorId(uint16_t &) = 0;
    virtual CHIP_ERROR GetProductName(char *, size_t) = 0;
    virtual CHIP_ERROR GetProductId(uint16_t &) = 0;
    virtual CHIP_ERROR GetPartNumber(char *, size_t) = 0;
    virtual CHIP_ERROR GetProductURL(char *, size_t) = 0;
    virtual CHIP_ERROR GetProductLabel(char *, size_t) = 0;
    virtual CHIP_ERROR GetSerialNumber(char *, size_t) = 0;
    virtual CHIP_ERROR GetManufacturingDate(uint16_t &, uint8_t &, uint8_t &) = 0;
    virtual CHIP_ERROR GetHardwareVersion(uint16_t &) = 0;
    virtual CHIP_ERROR GetHardwareVersionString(char *, size_t) = 0;
    virtual CHIP_ERROR GetRotatingDeviceIdUniqueId(MutableByteSpan &) = 0;
};
enum class DeviceEventType { kCommissioningComplete, kCommissioningWindowOpened,
                           kFailSafeTimerExpired, kFabricRemoved, kServerReady };
struct ChipDeviceEvent { DeviceEventType Type; };
}
namespace app::Clusters {
namespace BasicInformation {
constexpr uint32_t Id = 0x28;
namespace Attributes::NodeLabel {
constexpr uint32_t Id = 5;
struct TypeInfo { static constexpr size_t MaxLength() { return 32; } };
}
}
namespace LevelControl {
constexpr uint32_t Id = 8;
namespace Attributes::CurrentLevel { constexpr uint32_t Id = 0; }
}
namespace OnOff {
constexpr uint32_t Id = 6;
namespace Attributes::OnOff { constexpr uint32_t Id = 0; }
}
namespace Identify {
enum class EffectIdentifierEnum : uint8_t { kBlink = 0, kFinishEffect = 0xFE, kStopEffect = 0xFF };
}
}
class KvsPersistentStorageDelegate {
public:
    CHIP_ERROR Init(DeviceLayer::PersistedStorage::KeyValueStoreManager *manager)
    { assert(manager); initialized = true; return CHIP_NO_ERROR; }
    bool initialized = false;
};
namespace app {
struct ConcreteAttributePath {
    uint16_t mEndpointId;
    uint32_t mClusterId;
    uint32_t mAttributeId;
    ConcreteAttributePath(uint16_t endpoint, uint32_t cluster, uint32_t attribute)
        : mEndpointId(endpoint), mClusterId(cluster), mAttributeId(attribute) {}
};
class DefaultAttributePersistenceProvider {
public:
    CHIP_ERROR Init(KvsPersistentStorageDelegate *storage)
    { assert(storage && storage->initialized); initialized = true; return CHIP_NO_ERROR; }
    CHIP_ERROR ReadValue(const ConcreteAttributePath &, MutableByteSpan &);
    bool initialized = false;
};
namespace Storage {
template<size_t N> class String {
public:
    bool SetContent(CharSpan content)
    {
        if (content.size > N) return false;
        std::memcpy(value, content.data, content.size);
        value[content.size] = 0;
        return true;
    }
    const char *c_str() const { return value; }
private:
    char value[N + 1] = {};
};
}
class AttributePersistence {
public:
    explicit AttributePersistence(DefaultAttributePersistenceProvider &provider)
    { assert(provider.initialized); }
    template<size_t N> CHIP_ERROR StoreString(const ConcreteAttributePath &path, const Storage::String<N> &value)
    { return StoreLabel(path, value.c_str()); }
private:
    CHIP_ERROR StoreLabel(const ConcreteAttributePath &, const char *);
};
}
namespace System::Clock { struct Seconds16 { explicit Seconds16(uint16_t) {} }; }
enum class CommissioningWindowAdvertisement { kAllSupported };
class CommissioningWindowManager {
public:
    bool IsCommissioningWindowOpen() const { return true; }
    CHIP_ERROR OpenBasicCommissioningWindow(System::Clock::Seconds16,
                                           CommissioningWindowAdvertisement) { return CHIP_NO_ERROR; }
};
class FabricTable { public: unsigned FabricCount() const { return 1; } };
class Server {
public:
    static Server &GetInstance() { static Server instance; return instance; }
    FabricTable &GetFabricTable() { return fabrics; }
    CommissioningWindowManager &GetCommissioningWindowManager() { return window; }
private:
    FabricTable fabrics;
    CommissioningWindowManager window;
};
}

struct esp_matter_attr_val_t {
    union { uint8_t u8; bool b; } val = {};
    bool null = false;
    bool is_null() const { return null; }
};

namespace esp_matter {
struct node_t {};
struct cluster_t {};
struct endpoint_t { uint16_t id = 1; };
struct attribute_t { esp_matter_attr_val_t value; bool deferred = false; };
constexpr unsigned ENDPOINT_FLAG_NONE = 0;
namespace cluster {
cluster_t *get(uint16_t, uint32_t);
namespace basic_information::attribute {
attribute_t *create_serial_number(cluster_t *, const char *, uint16_t);
attribute_t *create_product_label(cluster_t *, const char *, uint16_t);
attribute_t *create_part_number(cluster_t *, const char *, uint16_t);
attribute_t *create_product_url(cluster_t *, const char *, uint16_t);
}
}
namespace attribute {
enum callback_type_t { PRE_UPDATE, POST_UPDATE };
using callback_t = esp_err_t (*)(callback_type_t, uint16_t, uint32_t, uint32_t, esp_matter_attr_val_t *, void *);
attribute_t *get(uint16_t, uint32_t, uint32_t);
esp_err_t get_val(attribute_t *, esp_matter_attr_val_t *);
esp_err_t set_deferred_persistence(attribute_t *);
}
namespace identification {
enum callback_type_t { START, STOP, EFFECT };
using callback_t = esp_err_t (*)(callback_type_t, uint16_t, uint8_t, uint8_t, void *);
}
namespace node {
struct config_t {
    struct {
        struct { char node_label[33] = {}; char unique_id[33] = {}; } basic_information;
    } root_node;
};
node_t *create(config_t *, attribute::callback_t, identification::callback_t);
}
struct nullable {
    uint8_t value = 0;
    bool null = true;
    nullable &operator=(uint8_t v) { value = v; null = false; return *this; }
    nullable &operator=(std::nullptr_t) { null = true; return *this; }
};
namespace endpoint {
inline uint16_t get_id(endpoint_t *endpoint) { return endpoint->id; }
namespace dimmable_light {
struct config_t {
    struct { bool on_off = false; } on_off;
    struct { nullable start_up_on_off; } on_off_lighting;
    struct { nullable current_level; nullable on_level; } level_control;
    struct { nullable start_up_current_level; } level_control_lighting;
};
endpoint_t *create(node_t *, config_t *, unsigned, void *);
}
}
namespace lock {
class ScopedChipStackLock {
public:
    explicit ScopedChipStackLock(uint32_t);
    ~ScopedChipStackLock();
};
}
bool is_started();
void set_custom_commissionable_data_provider(chip::DeviceLayer::CommissionableDataProvider *);
void set_custom_device_instance_info_provider(chip::DeviceLayer::DeviceInstanceInfoProvider *);
esp_err_t start(void (*)(const chip::DeviceLayer::ChipDeviceEvent *, intptr_t));
}

#define DISPLAY_MATTER_DISCRIMINATOR 1
#define DISPLAY_MATTER_SPAKE2P_ITERATIONS 10000
constexpr uint8_t DISPLAY_MATTER_SPAKE2P_SALT[32] = { 1 };
constexpr uint8_t DISPLAY_MATTER_SPAKE2P_VERIFIER[97] = { 2 };
constexpr uint8_t DISPLAY_MATTER_ROTATING_ID[16] = { 3 };
#define DISPLAY_MATTER_VENDOR_NAME "Test Manufacturer"
#define DISPLAY_MATTER_VENDOR_ID 0xFFF1
#define DISPLAY_MATTER_PRODUCT_NAME "Test Display"
#define DISPLAY_MATTER_PRODUCT_ID 0x8000
#define DISPLAY_MATTER_NODE_LABEL "Test Display"
#define DISPLAY_MATTER_SERIAL_NUMBER "TEST-1234"
#define DISPLAY_MATTER_HARDWARE_VERSION 1
#define DISPLAY_MATTER_HARDWARE_VERSION_STRING "1.0"
#define DISPLAY_MATTER_SOFTWARE_VERSION 1
#define DISPLAY_MATTER_SOFTWARE_VERSION_STRING "1.0.0"
