#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include "h2_ios_platform.h"
#include "h2_ios_corebluetooth_internal.h"

#include <string.h>

#define H2_COREBLUETOOTH_CONN_HANDLE 1u
#define H2_COREBLUETOOTH_WAIT_MS 10000u
#define H2_COREBLUETOOTH_DISCOVERY_MAX 64u
#define H2_COREBLUETOOTH_DEFAULT_ATT_PAYLOAD 20u
#define H2_COREBLUETOOTH_MTU_POLL_MS 10u

struct h2_pal_ble_adv_set {
    h2_pal_ble_adv_params_t params;
};

typedef NS_ENUM(NSInteger, H2CoreBluetoothOperation) {
    H2CoreBluetoothOperationNone = 0,
    H2CoreBluetoothOperationRegisterServices,
    H2CoreBluetoothOperationAdvertise,
    H2CoreBluetoothOperationConnect,
    H2CoreBluetoothOperationDisconnect,
    H2CoreBluetoothOperationDiscoverServices,
    H2CoreBluetoothOperationDiscoverCharacteristics,
    H2CoreBluetoothOperationRead,
    H2CoreBluetoothOperationWrite,
    H2CoreBluetoothOperationSubscribe,
};

@interface H2CoreBluetoothGattBinding : NSObject
@property(nonatomic) uint16_t valueHandle;
@property(nonatomic) uint16_t cccdHandle;
@property(nonatomic) h2_pal_ble_gatt_read_fn readCallback;
@property(nonatomic) h2_pal_ble_gatt_write_fn writeCallback;
@property(nonatomic) void *callbackUser;
@property(nonatomic, strong) NSData *initialValue;
@end

@implementation H2CoreBluetoothGattBinding
@end

static BOOL s_test_start_enabled;
static h2_pal_result_t s_test_start_result = H2_PAL_OK;
static char s_corebluetooth_queue_key;

h2_pal_result_t h2_ios_corebluetooth_readiness_result(
    int centralKnown,
    int centralReady,
    int peripheralKnown,
    int peripheralReady) {
    if (centralKnown == 0 || peripheralKnown == 0) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    return centralReady != 0 && peripheralReady != 0
        ? H2_PAL_OK
        : H2_PAL_ERR_UNAVAILABLE;
}

void h2_ios_corebluetooth_test_set_start_result(
    int enabled,
    h2_pal_result_t result) {
    @synchronized([H2CoreBluetoothGattBinding class]) {
        s_test_start_enabled = enabled != 0;
        s_test_start_result = result;
    }
}

@interface H2CoreBluetoothBackend : NSObject <
    CBCentralManagerDelegate,
    CBPeripheralDelegate,
    CBPeripheralManagerDelegate> {
@public
    uint8_t discoveryUUIDBytes[H2_COREBLUETOOTH_DISCOVERY_MAX][16];
}
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, strong) CBCentralManager *centralManager;
@property(nonatomic, strong) CBPeripheralManager *peripheralManager;
@property(nonatomic) BOOL centralReady;
@property(nonatomic) BOOL peripheralReady;
@property(nonatomic) BOOL centralStateKnown;
@property(nonatomic) BOOL peripheralStateKnown;
@property(nonatomic, strong) dispatch_semaphore_t stateSemaphore;
@property(nonatomic, strong) dispatch_semaphore_t operationSemaphore;
@property(nonatomic) H2CoreBluetoothOperation operation;
@property(nonatomic) h2_pal_result_t operationResult;
@property(nonatomic) NSUInteger operationRemaining;
@property(nonatomic, strong) CBCharacteristic *operationCharacteristic;
@property(nonatomic, strong) NSMutableDictionary<NSString *, CBPeripheral *> *peripheralsByAddress;
@property(nonatomic, strong) CBPeripheral *connectedPeripheral;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, CBService *> *clientServices;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, CBCharacteristic *> *clientCharacteristics;
@property(nonatomic, strong) NSMutableDictionary<NSValue *, NSNumber *> *clientHandles;
@property(nonatomic) uint16_t nextClientHandle;
@property(nonatomic, strong) NSMutableArray<CBMutableService *> *serverServices;
@property(nonatomic, strong) NSMapTable<CBCharacteristic *, H2CoreBluetoothGattBinding *> *serverBindings;
@property(nonatomic) uint16_t nextServerHandle;
@property(nonatomic, strong) NSMutableSet<NSString *> *subscribedCentrals;
@property(nonatomic, strong) CBCentral *subscribedCentral;
@property(nonatomic) h2_pal_ble_scan_result_fn scanCallback;
@property(nonatomic) void *scanUser;
@property(nonatomic) h2_pal_ble_scan_type_t scanType;
@property(nonatomic, strong) NSDictionary<NSString *, id> *advertisementData;
@property(nonatomic) h2_pal_ble_adv_set_t *advertisingSet;
@property(nonatomic) BOOL advertising;
@property(nonatomic) BOOL started;
@property(nonatomic) BOOL peripheralConnected;
@end

static void h2_corebluetooth_post(
    h2_pal_system_event_type_t type,
    const void *payload,
    size_t payloadSize) {
    if (dispatch_get_specific(&s_corebluetooth_queue_key) != NULL) {
        NSData *payloadCopy = payloadSize > 0u
            ? [NSData dataWithBytes:payload length:payloadSize]
            : nil;
        static dispatch_queue_t eventQueue;
        static dispatch_once_t eventQueueOnce;
        dispatch_once(&eventQueueOnce, ^{
            eventQueue = dispatch_queue_create(
                "com.gizclaw.h2.ios.corebluetooth.events",
                DISPATCH_QUEUE_SERIAL);
        });
        dispatch_async(eventQueue, ^{
            h2_corebluetooth_post(
                type, payloadCopy.bytes, payloadCopy.length);
        });
        return;
    }
    const h2_pal_system_event_t event = {
        .type = type,
        .payload = payload,
        .payload_size = payloadSize,
    };
    (void)h2_pal_system_event_post(
        h2_ios_system_event_api(), &event, 0u);
}

static void h2_corebluetooth_post_adv(
    h2_pal_system_event_type_t type,
    h2_pal_ble_adv_set_t *set,
    h2_pal_result_t status) {
    const h2_pal_ble_adv_set_event_t payload = {
        .set = set,
        .status = status,
    };
    h2_corebluetooth_post(type, &payload, sizeof(payload));
}

static CBUUID *h2_corebluetooth_uuid(const h2_pal_ble_uuid_t *uuid) {
    if (uuid == NULL || uuid->data == NULL ||
        (uuid->len != 2u && uuid->len != 4u && uuid->len != 16u)) {
        return nil;
    }
    uint8_t bytes[16];
    if (h2_ios_corebluetooth_uuid_to_platform(
            uuid, bytes, sizeof(bytes)) != H2_PAL_OK) {
        return nil;
    }
    return [CBUUID UUIDWithData:[NSData dataWithBytes:bytes length:uuid->len]];
}

static size_t h2_corebluetooth_copy_uuid(
    CBUUID *uuid,
    uint8_t outBytes[16]) {
    NSData *data = uuid.data;
    if (data.length != 2u && data.length != 4u && data.length != 16u) {
        return 0u;
    }
    const uint8_t *bytes = data.bytes;
    if (h2_ios_corebluetooth_uuid_from_platform(
            bytes, data.length, outBytes, 16u) != H2_PAL_OK) {
        return 0u;
    }
    return data.length;
}

static BOOL h2_corebluetooth_uuid_equal(
    CBUUID *candidate,
    const h2_pal_ble_uuid_t *filter) {
    if (filter == NULL || filter->len == 0u) {
        return YES;
    }
    CBUUID *expected = h2_corebluetooth_uuid(filter);
    return expected != nil && [candidate isEqual:expected];
}

static void h2_corebluetooth_address(
    CBPeripheral *peripheral,
    h2_pal_ble_addr_t *outAddress) {
    uuid_t bytes;
    [peripheral.identifier getUUIDBytes:bytes];
    memset(outAddress, 0, sizeof(*outAddress));
    memcpy(outAddress->value, &bytes[10], H2_PAL_BLE_ADDR_LEN);
    outAddress->type = H2_PAL_BLE_ADDR_TYPE_PLATFORM_ID;
}

static NSString *h2_corebluetooth_address_key(
    const h2_pal_ble_addr_t *address) {
    return [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x",
        address->value[0], address->value[1], address->value[2],
        address->value[3], address->value[4], address->value[5]];
}

static uint32_t h2_corebluetooth_properties(CBCharacteristicProperties properties) {
    uint32_t value = 0u;
    if ((properties & CBCharacteristicPropertyRead) != 0u) {
        value |= H2_PAL_BLE_GATT_PROPERTY_READ;
    }
    if ((properties & CBCharacteristicPropertyWrite) != 0u) {
        value |= H2_PAL_BLE_GATT_PROPERTY_WRITE;
    }
    if ((properties & CBCharacteristicPropertyWriteWithoutResponse) != 0u) {
        value |= H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP;
    }
    if ((properties & CBCharacteristicPropertyNotify) != 0u) {
        value |= H2_PAL_BLE_GATT_PROPERTY_NOTIFY;
    }
    if ((properties & CBCharacteristicPropertyIndicate) != 0u) {
        value |= H2_PAL_BLE_GATT_PROPERTY_INDICATE;
    }
    return value;
}

static CBCharacteristicProperties h2_corebluetooth_cb_properties(uint32_t properties) {
    CBCharacteristicProperties value = 0u;
    if ((properties & H2_PAL_BLE_GATT_PROPERTY_READ) != 0u) {
        value |= CBCharacteristicPropertyRead;
    }
    if ((properties & H2_PAL_BLE_GATT_PROPERTY_WRITE) != 0u) {
        value |= CBCharacteristicPropertyWrite;
    }
    if ((properties & H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP) != 0u) {
        value |= CBCharacteristicPropertyWriteWithoutResponse;
    }
    if ((properties & H2_PAL_BLE_GATT_PROPERTY_NOTIFY) != 0u) {
        value |= CBCharacteristicPropertyNotify;
    }
    if ((properties & H2_PAL_BLE_GATT_PROPERTY_INDICATE) != 0u) {
        value |= CBCharacteristicPropertyIndicate;
    }
    return value;
}

static CBAttributePermissions h2_corebluetooth_permissions(uint32_t permissions) {
    CBAttributePermissions value = 0u;
    if ((permissions & H2_PAL_BLE_GATT_PERMISSION_READ) != 0u) {
        value |= CBAttributePermissionsReadable;
    }
    if ((permissions & H2_PAL_BLE_GATT_PERMISSION_WRITE) != 0u) {
        value |= CBAttributePermissionsWriteable;
    }
    return value;
}

static CBATTError h2_corebluetooth_att_error(h2_pal_result_t result) {
    switch (result) {
    case H2_PAL_OK:
        return CBATTErrorSuccess;
    case H2_PAL_ERR_INVALID_ARG:
        return CBATTErrorInvalidPdu;
    case H2_PAL_ERR_UNSUPPORTED:
        return CBATTErrorRequestNotSupported;
    case H2_PAL_ERR_NO_MEMORY:
    case H2_PAL_ERR_FULL:
        return CBATTErrorInsufficientResources;
    default:
        return CBATTErrorUnlikelyError;
    }
}

@implementation H2CoreBluetoothBackend

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _queue = dispatch_queue_create(
            "com.gizclaw.h2.ios.corebluetooth", DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(
            _queue,
            &s_corebluetooth_queue_key,
            &s_corebluetooth_queue_key,
            NULL);
        _stateSemaphore = dispatch_semaphore_create(0);
        _peripheralsByAddress = [NSMutableDictionary dictionary];
        _clientServices = [NSMutableDictionary dictionary];
        _clientCharacteristics = [NSMutableDictionary dictionary];
        _clientHandles = [NSMutableDictionary dictionary];
        _serverServices = [NSMutableArray array];
        _serverBindings = [NSMapTable strongToStrongObjectsMapTable];
        _subscribedCentrals = [NSMutableSet set];
        _nextClientHandle = 1u;
        _nextServerHandle = 1u;
    }
    return self;
}

- (BOOL)beginOperation:(H2CoreBluetoothOperation)operation {
    if (self.operation != H2CoreBluetoothOperationNone ||
        self.operationSemaphore != nil) {
        return NO;
    }
    self.operation = operation;
    self.operationResult = H2_PAL_ERR_TIMEOUT;
    self.operationRemaining = 0u;
    self.operationCharacteristic = nil;
    self.operationSemaphore = dispatch_semaphore_create(0);
    return YES;
}

- (void)completeOperation:(H2CoreBluetoothOperation)operation
                    result:(h2_pal_result_t)result {
    if (self.operation != operation || self.operationSemaphore == nil) {
        return;
    }
    self.operationResult = result;
    dispatch_semaphore_signal(self.operationSemaphore);
}

- (void)completePendingOperation:(h2_pal_result_t)result {
    if (self.operation == H2CoreBluetoothOperationNone ||
        self.operationSemaphore == nil) {
        return;
    }
    self.operationResult = result;
    dispatch_semaphore_signal(self.operationSemaphore);
}

- (void)abandonOperation {
    self.operation = H2CoreBluetoothOperationNone;
    self.operationRemaining = 0u;
    self.operationCharacteristic = nil;
    self.operationSemaphore = nil;
}

- (void)clearClientMappings {
    [self.clientServices removeAllObjects];
    [self.clientCharacteristics removeAllObjects];
    [self.clientHandles removeAllObjects];
    self.nextClientHandle = 1u;
}

- (h2_pal_result_t)waitForOperation:(uint32_t)timeoutMs {
    dispatch_semaphore_t semaphore = self.operationSemaphore;
    uint32_t effectiveTimeout = timeoutMs == 0u ? H2_COREBLUETOOTH_WAIT_MS : timeoutMs;
    long waitResult = dispatch_semaphore_wait(
        semaphore,
        dispatch_time(DISPATCH_TIME_NOW,
            (int64_t)effectiveTimeout * (int64_t)NSEC_PER_MSEC));
    __block h2_pal_result_t result = H2_PAL_ERR_TIMEOUT;
    dispatch_sync(self.queue, ^{
        if (waitResult == 0) {
            result = self.operationResult;
        }
        self.operation = H2CoreBluetoothOperationNone;
        self.operationRemaining = 0u;
        self.operationCharacteristic = nil;
        self.operationSemaphore = nil;
    });
    return result;
}

- (h2_pal_result_t)start {
    if (self.started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    @synchronized([H2CoreBluetoothGattBinding class]) {
        if (s_test_start_enabled) {
            if (s_test_start_result == H2_PAL_OK) {
                self.started = YES;
                h2_corebluetooth_post(
                    H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED, NULL, 0u);
            }
            return s_test_start_result;
        }
    }
    dispatch_async(self.queue, ^{
        self.centralManager = [[CBCentralManager alloc]
            initWithDelegate:self queue:self.queue options:nil];
        self.peripheralManager = [[CBPeripheralManager alloc]
            initWithDelegate:self queue:self.queue options:nil];
    });
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:10.0];
    while ([deadline timeIntervalSinceNow] > 0.0) {
        __block h2_pal_result_t readiness = H2_PAL_ERR_WOULD_BLOCK;
        dispatch_sync(self.queue, ^{
            readiness = h2_ios_corebluetooth_readiness_result(
                self.centralStateKnown, self.centralReady,
                self.peripheralStateKnown, self.peripheralReady);
        });
        if (readiness == H2_PAL_OK) {
            self.started = YES;
            h2_corebluetooth_post(
                H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED, NULL, 0u);
            return H2_PAL_OK;
        }
        if (readiness == H2_PAL_ERR_UNAVAILABLE) {
            return readiness;
        }
        (void)dispatch_semaphore_wait(
            self.stateSemaphore,
            dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC));
    }
    return H2_PAL_ERR_TIMEOUT;
}

- (h2_pal_result_t)stop {
    if (!self.started) {
        return H2_PAL_OK;
    }
    dispatch_sync(self.queue, ^{
        [self completePendingOperation:H2_PAL_ERR_CLOSED];
        [self.centralManager stopScan];
        if (self.connectedPeripheral != nil) {
            [self.centralManager cancelPeripheralConnection:self.connectedPeripheral];
        }
        [self.peripheralManager stopAdvertising];
        [self.peripheralManager removeAllServices];
        self.scanCallback = NULL;
        self.scanUser = NULL;
        self.connectedPeripheral = nil;
        [self clearClientMappings];
        [self.peripheralsByAddress removeAllObjects];
        [self.serverServices removeAllObjects];
        [self.serverBindings removeAllObjects];
        [self.subscribedCentrals removeAllObjects];
        self.subscribedCentral = nil;
        self.advertisementData = nil;
        self.advertising = NO;
        self.peripheralConnected = NO;
    });
    if (self.advertisingSet != NULL) {
        free(self.advertisingSet);
        self.advertisingSet = NULL;
    }
    self.started = NO;
    h2_corebluetooth_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED, NULL, 0u);
    return H2_PAL_OK;
}

- (NSDictionary<NSString *, id> *)copyAdvertisementData:
    (const h2_pal_ble_adv_data_t *)data {
    if (data == NULL ||
        (data->service_uuid_count > 0u && data->service_uuids == NULL)) {
        return nil;
    }
    NSMutableDictionary<NSString *, id> *advertisement = [NSMutableDictionary dictionary];
    if (data->local_name != NULL) {
        NSString *name = [NSString stringWithUTF8String:data->local_name];
        if (name == nil) return nil;
        advertisement[CBAdvertisementDataLocalNameKey] = name;
    }
    NSMutableArray<CBUUID *> *serviceUUIDs = [NSMutableArray array];
    for (size_t index = 0u; index < data->service_uuid_count; ++index) {
        CBUUID *uuid = h2_corebluetooth_uuid(&data->service_uuids[index]);
        if (uuid == nil) return nil;
        [serviceUUIDs addObject:uuid];
    }
    if (serviceUUIDs.count > 0u) {
        advertisement[CBAdvertisementDataServiceUUIDsKey] = serviceUUIDs;
    }
    if (data->manufacturer_data.len > 0u) {
        if (data->manufacturer_data.data == NULL) return nil;
        advertisement[CBAdvertisementDataManufacturerDataKey] =
            [NSData dataWithBytes:data->manufacturer_data.data
                           length:data->manufacturer_data.len];
    }
    if (data->service_data.len > 0u) {
        CBUUID *uuid = h2_corebluetooth_uuid(&data->service_data_uuid);
        if (uuid == nil || data->service_data.data == NULL) return nil;
        advertisement[CBAdvertisementDataServiceDataKey] = @{
            uuid : [NSData dataWithBytes:data->service_data.data
                                  length:data->service_data.len]
        };
    }
    return [advertisement copy];
}

- (h2_pal_result_t)setAdvertisingData:(const h2_pal_ble_adv_data_t *)data {
    NSDictionary<NSString *, id> *advertisement = [self copyAdvertisementData:data];
    if (advertisement == nil) return H2_PAL_ERR_INVALID_ARG;
    dispatch_sync(self.queue, ^{
        self.advertisementData = advertisement;
    });
    return H2_PAL_OK;
}

- (h2_pal_result_t)startAdvertising {
    if (!self.started || self.advertisementData == nil) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    __block BOOL began = NO;
    __block BOOL alreadyAdvertising = NO;
    dispatch_sync(self.queue, ^{
        alreadyAdvertising = self.advertising;
        if (!alreadyAdvertising) {
            began = [self beginOperation:H2CoreBluetoothOperationAdvertise];
            if (began) {
                [self.peripheralManager startAdvertising:self.advertisementData];
            }
        }
    });
    if (alreadyAdvertising) return H2_PAL_OK;
    if (!began) return H2_PAL_ERR_WOULD_BLOCK;
    return [self waitForOperation:H2_COREBLUETOOTH_WAIT_MS];
}

- (h2_pal_result_t)stopAdvertising {
    dispatch_sync(self.queue, ^{
        [self.peripheralManager stopAdvertising];
        self.advertising = NO;
    });
    h2_corebluetooth_post_adv(
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
        self.advertisingSet, H2_PAL_OK);
    return H2_PAL_OK;
}

- (h2_pal_result_t)createAdvertisingSet:(const h2_pal_ble_adv_params_t *)params
                                  output:(h2_pal_ble_adv_set_t **)outSet {
    if (params == NULL || outSet == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (!h2_ios_corebluetooth_adv_params_supported(params)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (self.advertisingSet != NULL) return H2_PAL_ERR_FULL;
    h2_pal_ble_adv_set_t *set = calloc(1u, sizeof(*set));
    if (set == NULL) return H2_PAL_ERR_NO_MEMORY;
    set->params = *params;
    self.advertisingSet = set;
    *outSet = set;
    return H2_PAL_OK;
}

- (h2_pal_result_t)destroyAdvertisingSet:(h2_pal_ble_adv_set_t *)set {
    if (set == NULL || set != self.advertisingSet) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)[self stopAdvertising];
    free(set);
    self.advertisingSet = NULL;
    self.advertisementData = nil;
    return H2_PAL_OK;
}

- (h2_pal_result_t)startScan:(const h2_pal_ble_scan_params_t *)params
                    callback:(h2_pal_ble_scan_result_fn)callback
                        user:(void *)user {
    if (!self.started || params == NULL || callback == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!h2_ios_corebluetooth_scan_params_supported(params)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    dispatch_sync(self.queue, ^{
        self.scanCallback = callback;
        self.scanUser = user;
        self.scanType = params->type;
        NSDictionary *options = @{
            CBCentralManagerScanOptionAllowDuplicatesKey : @YES
        };
        [self.centralManager scanForPeripheralsWithServices:nil options:options];
    });
    h2_corebluetooth_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STARTED, NULL, 0u);
    return H2_PAL_OK;
}

- (h2_pal_result_t)stopScan {
    dispatch_sync(self.queue, ^{
        [self.centralManager stopScan];
        self.scanCallback = NULL;
        self.scanUser = NULL;
    });
    h2_corebluetooth_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STOPPED, NULL, 0u);
    return H2_PAL_OK;
}

- (h2_pal_result_t)registerServices:(const h2_pal_ble_gatt_service_t *)services
                              count:(size_t)count {
    if (!self.started || services == NULL || count == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    __block BOOL began = NO;
    dispatch_sync(self.queue, ^{
        began = [self beginOperation:H2CoreBluetoothOperationRegisterServices];
    });
    if (!began) return H2_PAL_ERR_WOULD_BLOCK;
    NSMutableArray<CBMutableService *> *newServices = [NSMutableArray array];
    for (size_t serviceIndex = 0u; serviceIndex < count; ++serviceIndex) {
        const h2_pal_ble_gatt_service_t *sourceService = &services[serviceIndex];
        CBUUID *serviceUUID = h2_corebluetooth_uuid(&sourceService->uuid);
        if (serviceUUID == nil ||
            (sourceService->characteristic_count > 0u &&
             sourceService->characteristics == NULL)) {
            dispatch_sync(self.queue, ^{ [self abandonOperation]; });
            return H2_PAL_ERR_INVALID_ARG;
        }
        uint16_t serviceHandle = self.nextServerHandle++;
        if (sourceService->out_service_handle != NULL) {
            *sourceService->out_service_handle = serviceHandle;
        }
        NSMutableArray<CBMutableCharacteristic *> *characteristics =
            [NSMutableArray array];
        for (size_t characteristicIndex = 0u;
             characteristicIndex < sourceService->characteristic_count;
             ++characteristicIndex) {
            const h2_pal_ble_gatt_characteristic_t *source =
                &sourceService->characteristics[characteristicIndex];
            CBUUID *characteristicUUID = h2_corebluetooth_uuid(&source->uuid);
            if (characteristicUUID == nil ||
                (source->initial_value_len > 0u && source->initial_value == NULL)) {
                dispatch_sync(self.queue, ^{ [self abandonOperation]; });
                return H2_PAL_ERR_INVALID_ARG;
            }
            uint16_t valueHandle = self.nextServerHandle++;
            uint16_t cccdHandle = 0u;
            if ((source->properties &
                 (H2_PAL_BLE_GATT_PROPERTY_NOTIFY |
                  H2_PAL_BLE_GATT_PROPERTY_INDICATE)) != 0u) {
                cccdHandle = self.nextServerHandle++;
            }
            if (source->out_value_handle != NULL) {
                *source->out_value_handle = valueHandle;
            }
            if (source->out_cccd_handle != NULL) {
                *source->out_cccd_handle = cccdHandle;
            }
            NSData *initialValue = source->initial_value_len > 0u
                ? [NSData dataWithBytes:source->initial_value
                                 length:source->initial_value_len]
                : nil;
            CBMutableCharacteristic *characteristic =
                [[CBMutableCharacteristic alloc]
                    initWithType:characteristicUUID
                      properties:h2_corebluetooth_cb_properties(source->properties)
                           value:source->read == NULL && source->write == NULL
                               ? initialValue : nil
                     permissions:h2_corebluetooth_permissions(source->permissions)];
            H2CoreBluetoothGattBinding *binding =
                [[H2CoreBluetoothGattBinding alloc] init];
            binding.valueHandle = valueHandle;
            binding.cccdHandle = cccdHandle;
            binding.readCallback = source->read;
            binding.writeCallback = source->write;
            binding.callbackUser = source->user;
            binding.initialValue = initialValue;
            [self.serverBindings setObject:binding forKey:characteristic];
            [characteristics addObject:characteristic];
        }
        CBMutableService *service = [[CBMutableService alloc]
            initWithType:serviceUUID primary:sourceService->primary];
        service.characteristics = characteristics;
        [newServices addObject:service];
    }
    dispatch_sync(self.queue, ^{
        self.operationRemaining = newServices.count;
        [self.serverServices addObjectsFromArray:newServices];
        for (CBMutableService *service in newServices) {
            [self.peripheralManager addService:service];
        }
    });
    return [self waitForOperation:H2_COREBLUETOOTH_WAIT_MS];
}

- (h2_pal_result_t)unregisterServices {
    dispatch_sync(self.queue, ^{
        [self.peripheralManager removeAllServices];
        [self.serverServices removeAllObjects];
        [self.serverBindings removeAllObjects];
        self.nextServerHandle = 1u;
    });
    return H2_PAL_OK;
}

- (h2_pal_result_t)notify:(uint16_t)connectionHandle
                   handle:(uint16_t)attributeHandle
                     data:(const uint8_t *)data
                   length:(size_t)length {
    if (connectionHandle != H2_COREBLUETOOTH_CONN_HANDLE ||
        (length > 0u && data == NULL) ||
        length > H2_PAL_BLE_ATT_MAX_VALUE_LEN) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    __block CBMutableCharacteristic *target = nil;
    __block CBCentral *central = nil;
    dispatch_sync(self.queue, ^{
        for (CBMutableService *service in self.serverServices) {
            for (CBMutableCharacteristic *characteristic in service.characteristics) {
                H2CoreBluetoothGattBinding *binding =
                    [self.serverBindings objectForKey:characteristic];
                if (binding.valueHandle == attributeHandle) {
                    target = characteristic;
                    central = self.subscribedCentral;
                    return;
                }
            }
        }
    });
    if (target == nil || central == nil) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    NSData *value = [NSData dataWithBytes:data length:length];
    __block BOOL queued = NO;
    dispatch_sync(self.queue, ^{
        queued = [self.peripheralManager updateValue:value
                                   forCharacteristic:target
                                onSubscribedCentrals:@[ central ]];
    });
    return queued ? H2_PAL_OK : H2_PAL_ERR_WOULD_BLOCK;
}

- (h2_pal_result_t)connectAddress:(const h2_pal_ble_addr_t *)address
                           timeout:(uint32_t)timeoutMs
                            output:(uint16_t *)outHandle {
    if (!self.started || address == NULL || outHandle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    NSString *key = h2_corebluetooth_address_key(address);
    __block CBPeripheral *peripheral = nil;
    __block BOOL began = NO;
    dispatch_sync(self.queue, ^{
        peripheral = self.peripheralsByAddress[key];
        if (peripheral != nil) {
            began = [self beginOperation:H2CoreBluetoothOperationConnect];
            if (began) {
                [self.centralManager connectPeripheral:peripheral options:nil];
            }
        }
    });
    if (peripheral == nil) return H2_PAL_ERR_NOT_FOUND;
    if (!began) return H2_PAL_ERR_WOULD_BLOCK;
    h2_pal_result_t result = [self waitForOperation:timeoutMs];
    if (result == H2_PAL_OK) {
        *outHandle = H2_COREBLUETOOTH_CONN_HANDLE;
    }
    return result;
}

- (h2_pal_result_t)disconnect:(uint16_t)connectionHandle {
    if (connectionHandle != H2_COREBLUETOOTH_CONN_HANDLE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    __block BOOL centralConnection = NO;
    __block BOOL began = NO;
    dispatch_sync(self.queue, ^{
        if (self.connectedPeripheral != nil) {
            centralConnection = YES;
            began = [self beginOperation:H2CoreBluetoothOperationDisconnect];
            if (began) {
                [self.centralManager cancelPeripheralConnection:self.connectedPeripheral];
            }
        }
    });
    if (!centralConnection) {
        /* CoreBluetooth exposes no peripheral-side force-disconnect API. */
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (!began) return H2_PAL_ERR_WOULD_BLOCK;
    return [self waitForOperation:H2_COREBLUETOOTH_WAIT_MS];
}

- (h2_pal_result_t)exchangeMtu:(uint16_t)connectionHandle
                          output:(uint16_t *)outMtu
                         timeout:(uint32_t)timeoutMs {
    if (connectionHandle != H2_COREBLUETOOTH_CONN_HANDLE || outMtu == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    __block NSUInteger payloadLength = 0u;
    __block BOOL connected = NO;
    const uint32_t waitMs = timeoutMs != 0u
        ? timeoutMs : H2_COREBLUETOOTH_WAIT_MS;
    uint32_t elapsedMs = 0u;
    do {
        dispatch_sync(self.queue, ^{
            connected = self.connectedPeripheral != nil ||
                self.subscribedCentral != nil;
            if (self.connectedPeripheral != nil) {
                payloadLength = [self.connectedPeripheral
                    maximumWriteValueLengthForType:
                        CBCharacteristicWriteWithoutResponse];
            } else if (self.subscribedCentral != nil) {
                payloadLength = self.subscribedCentral.maximumUpdateValueLength;
            }
        });
        if (!connected ||
            payloadLength > H2_COREBLUETOOTH_DEFAULT_ATT_PAYLOAD ||
            elapsedMs >= waitMs) {
            break;
        }
        [NSThread sleepForTimeInterval:
            (double)H2_COREBLUETOOTH_MTU_POLL_MS / 1000.0];
        elapsedMs += H2_COREBLUETOOTH_MTU_POLL_MS;
    } while (true);
    if (!connected) return H2_PAL_ERR_INVALID_STATE;
    if (payloadLength == 0u) return H2_PAL_ERR_INVALID_STATE;
    NSUInteger mtu = MIN(payloadLength + H2_PAL_BLE_ATT_HEADER_LEN,
        (NSUInteger)H2_PAL_BLE_ATT_MAX_MTU);
    *outMtu = (uint16_t)mtu;
    const h2_pal_ble_mtu_info_t info = {
        .conn_handle = connectionHandle,
        .mtu = (uint16_t)mtu,
    };
    h2_corebluetooth_post(H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED,
        &info, sizeof(info));
    return H2_PAL_OK;
}

- (CBCharacteristic *)clientCharacteristic:(uint16_t)handle {
    __block CBCharacteristic *characteristic = nil;
    dispatch_sync(self.queue, ^{
        characteristic = self.clientCharacteristics[@(handle)];
    });
    return characteristic;
}

- (h2_pal_result_t)discover:(uint16_t)connectionHandle
                     request:(const h2_pal_ble_gatt_discovery_request_t *)request
                     entries:(h2_pal_ble_gatt_discovery_entry_t *)entries
                    capacity:(size_t)capacity
                       count:(size_t *)outCount
                     timeout:(uint32_t)timeoutMs {
    if (connectionHandle != H2_COREBLUETOOTH_CONN_HANDLE || request == NULL ||
        outCount == NULL || (capacity > 0u && entries == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *outCount = 0u;
    if (self.connectedPeripheral == nil) return H2_PAL_ERR_INVALID_STATE;
    if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_SERVICE) {
        CBUUID *filter = h2_corebluetooth_uuid(&request->uuid_filter);
        __block BOOL began = NO;
        dispatch_sync(self.queue, ^{
            began = [self beginOperation:H2CoreBluetoothOperationDiscoverServices];
            if (began) {
                [self.connectedPeripheral discoverServices:
                    filter != nil ? @[ filter ] : nil];
            }
        });
        if (!began) return H2_PAL_ERR_WOULD_BLOCK;
        h2_pal_result_t result = [self waitForOperation:timeoutMs];
        if (result != H2_PAL_OK) return result;
        __block size_t count = 0u;
        dispatch_sync(self.queue, ^{
            for (CBService *service in self.connectedPeripheral.services) {
                if (count >= capacity || count >= H2_COREBLUETOOTH_DISCOVERY_MAX ||
                    !h2_corebluetooth_uuid_equal(service.UUID, &request->uuid_filter)) {
                    continue;
                }
                NSNumber *handleNumber = self.clientHandles[[NSValue valueWithNonretainedObject:service]];
                uint16_t handle = handleNumber != nil ? handleNumber.unsignedShortValue : self.nextClientHandle++;
                self.clientHandles[[NSValue valueWithNonretainedObject:service]] = @(handle);
                self.clientServices[@(handle)] = service;
                size_t uuidLength = h2_corebluetooth_copy_uuid(
                    service.UUID, discoveryUUIDBytes[count]);
                entries[count] = (h2_pal_ble_gatt_discovery_entry_t){
                    .kind = H2_PAL_BLE_GATT_DISCOVERY_SERVICE,
                    .uuid = { discoveryUUIDBytes[count], uuidLength },
                    .start_handle = handle,
                    .end_handle = UINT16_MAX,
                    .value_handle = handle,
                    .properties = 0u,
                };
                ++count;
            }
        });
        *outCount = count;
        return H2_PAL_OK;
    }
    if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC) {
        __block NSArray<CBService *> *services = nil;
        __block BOOL began = NO;
        dispatch_sync(self.queue, ^{
            NSMutableArray<CBService *> *matches = [NSMutableArray array];
            for (NSNumber *key in self.clientServices) {
                uint16_t handle = key.unsignedShortValue;
                if (handle >= request->start_handle && handle <= request->end_handle) {
                    [matches addObject:self.clientServices[key]];
                }
            }
            services = [matches copy];
            began = [self beginOperation:
                H2CoreBluetoothOperationDiscoverCharacteristics];
            if (!began) return;
            self.operationRemaining = services.count;
            CBUUID *filter = h2_corebluetooth_uuid(&request->uuid_filter);
            for (CBService *service in services) {
                [self.connectedPeripheral discoverCharacteristics:
                    filter != nil ? @[ filter ] : nil forService:service];
            }
            if (services.count == 0u) {
                [self completeOperation:H2CoreBluetoothOperationDiscoverCharacteristics
                                 result:H2_PAL_ERR_NOT_FOUND];
            }
        });
        if (!began) return H2_PAL_ERR_WOULD_BLOCK;
        h2_pal_result_t result = [self waitForOperation:timeoutMs];
        if (result != H2_PAL_OK) return result;
        __block size_t count = 0u;
        dispatch_sync(self.queue, ^{
            for (CBService *service in services) {
                for (CBCharacteristic *characteristic in service.characteristics) {
                    if (count >= capacity || count >= H2_COREBLUETOOTH_DISCOVERY_MAX ||
                        !h2_corebluetooth_uuid_equal(
                            characteristic.UUID, &request->uuid_filter)) {
                        continue;
                    }
                    NSValue *objectKey = [NSValue valueWithNonretainedObject:characteristic];
                    NSNumber *handleNumber = self.clientHandles[objectKey];
                    uint16_t handle = handleNumber != nil
                        ? handleNumber.unsignedShortValue : self.nextClientHandle++;
                    self.clientHandles[objectKey] = @(handle);
                    self.clientCharacteristics[@(handle)] = characteristic;
                    size_t uuidLength = h2_corebluetooth_copy_uuid(
                        characteristic.UUID, discoveryUUIDBytes[count]);
                    entries[count] = (h2_pal_ble_gatt_discovery_entry_t){
                        .kind = H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC,
                        .uuid = { discoveryUUIDBytes[count], uuidLength },
                        .start_handle = handle,
                        .end_handle = (uint16_t)(handle + 1u),
                        .value_handle = handle,
                        .properties = h2_corebluetooth_properties(characteristic.properties),
                    };
                    ++count;
                }
            }
        });
        *outCount = count;
        return H2_PAL_OK;
    }
    if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR) {
        static const uint8_t cccdBytes[] = { 0x02u, 0x29u };
        if (capacity == 0u || request->uuid_filter.len != sizeof(cccdBytes) ||
            memcmp(request->uuid_filter.data, cccdBytes, sizeof(cccdBytes)) != 0) {
            return H2_PAL_OK;
        }
        __block uint16_t valueHandle = 0u;
        dispatch_sync(self.queue, ^{
            for (NSNumber *key in self.clientCharacteristics) {
                uint16_t handle = key.unsignedShortValue;
                CBCharacteristic *characteristic = self.clientCharacteristics[key];
                if (handle + 1u >= request->start_handle &&
                    handle + 1u <= request->end_handle &&
                    (characteristic.properties & CBCharacteristicPropertyNotify) != 0u) {
                    valueHandle = (uint16_t)(handle + 1u);
                    break;
                }
            }
        });
        if (valueHandle == 0u) return H2_PAL_OK;
        memcpy(discoveryUUIDBytes[0], cccdBytes, sizeof(cccdBytes));
        entries[0] = (h2_pal_ble_gatt_discovery_entry_t){
            .kind = H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR,
            .uuid = { discoveryUUIDBytes[0], sizeof(cccdBytes) },
            .start_handle = valueHandle,
            .end_handle = valueHandle,
            .value_handle = valueHandle,
            .properties = 0u,
        };
        *outCount = 1u;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

- (h2_pal_result_t)read:(uint16_t)connectionHandle
                  handle:(uint16_t)attributeHandle
                  offset:(uint16_t)offset
                  output:(uint8_t *)outBytes
                capacity:(size_t)capacity
                   count:(size_t *)outCount
                 timeout:(uint32_t)timeoutMs {
    if (connectionHandle != H2_COREBLUETOOTH_CONN_HANDLE || outCount == NULL ||
        (capacity > 0u && outBytes == NULL) || offset != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    CBCharacteristic *characteristic = [self clientCharacteristic:attributeHandle];
    if (characteristic == nil) return H2_PAL_ERR_NOT_FOUND;
    __block BOOL began = NO;
    dispatch_sync(self.queue, ^{
        began = [self beginOperation:H2CoreBluetoothOperationRead];
        if (began) {
            self.operationCharacteristic = characteristic;
            [self.connectedPeripheral readValueForCharacteristic:characteristic];
        }
    });
    if (!began) return H2_PAL_ERR_WOULD_BLOCK;
    h2_pal_result_t result = [self waitForOperation:timeoutMs];
    if (result != H2_PAL_OK) return result;
    NSData *value = characteristic.value;
    if (value.length > capacity) return H2_PAL_ERR_FULL;
    memcpy(outBytes, value.bytes, value.length);
    *outCount = value.length;
    return H2_PAL_OK;
}

- (h2_pal_result_t)write:(uint16_t)connectionHandle
                   handle:(uint16_t)attributeHandle
                     data:(const uint8_t *)data
                   length:(size_t)length
             withResponse:(BOOL)withResponse
                  timeout:(uint32_t)timeoutMs {
    if (connectionHandle != H2_COREBLUETOOTH_CONN_HANDLE ||
        (length > 0u && data == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    CBCharacteristic *characteristic = [self clientCharacteristic:attributeHandle];
    if (characteristic == nil || self.connectedPeripheral == nil) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    NSData *value = [NSData dataWithBytes:data length:length];
    if (!withResponse) {
        __block BOOL ready = NO;
        dispatch_sync(self.queue, ^{
            ready = self.connectedPeripheral.canSendWriteWithoutResponse;
            if (ready) {
                [self.connectedPeripheral writeValue:value
                                   forCharacteristic:characteristic
                                                type:CBCharacteristicWriteWithoutResponse];
            }
        });
        return ready ? H2_PAL_OK : H2_PAL_ERR_WOULD_BLOCK;
    }
    __block BOOL began = NO;
    dispatch_sync(self.queue, ^{
        began = [self beginOperation:H2CoreBluetoothOperationWrite];
        if (began) {
            self.operationCharacteristic = characteristic;
            [self.connectedPeripheral writeValue:value
                               forCharacteristic:characteristic
                                            type:CBCharacteristicWriteWithResponse];
        }
    });
    if (!began) return H2_PAL_ERR_WOULD_BLOCK;
    return [self waitForOperation:timeoutMs];
}

- (h2_pal_result_t)subscribe:(uint16_t)connectionHandle
                       config:(const h2_pal_ble_gatt_subscribe_t *)subscribe
                      timeout:(uint32_t)timeoutMs {
    if (connectionHandle != H2_COREBLUETOOTH_CONN_HANDLE || subscribe == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    CBCharacteristic *characteristic = [self clientCharacteristic:subscribe->value_handle];
    if (characteristic == nil) return H2_PAL_ERR_NOT_FOUND;
    __block BOOL began = NO;
    dispatch_sync(self.queue, ^{
        began = [self beginOperation:H2CoreBluetoothOperationSubscribe];
        if (began) {
            self.operationCharacteristic = characteristic;
            [self.connectedPeripheral setNotifyValue:subscribe->enable
                                  forCharacteristic:characteristic];
        }
    });
    if (!began) return H2_PAL_ERR_WOULD_BLOCK;
    return [self waitForOperation:timeoutMs];
}

- (BOOL)isActivePeripheralCentral:(CBCentral *)central {
    return central != nil && self.subscribedCentral != nil &&
        [self.subscribedCentral.identifier isEqual:central.identifier];
}

- (BOOL)ensurePeripheralConnection:(CBCentral *)central {
    if (central == nil) return NO;
    if (self.subscribedCentral != nil) {
        return [self isActivePeripheralCentral:central];
    }
    NSString *identifier = central.identifier.UUIDString;
    self.subscribedCentral = central;
    if (self.peripheralConnected) return YES;
    self.peripheralConnected = YES;
    h2_pal_ble_connection_t connection;
    memset(&connection, 0, sizeof(connection));
    connection.conn_handle = H2_COREBLUETOOTH_CONN_HANDLE;
    connection.role = H2_PAL_BLE_ROLE_PERIPHERAL;
    connection.peer_addr.type = H2_PAL_BLE_ADDR_TYPE_PLATFORM_ID;
    uuid_t bytes;
    [central.identifier getUUIDBytes:bytes];
    memcpy(connection.peer_addr.value, &bytes[10], H2_PAL_BLE_ADDR_LEN);
    connection.mtu = (uint16_t)MIN(
        central.maximumUpdateValueLength + H2_PAL_BLE_ATT_HEADER_LEN,
        (NSUInteger)H2_PAL_BLE_ATT_MAX_MTU);
    [self.subscribedCentrals addObject:identifier];
    h2_corebluetooth_post(
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED, &connection, sizeof(connection));
    const h2_pal_ble_mtu_info_t mtu = {
        .conn_handle = connection.conn_handle,
        .mtu = connection.mtu,
    };
    h2_corebluetooth_post(
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED, &mtu, sizeof(mtu));
    return YES;
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    self.centralStateKnown = YES;
    self.centralReady = central.state == CBManagerStatePoweredOn;
    if (!self.centralReady) {
        [self completePendingOperation:H2_PAL_ERR_UNAVAILABLE];
    }
    dispatch_semaphore_signal(self.stateSemaphore);
}

- (void)peripheralManagerDidUpdateState:(CBPeripheralManager *)peripheral {
    self.peripheralStateKnown = YES;
    self.peripheralReady = peripheral.state == CBManagerStatePoweredOn;
    if (!self.peripheralReady) {
        [self completePendingOperation:H2_PAL_ERR_UNAVAILABLE];
    }
    dispatch_semaphore_signal(self.stateSemaphore);
}

- (void)centralManager:(CBCentralManager *)central
 didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *, id> *)advertisementData
                  RSSI:(NSNumber *)RSSI {
    (void)central;
    h2_pal_ble_addr_t address;
    h2_corebluetooth_address(peripheral, &address);
    self.peripheralsByAddress[h2_corebluetooth_address_key(&address)] = peripheral;
    h2_pal_ble_scan_result_fn callback = self.scanCallback;
    if (callback == NULL) return;

    NSArray<CBUUID *> *serviceUUIDObjects =
        advertisementData[CBAdvertisementDataServiceUUIDsKey];
    size_t serviceCount = MIN(serviceUUIDObjects.count,
        (NSUInteger)H2_COREBLUETOOTH_DISCOVERY_MAX);
    h2_pal_ble_uuid_t serviceUUIDs[H2_COREBLUETOOTH_DISCOVERY_MAX];
    uint8_t serviceBytes[H2_COREBLUETOOTH_DISCOVERY_MAX][16];
    for (size_t index = 0u; index < serviceCount; ++index) {
        size_t length = h2_corebluetooth_copy_uuid(
            serviceUUIDObjects[index], serviceBytes[index]);
        serviceUUIDs[index] = (h2_pal_ble_uuid_t){ serviceBytes[index], length };
    }
    NSString *name = advertisementData[CBAdvertisementDataLocalNameKey];
    NSData *manufacturer = advertisementData[CBAdvertisementDataManufacturerDataKey];
    NSDictionary<CBUUID *, NSData *> *serviceData =
        advertisementData[CBAdvertisementDataServiceDataKey];
    NSData *firstServiceData = serviceData.allValues.firstObject;
    NSNumber *connectable = advertisementData[CBAdvertisementDataIsConnectable];
    NSNumber *txPower = advertisementData[CBAdvertisementDataTxPowerLevelKey];
    h2_pal_ble_scan_result_t result;
    memset(&result, 0, sizeof(result));
    result.addr = address;
    result.rssi = RSSI.intValue;
    result.connectable = connectable == nil || connectable.boolValue;
    result.local_name = name.UTF8String;
    result.local_name_len = name != nil ? strlen(name.UTF8String) : 0u;
    result.manufacturer_data = (h2_pal_ble_bytes_t){
        manufacturer.bytes, manufacturer.length };
    result.service_data = (h2_pal_ble_bytes_t){
        firstServiceData.bytes, firstServiceData.length };
    result.service_uuids = serviceCount > 0u ? serviceUUIDs : NULL;
    result.service_uuid_count = serviceCount;
    result.adv_type = H2_PAL_BLE_ADV_TYPE_LEGACY;
    result.primary_phy = H2_PAL_BLE_PHY_UNKNOWN;
    result.secondary_phy = H2_PAL_BLE_PHY_UNKNOWN;
    result.data_status = H2_PAL_BLE_ADV_DATA_COMPLETE;
    result.tx_power = txPower != nil ? (int8_t)txPower.intValue : 127;
    if (callback(self.scanUser, &result)) {
        [self.centralManager stopScan];
    }
}

- (void)centralManager:(CBCentralManager *)central
  didConnectPeripheral:(CBPeripheral *)peripheral {
    (void)central;
    self.connectedPeripheral = peripheral;
    peripheral.delegate = self;
    [self clearClientMappings];
    h2_pal_ble_connection_t connection;
    memset(&connection, 0, sizeof(connection));
    connection.conn_handle = H2_COREBLUETOOTH_CONN_HANDLE;
    connection.role = H2_PAL_BLE_ROLE_CENTRAL;
    h2_corebluetooth_address(peripheral, &connection.peer_addr);
    connection.mtu = 23u;
    h2_corebluetooth_post(
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED, &connection, sizeof(connection));
    [self completeOperation:H2CoreBluetoothOperationConnect result:H2_PAL_OK];
}

- (void)centralManager:(CBCentralManager *)central
 didFailToConnectPeripheral:(CBPeripheral *)peripheral
                  error:(NSError *)error {
    (void)central;
    (void)peripheral;
    (void)error;
    self.connectedPeripheral = nil;
    [self clearClientMappings];
    [self completeOperation:H2CoreBluetoothOperationConnect result:H2_PAL_ERR_IO];
}

- (void)centralManager:(CBCentralManager *)central
 didDisconnectPeripheral:(CBPeripheral *)peripheral
                   error:(NSError *)error {
    (void)central;
    h2_pal_ble_disconnected_info_t info;
    memset(&info, 0, sizeof(info));
    info.conn_handle = H2_COREBLUETOOTH_CONN_HANDLE;
    h2_corebluetooth_address(peripheral, &info.peer_addr);
    info.reason = error != nil ? (int)error.code : H2_PAL_OK;
    self.connectedPeripheral = nil;
    [self clearClientMappings];
    h2_corebluetooth_post(
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED, &info, sizeof(info));
    if (self.operation == H2CoreBluetoothOperationDisconnect) {
        [self completeOperation:H2CoreBluetoothOperationDisconnect result:H2_PAL_OK];
    } else {
        [self completePendingOperation:H2_PAL_ERR_IO];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
 didDiscoverServices:(NSError *)error {
    (void)peripheral;
    [self completeOperation:H2CoreBluetoothOperationDiscoverServices
                     result:error == nil ? H2_PAL_OK : H2_PAL_ERR_IO];
}

- (void)peripheral:(CBPeripheral *)peripheral
 didDiscoverCharacteristicsForService:(CBService *)service
              error:(NSError *)error {
    (void)peripheral;
    (void)service;
    if (self.operation != H2CoreBluetoothOperationDiscoverCharacteristics) return;
    if (error != nil) self.operationResult = H2_PAL_ERR_IO;
    if (self.operationRemaining > 0u) --self.operationRemaining;
    if (self.operationRemaining == 0u) {
        [self completeOperation:H2CoreBluetoothOperationDiscoverCharacteristics
                         result:self.operationResult == H2_PAL_ERR_IO
                             ? H2_PAL_ERR_IO : H2_PAL_OK];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
 didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic
              error:(NSError *)error {
    (void)peripheral;
    NSNumber *handleNumber = self.clientHandles[
        [NSValue valueWithNonretainedObject:characteristic]];
    if (characteristic.isNotifying && handleNumber != nil && error == nil) {
        NSData *value = characteristic.value;
        if (value.length <= H2_PAL_BLE_ATT_MAX_VALUE_LEN) {
            h2_pal_ble_gatt_client_value_t notification;
            memset(&notification, 0, sizeof(notification));
            notification.conn_handle = H2_COREBLUETOOTH_CONN_HANDLE;
            notification.attr_handle = handleNumber.unsignedShortValue;
            notification.value_len = value.length;
            memcpy(notification.value, value.bytes, value.length);
            h2_corebluetooth_post(
                H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION,
                &notification, sizeof(notification));
        }
    }
    if (self.operation == H2CoreBluetoothOperationRead &&
        self.operationCharacteristic == characteristic) {
        [self completeOperation:H2CoreBluetoothOperationRead
                         result:error == nil ? H2_PAL_OK : H2_PAL_ERR_IO];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
 didWriteValueForCharacteristic:(CBCharacteristic *)characteristic
              error:(NSError *)error {
    (void)peripheral;
    if (self.operation == H2CoreBluetoothOperationWrite &&
        self.operationCharacteristic == characteristic) {
        [self completeOperation:H2CoreBluetoothOperationWrite
                         result:error == nil ? H2_PAL_OK : H2_PAL_ERR_IO];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
 didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)characteristic
              error:(NSError *)error {
    (void)peripheral;
    if (self.operation == H2CoreBluetoothOperationSubscribe &&
        self.operationCharacteristic == characteristic) {
        [self completeOperation:H2CoreBluetoothOperationSubscribe
                         result:error == nil ? H2_PAL_OK : H2_PAL_ERR_IO];
    }
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral
            didAddService:(CBService *)service
                    error:(NSError *)error {
    (void)peripheral;
    (void)service;
    if (self.operation != H2CoreBluetoothOperationRegisterServices) return;
    if (error != nil) self.operationResult = H2_PAL_ERR_IO;
    if (self.operationRemaining > 0u) --self.operationRemaining;
    if (self.operationRemaining == 0u) {
        [self completeOperation:H2CoreBluetoothOperationRegisterServices
                         result:self.operationResult == H2_PAL_ERR_IO
                             ? H2_PAL_ERR_IO : H2_PAL_OK];
    }
}

- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager *)peripheral
                                       error:(NSError *)error {
    (void)peripheral;
    h2_pal_result_t result = error == nil ? H2_PAL_OK : H2_PAL_ERR_IO;
    self.advertising = error == nil;
    h2_corebluetooth_post_adv(
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
        self.advertisingSet, result);
    [self completeOperation:H2CoreBluetoothOperationAdvertise result:result];
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral
                    central:(CBCentral *)central
 didSubscribeToCharacteristic:(CBCharacteristic *)characteristic {
    (void)peripheral;
    if (![self ensurePeripheralConnection:central]) return;
    [self.subscribedCentrals addObject:central.identifier.UUIDString];
    H2CoreBluetoothGattBinding *binding =
        [self.serverBindings objectForKey:characteristic];
    if (binding == nil) return;
    const h2_pal_ble_subscription_state_t state = {
        .conn_handle = H2_COREBLUETOOTH_CONN_HANDLE,
        .value_handle = binding.valueHandle,
        .mode = (characteristic.properties & CBCharacteristicPropertyNotify) != 0u
            ? H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY
            : H2_PAL_BLE_SUBSCRIBE_MODE_INDICATE,
        .enabled = true,
    };
    h2_corebluetooth_post(
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED,
        &state, sizeof(state));
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral
                    central:(CBCentral *)central
didUnsubscribeFromCharacteristic:(CBCharacteristic *)characteristic {
    (void)peripheral;
    if (![self isActivePeripheralCentral:central]) return;
    H2CoreBluetoothGattBinding *binding =
        [self.serverBindings objectForKey:characteristic];
    if (binding != nil) {
        const h2_pal_ble_subscription_state_t state = {
            .conn_handle = H2_COREBLUETOOTH_CONN_HANDLE,
            .value_handle = binding.valueHandle,
            .mode = (characteristic.properties & CBCharacteristicPropertyNotify) != 0u
                ? H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY
                : H2_PAL_BLE_SUBSCRIBE_MODE_INDICATE,
            .enabled = false,
        };
        h2_corebluetooth_post(
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED,
            &state, sizeof(state));
    }
    [self.subscribedCentrals removeObject:central.identifier.UUIDString];
    if (self.subscribedCentrals.count == 0u && self.peripheralConnected) {
        h2_pal_ble_disconnected_info_t info;
        memset(&info, 0, sizeof(info));
        info.conn_handle = H2_COREBLUETOOTH_CONN_HANDLE;
        uuid_t bytes;
        [central.identifier getUUIDBytes:bytes];
        memcpy(info.peer_addr.value, &bytes[10], H2_PAL_BLE_ADDR_LEN);
        info.peer_addr.type = H2_PAL_BLE_ADDR_TYPE_PLATFORM_ID;
        self.peripheralConnected = NO;
        self.subscribedCentral = nil;
        h2_corebluetooth_post(
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
            &info, sizeof(info));
    }
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral
   didReceiveReadRequest:(CBATTRequest *)request {
    if (![self ensurePeripheralConnection:request.central]) {
        [peripheral respondToRequest:request
                          withResult:CBATTErrorInsufficientAuthorization];
        return;
    }
    H2CoreBluetoothGattBinding *binding =
        [self.serverBindings objectForKey:request.characteristic];
    if (binding == nil) {
        [peripheral respondToRequest:request withResult:CBATTErrorAttributeNotFound];
        return;
    }
    if (binding.readCallback == NULL) {
        NSData *value = binding.initialValue ?: [NSData data];
        if (request.offset > value.length) {
            [peripheral respondToRequest:request withResult:CBATTErrorInvalidOffset];
            return;
        }
        request.value = [value subdataWithRange:
            NSMakeRange(request.offset, value.length - request.offset)];
        [peripheral respondToRequest:request withResult:CBATTErrorSuccess];
        return;
    }
    uint8_t bytes[H2_PAL_BLE_ATT_MAX_VALUE_LEN];
    size_t length = 0u;
    const h2_pal_ble_gatt_access_t access = {
        .conn_handle = H2_COREBLUETOOTH_CONN_HANDLE,
        .attr_handle = binding.valueHandle,
        .offset = (uint16_t)request.offset,
    };
    h2_pal_result_t result = binding.readCallback(
        binding.callbackUser, &access, bytes, sizeof(bytes), &length);
    if (result == H2_PAL_OK) {
        request.value = [NSData dataWithBytes:bytes length:length];
    }
    [peripheral respondToRequest:request withResult:h2_corebluetooth_att_error(result)];
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral
  didReceiveWriteRequests:(NSArray<CBATTRequest *> *)requests {
    for (CBATTRequest *request in requests) {
        if (![self ensurePeripheralConnection:request.central]) {
            [peripheral respondToRequest:request
                              withResult:CBATTErrorInsufficientAuthorization];
            continue;
        }
        H2CoreBluetoothGattBinding *binding =
            [self.serverBindings objectForKey:request.characteristic];
        h2_pal_result_t result = H2_PAL_ERR_NOT_FOUND;
        if (binding != nil && binding.writeCallback != NULL) {
            const h2_pal_ble_gatt_access_t access = {
                .conn_handle = H2_COREBLUETOOTH_CONN_HANDLE,
                .attr_handle = binding.valueHandle,
                .offset = (uint16_t)request.offset,
            };
            result = binding.writeCallback(
                binding.callbackUser, &access,
                request.value.bytes, request.value.length);
        }
        [peripheral respondToRequest:request
                          withResult:h2_corebluetooth_att_error(result)];
    }
}

- (void)peripheralManagerIsReadyToUpdateSubscribers:
    (CBPeripheralManager *)peripheral {
    (void)peripheral;
    /* h2_bleikcp retries H2_PAL_ERR_WOULD_BLOCK frames from its output queue. */
}

@end

static H2CoreBluetoothBackend *h2_corebluetooth_backend(void) {
    static H2CoreBluetoothBackend *backend;
    @synchronized([H2CoreBluetoothBackend class]) {
        if (backend == nil) {
            backend = [[H2CoreBluetoothBackend alloc] init];
        }
    }
    return backend;
}

void h2_ios_corebluetooth_test_post_connected_on_backend_queue(void) {
    dispatch_async(h2_corebluetooth_backend().queue, ^{
        const h2_pal_ble_connection_t connection = {
            .conn_handle = H2_COREBLUETOOTH_CONN_HANDLE,
            .role = H2_PAL_BLE_ROLE_CENTRAL,
            .mtu = 23u,
        };
        h2_corebluetooth_post(
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
            &connection,
            sizeof(connection));
    });
}

static h2_pal_result_t h2_corebluetooth_start(void *user) {
    (void)user;
    return [h2_corebluetooth_backend() start];
}

static h2_pal_result_t h2_corebluetooth_stop(void *user) {
    (void)user;
    return [h2_corebluetooth_backend() stop];
}

static h2_pal_result_t h2_corebluetooth_set_adv_data(
    void *user,
    const h2_pal_ble_adv_data_t *data) {
    (void)user;
    return [h2_corebluetooth_backend() setAdvertisingData:data];
}

static h2_pal_result_t h2_corebluetooth_start_advertising(
    void *user,
    const h2_pal_ble_adv_params_t *params) {
    (void)user;
    if (params == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (!h2_ios_corebluetooth_adv_params_supported(params)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return [h2_corebluetooth_backend() startAdvertising];
}

static h2_pal_result_t h2_corebluetooth_stop_advertising(void *user) {
    (void)user;
    return [h2_corebluetooth_backend() stopAdvertising];
}

static h2_pal_result_t h2_corebluetooth_adv_set_create(
    void *user,
    const h2_pal_ble_adv_params_t *params,
    h2_pal_ble_adv_set_t **outSet) {
    (void)user;
    return [h2_corebluetooth_backend() createAdvertisingSet:params output:outSet];
}

static h2_pal_result_t h2_corebluetooth_adv_set_set_data(
    void *user,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
    (void)user;
    if (set != h2_corebluetooth_backend().advertisingSet) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return [h2_corebluetooth_backend() setAdvertisingData:data];
}

static h2_pal_result_t
h2_corebluetooth_adv_set_set_scan_response_data_unsupported(
    void *user,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
    (void)user;
    (void)set;
    (void)data;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_corebluetooth_adv_set_start(
    void *user,
    h2_pal_ble_adv_set_t *set) {
    (void)user;
    if (set != h2_corebluetooth_backend().advertisingSet) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return [h2_corebluetooth_backend() startAdvertising];
}

static h2_pal_result_t h2_corebluetooth_adv_set_stop(
    void *user,
    h2_pal_ble_adv_set_t *set) {
    (void)user;
    if (set != h2_corebluetooth_backend().advertisingSet) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return [h2_corebluetooth_backend() stopAdvertising];
}

static h2_pal_result_t h2_corebluetooth_adv_set_destroy(
    void *user,
    h2_pal_ble_adv_set_t *set) {
    (void)user;
    return [h2_corebluetooth_backend() destroyAdvertisingSet:set];
}

static h2_pal_result_t h2_corebluetooth_start_scan(
    void *user,
    const h2_pal_ble_scan_params_t *params,
    h2_pal_ble_scan_result_fn callback,
    void *scanUser) {
    (void)user;
    return [h2_corebluetooth_backend() startScan:params
                                        callback:callback user:scanUser];
}

static h2_pal_result_t h2_corebluetooth_stop_scan(void *user) {
    (void)user;
    return [h2_corebluetooth_backend() stopScan];
}

static h2_pal_result_t h2_corebluetooth_register_gatt_services(
    void *user,
    const h2_pal_ble_gatt_service_t *services,
    size_t count) {
    (void)user;
    return [h2_corebluetooth_backend() registerServices:services count:count];
}

static h2_pal_result_t h2_corebluetooth_unregister_gatt_services(void *user) {
    (void)user;
    return [h2_corebluetooth_backend() unregisterServices];
}

static h2_pal_result_t h2_corebluetooth_notify(
    void *user,
    uint16_t connectionHandle,
    uint16_t attributeHandle,
    const uint8_t *data,
    size_t length) {
    (void)user;
    return [h2_corebluetooth_backend() notify:connectionHandle
                                          handle:attributeHandle
                                            data:data length:length];
}

static h2_pal_result_t h2_corebluetooth_indicate(
    void *user,
    uint16_t connectionHandle,
    uint16_t attributeHandle,
    const uint8_t *data,
    size_t length,
    uint32_t timeoutMs) {
    (void)user;
    (void)connectionHandle;
    (void)attributeHandle;
    (void)data;
    (void)length;
    (void)timeoutMs;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_corebluetooth_connect(
    void *user,
    const h2_pal_ble_addr_t *address,
    const h2_pal_ble_connect_params_t *params,
    uint16_t *outHandle) {
    (void)user;
    return [h2_corebluetooth_backend() connectAddress:address
        timeout:params != NULL ? params->timeout_ms : 0u output:outHandle];
}

static h2_pal_result_t h2_corebluetooth_disconnect(
    void *user,
    uint16_t connectionHandle) {
    (void)user;
    return [h2_corebluetooth_backend() disconnect:connectionHandle];
}

static h2_pal_result_t h2_corebluetooth_unsupported_connection(
    void *user,
    uint16_t connectionHandle,
    const h2_pal_ble_connection_params_t *params) {
    (void)user;
    (void)connectionHandle;
    (void)params;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_corebluetooth_exchange_mtu(
    void *user,
    uint16_t connectionHandle,
    uint16_t *outMtu,
    uint32_t timeoutMs) {
    (void)user;
    return [h2_corebluetooth_backend() exchangeMtu:connectionHandle
        output:outMtu timeout:timeoutMs];
}

static h2_pal_result_t h2_corebluetooth_set_phy(
    void *user,
    uint16_t connectionHandle,
    h2_pal_ble_phy_t txPhy,
    h2_pal_ble_phy_t rxPhy,
    uint32_t timeoutMs) {
    (void)user;
    (void)connectionHandle;
    (void)txPhy;
    (void)rxPhy;
    (void)timeoutMs;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_corebluetooth_read_phy(
    void *user,
    uint16_t connectionHandle,
    h2_pal_ble_phy_info_t *outPhy,
    uint32_t timeoutMs) {
    (void)user;
    (void)connectionHandle;
    (void)outPhy;
    (void)timeoutMs;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_corebluetooth_discover(
    void *user,
    uint16_t connectionHandle,
    const h2_pal_ble_gatt_discovery_request_t *request,
    h2_pal_ble_gatt_discovery_entry_t *entries,
    size_t capacity,
    size_t *outCount,
    uint32_t timeoutMs) {
    (void)user;
    return [h2_corebluetooth_backend() discover:connectionHandle
        request:request entries:entries capacity:capacity count:outCount
        timeout:timeoutMs];
}

static h2_pal_result_t h2_corebluetooth_read(
    void *user,
    uint16_t connectionHandle,
    uint16_t attributeHandle,
    uint16_t offset,
    uint8_t *outBytes,
    size_t capacity,
    size_t *outCount,
    uint32_t timeoutMs) {
    (void)user;
    return [h2_corebluetooth_backend() read:connectionHandle
        handle:attributeHandle offset:offset output:outBytes capacity:capacity
        count:outCount timeout:timeoutMs];
}

static h2_pal_result_t h2_corebluetooth_write(
    void *user,
    uint16_t connectionHandle,
    uint16_t attributeHandle,
    const uint8_t *data,
    size_t length,
    bool withResponse,
    uint32_t timeoutMs) {
    (void)user;
    return [h2_corebluetooth_backend() write:connectionHandle
        handle:attributeHandle data:data length:length
        withResponse:withResponse timeout:timeoutMs];
}

static h2_pal_result_t h2_corebluetooth_subscribe(
    void *user,
    uint16_t connectionHandle,
    const h2_pal_ble_gatt_subscribe_t *subscribe,
    uint32_t timeoutMs) {
    (void)user;
    return [h2_corebluetooth_backend() subscribe:connectionHandle
        config:subscribe timeout:timeoutMs];
}

static const h2_pal_ble_vtable_t s_h2_corebluetooth_vtable = {
    .start = h2_corebluetooth_start,
    .stop = h2_corebluetooth_stop,
    .set_adv_data = h2_corebluetooth_set_adv_data,
    .start_advertising = h2_corebluetooth_start_advertising,
    .stop_advertising = h2_corebluetooth_stop_advertising,
    .adv_set_create = h2_corebluetooth_adv_set_create,
    .adv_set_set_data = h2_corebluetooth_adv_set_set_data,
    .adv_set_set_scan_response_data =
        h2_corebluetooth_adv_set_set_scan_response_data_unsupported,
    .adv_set_start = h2_corebluetooth_adv_set_start,
    .adv_set_stop = h2_corebluetooth_adv_set_stop,
    .adv_set_destroy = h2_corebluetooth_adv_set_destroy,
    .start_scan = h2_corebluetooth_start_scan,
    .stop_scan = h2_corebluetooth_stop_scan,
    .register_gatt_services = h2_corebluetooth_register_gatt_services,
    .unregister_gatt_services = h2_corebluetooth_unregister_gatt_services,
    .notify = h2_corebluetooth_notify,
    .indicate = h2_corebluetooth_indicate,
    .connect = h2_corebluetooth_connect,
    .disconnect = h2_corebluetooth_disconnect,
    .update_connection = h2_corebluetooth_unsupported_connection,
    .exchange_mtu = h2_corebluetooth_exchange_mtu,
    .set_preferred_phy = h2_corebluetooth_set_phy,
    .read_phy = h2_corebluetooth_read_phy,
    .gatt_discover = h2_corebluetooth_discover,
    .gatt_read = h2_corebluetooth_read,
    .gatt_write = h2_corebluetooth_write,
    .gatt_subscribe = h2_corebluetooth_subscribe,
};

static h2_pal_ble_t s_h2_ios_corebluetooth_api = {
    .user = NULL,
    .vtable = &s_h2_corebluetooth_vtable,
    .allocator = NULL,
};

h2_pal_ble_t *h2_ios_corebluetooth_ble(
    const h2_pal_mem_api_t *allocator) {
    if (allocator == NULL || allocator->vtable == NULL ||
        allocator->vtable->alloc == NULL ||
        allocator->vtable->realloc == NULL ||
        allocator->vtable->free == NULL) {
        return NULL;
    }
    if (s_h2_ios_corebluetooth_api.allocator != NULL &&
        s_h2_ios_corebluetooth_api.allocator != allocator) {
        return NULL;
    }
    s_h2_ios_corebluetooth_api.allocator = allocator;
    return &s_h2_ios_corebluetooth_api;
}
