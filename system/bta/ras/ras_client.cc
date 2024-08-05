/*
 * Copyright 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <base/functional/bind.h>

#include "bta/include/bta_gatt_api.h"
#include "bta/include/bta_ras_api.h"
#include "bta/ras/ras_types.h"
#include "os/logging/log_adapter.h"
#include "stack/include/bt_types.h"
#include "stack/include/btm_ble_addr.h"
#include "stack/include/gap_api.h"

using namespace bluetooth;
using namespace ::ras;
using namespace ::ras::feature;
using namespace ::ras::uuid;
using bluetooth::ras::VendorSpecificCharacteristic;

namespace {

class RasClientImpl;
RasClientImpl* instance;

enum CallbackDataType { VENDOR_SPECIFIC_REPLY };

class RasClientImpl : public bluetooth::ras::RasClient {
public:
  struct GattWriteCallbackData {
    const CallbackDataType type_;
  };

  struct RasTracker {
    RasTracker(const RawAddress& address, const RawAddress& address_for_cs)
        : address_(address), address_for_cs_(address_for_cs) {}
    uint16_t conn_id_;
    RawAddress address_;
    RawAddress address_for_cs_;
    const gatt::Service* service_ = nullptr;
    uint32_t remote_supported_features_;
    uint16_t latest_ranging_counter_ = 0;
    bool handling_on_demand_data_ = false;
    bool is_connected_ = false;
    bool service_search_complete_ = false;
    std::vector<VendorSpecificCharacteristic> vendor_specific_characteristics_;
    uint8_t writeReplyCounter_ = 0;
    uint8_t writeReplySuccessCounter_ = 0;

    const gatt::Characteristic* FindCharacteristicByUuid(Uuid uuid) {
      for (auto& characteristic : service_->characteristics) {
        if (characteristic.uuid == uuid) {
          return &characteristic;
        }
      }
      return nullptr;
    }
    const gatt::Characteristic* FindCharacteristicByHandle(uint16_t handle) {
      for (auto& characteristic : service_->characteristics) {
        if (characteristic.value_handle == handle) {
          return &characteristic;
        }
      }
      return nullptr;
    }

    VendorSpecificCharacteristic* GetVendorSpecificCharacteristic(const bluetooth::Uuid& uuid) {
      for (auto& characteristic : vendor_specific_characteristics_) {
        if (characteristic.characteristicUuid_ == uuid) {
          return &characteristic;
        }
      }
      return nullptr;
    }
  };

  void Initialize() override {
    BTA_GATTC_AppRegister(
            [](tBTA_GATTC_EVT event, tBTA_GATTC* p_data) {
              if (instance && p_data) {
                instance->GattcCallback(event, p_data);
              }
            },
            base::Bind([](uint8_t client_id, uint8_t status) {
              if (status != GATT_SUCCESS) {
                log::error("Can't start Gatt client for Ranging Service");
                return;
              }
              log::info("Initialize, client_id {}", client_id);
              instance->gatt_if_ = client_id;
            }),
            true);
  }

  void RegisterCallbacks(bluetooth::ras::RasClientCallbacks* callbacks) { callbacks_ = callbacks; }

  void Connect(const RawAddress& address) override {
    tBLE_BD_ADDR ble_bd_addr;
    ResolveAddress(ble_bd_addr, address);
    log::info("address {}, resolve {}", address, ble_bd_addr.bda);

    auto tracker = FindTrackerByAddress(ble_bd_addr.bda);
    if (tracker == nullptr) {
      trackers_.emplace_back(std::make_shared<RasTracker>(ble_bd_addr.bda, address));
    } else if (tracker->is_connected_) {
      log::info("Already connected");
      uint16_t att_handle = tracker->FindCharacteristicByUuid(kRasRealTimeRangingDataCharacteristic)
                                    ->value_handle;
      callbacks_->OnConnected(address, att_handle, tracker->vendor_specific_characteristics_);
      return;
    }
    BTA_GATTC_Open(gatt_if_, ble_bd_addr.bda, BTM_BLE_DIRECT_CONNECTION, true);
  }

  void SendVendorSpecificReply(
          const RawAddress& address,
          const std::vector<VendorSpecificCharacteristic>& vendor_specific_data) {
    tBLE_BD_ADDR ble_bd_addr;
    ResolveAddress(ble_bd_addr, address);
    log::info("address {}, resolve {}", address, ble_bd_addr.bda);
    auto tracker = FindTrackerByAddress(ble_bd_addr.bda);

    for (auto& vendor_specific_characteristic : vendor_specific_data) {
      auto characteristic =
              tracker->FindCharacteristicByUuid(vendor_specific_characteristic.characteristicUuid_);
      if (characteristic == nullptr) {
        log::warn("Can't find characteristic uuid {}",
                  vendor_specific_characteristic.characteristicUuid_);
        return;
      }
      log::debug("write to remote, uuid {}, len {}",
                 vendor_specific_characteristic.characteristicUuid_,
                 vendor_specific_characteristic.value_.size());
      BTA_GATTC_WriteCharValue(tracker->conn_id_, characteristic->value_handle, GATT_WRITE_NO_RSP,
                               vendor_specific_characteristic.value_, GATT_AUTH_REQ_NO_MITM,
                               GattWriteCallback, &gatt_write_callback_data_);
    }
  }

  void GattcCallback(tBTA_GATTC_EVT event, tBTA_GATTC* p_data) {
    log::debug("event: {}", gatt_client_event_text(event));
    switch (event) {
      case BTA_GATTC_OPEN_EVT: {
        OnGattConnected(p_data->open);
      } break;
      case BTA_GATTC_CLOSE_EVT: {
        OnGattDisconnected(p_data->close);
        break;
      }
      case BTA_GATTC_SEARCH_CMPL_EVT: {
        OnGattServiceSearchComplete(p_data->search_cmpl);
      } break;
      case BTA_GATTC_NOTIF_EVT: {
        OnGattNotification(p_data->notify);
      } break;
      default:
        log::warn("Unhandled event: {}", gatt_client_event_text(event));
    }
  }

  void OnGattConnected(const tBTA_GATTC_OPEN& evt) {
    log::info("{}, conn_id=0x{:04x}, transport:{}, status:{}", evt.remote_bda, evt.conn_id,
              bt_transport_text(evt.transport), gatt_status_text(evt.status));

    if (evt.transport != BT_TRANSPORT_LE) {
      log::warn("Only LE connection is allowed (transport {})", bt_transport_text(evt.transport));
      BTA_GATTC_Close(evt.conn_id);
      return;
    }

    auto tracker = FindTrackerByAddress(evt.remote_bda);
    if (tracker == nullptr) {
      log::warn("Skipping unknown device, address: {}", evt.remote_bda);
      BTA_GATTC_Close(evt.conn_id);
      return;
    }

    if (evt.status != GATT_SUCCESS) {
      log::error("Failed to connect to server device {}", evt.remote_bda);
      return;
    }
    tracker->conn_id_ = evt.conn_id;
    tracker->is_connected_ = true;
    log::info("Search service");
    BTA_GATTC_ServiceSearchRequest(tracker->conn_id_, kRangingService);
  }

  void OnGattDisconnected(const tBTA_GATTC_CLOSE& evt) {
    log::info("{}, conn_id=0x{:04x}, status:{}, reason:{}", evt.remote_bda, evt.conn_id,
              gatt_status_text(evt.status), gatt_disconnection_reason_text(evt.reason));

    auto tracker = FindTrackerByAddress(evt.remote_bda);
    if (tracker == nullptr) {
      log::warn("Skipping unknown device, address: {}", evt.remote_bda);
      BTA_GATTC_Close(evt.conn_id);
      return;
    }
    trackers_.remove(tracker);
  }

  void OnGattServiceSearchComplete(const tBTA_GATTC_SEARCH_CMPL& evt) {
    auto tracker = FindTrackerByHandle(evt.conn_id);
    if (tracker == nullptr) {
      log::warn("Can't find tracker for conn_id:{}", evt.conn_id);
      return;
    }

    // Get Ranging Service
    bool service_found = false;
    const std::list<gatt::Service>* all_services = BTA_GATTC_GetServices(evt.conn_id);
    for (const auto& service : *all_services) {
      if (service.uuid == kRangingService) {
        tracker->service_ = &service;
        service_found = true;
        break;
      }
    }

    if (tracker->service_search_complete_) {
      log::info("Service search already completed, ignore");
      return;
    } else if (!service_found) {
      log::error("Can't find Ranging Service in the services list");
      return;
    } else {
      log::info("Found Ranging Service");
      tracker->service_search_complete_ = true;
      ListCharacteristic(tracker);
    }

    // Read Vendor Specific Uuid
    if (!tracker->vendor_specific_characteristics_.empty()) {
      for (auto& vendor_specific_characteristic : tracker->vendor_specific_characteristics_) {
        log::debug("Read vendor specific characteristic uuid {}",
                   vendor_specific_characteristic.characteristicUuid_);
        auto characteristic = tracker->FindCharacteristicByUuid(
                vendor_specific_characteristic.characteristicUuid_);

        BTA_GATTC_ReadCharacteristic(
                tracker->conn_id_, characteristic->value_handle, GATT_AUTH_REQ_NO_MITM,
                [](uint16_t conn_id, tGATT_STATUS status, uint16_t handle, uint16_t len,
                   uint8_t* value, void* data) {
                  instance->OnReadCharacteristicCallback(conn_id, status, handle, len, value, data);
                },
                nullptr);
      }
    }

    // Read Ras Features
    log::info("Read Ras Features");
    auto characteristic = tracker->FindCharacteristicByUuid(kRasFeaturesCharacteristic);
    if (characteristic == nullptr) {
      log::error("Can not find Characteristic for Ras Features");
      return;
    }
    BTA_GATTC_ReadCharacteristic(
            tracker->conn_id_, characteristic->value_handle, GATT_AUTH_REQ_NO_MITM,
            [](uint16_t conn_id, tGATT_STATUS status, uint16_t handle, uint16_t len, uint8_t* value,
               void* data) {
              instance->OnReadCharacteristicCallback(conn_id, status, handle, len, value, data);
            },
            nullptr);

    SubscribeCharacteristic(tracker, kRasControlPointCharacteristic);
  }

  void OnGattNotification(const tBTA_GATTC_NOTIFY& evt) {
    auto tracker = FindTrackerByHandle(evt.conn_id);
    if (tracker == nullptr) {
      log::warn("Can't find tracker for conn_id:{}", evt.conn_id);
      return;
    }
    auto characteristic = tracker->FindCharacteristicByHandle(evt.handle);
    if (characteristic == nullptr) {
      log::warn("Can't find characteristic for handle:{}", evt.handle);
      return;
    }

    uint16_t uuid_16bit = characteristic->uuid.As16Bit();
    log::debug("Handle uuid 0x{:04x}, {}, size {}", uuid_16bit, getUuidName(characteristic->uuid),
               evt.len);

    switch (uuid_16bit) {
      case kRasRealTimeRangingDataCharacteristic16bit:
      case kRasOnDemandDataCharacteristic16bit: {
        OnRemoteData(evt, tracker);
        break;
      }
      case kRasControlPointCharacteristic16bit: {
        OnControlPointEvent(evt, tracker);
      } break;
      case kRasRangingDataReadyCharacteristic16bit: {
        OnRangingDataReady(evt, tracker);
      } break;
      default:
        log::warn("Unexpected UUID");
    }
  }

  void OnRemoteData(const tBTA_GATTC_NOTIFY& evt, std::shared_ptr<RasTracker> tracker) {
    std::vector<uint8_t> data;
    data.resize(evt.len);
    std::copy(evt.value, evt.value + evt.len, data.begin());
    callbacks_->OnRemoteData(tracker->address_for_cs_, data);
  }

  void OnControlPointEvent(const tBTA_GATTC_NOTIFY& evt, std::shared_ptr<RasTracker> tracker) {
    switch (evt.value[0]) {
      case (uint8_t)EventCode::COMPLETE_RANGING_DATA_RESPONSE: {
        uint16_t ranging_counter = evt.value[1];
        ranging_counter |= (evt.value[2] << 8);
        log::debug("Received complete ranging data response, ranging_counter: {}", ranging_counter);
        AckRangingData(ranging_counter, tracker);
      } break;
      case (uint8_t)EventCode::RESPONSE_CODE: {
        tracker->handling_on_demand_data_ = false;
        log::debug("Received response code 0x{:02x}", evt.value[1]);
      } break;
      default:
        log::warn("Unexpected event code 0x{:02x}", evt.value[0]);
    }
  }

  void OnRangingDataReady(const tBTA_GATTC_NOTIFY& evt, std::shared_ptr<RasTracker> tracker) {
    if (evt.len != kRingingCounterSize) {
      log::error("Invalid len for ranging data ready");
      return;
    }
    uint16_t ranging_counter = evt.value[0];
    ranging_counter |= (evt.value[1] << 8);
    log::debug("ranging_counter: {}", ranging_counter);

    // Send get ranging data command
    tracker->latest_ranging_counter_ = ranging_counter;
    GetRangingData(ranging_counter, tracker);
  }

  void GetRangingData(uint16_t ranging_counter, std::shared_ptr<RasTracker> tracker) {
    log::debug("ranging_counter:{}", ranging_counter);
    if (tracker->handling_on_demand_data_) {
      log::warn("Handling other procedure, skip");
      return;
    }

    auto characteristic = tracker->FindCharacteristicByUuid(kRasControlPointCharacteristic);
    if (characteristic == nullptr) {
      log::warn("Can't find characteristic for RAS-CP");
      return;
    }

    tracker->handling_on_demand_data_ = true;
    std::vector<uint8_t> value(3);
    value[0] = (uint8_t)Opcode::GET_RANGING_DATA;
    value[1] = (uint8_t)(ranging_counter & 0xFF);
    value[2] = (uint8_t)((ranging_counter >> 8) & 0xFF);
    BTA_GATTC_WriteCharValue(tracker->conn_id_, characteristic->value_handle, GATT_WRITE_NO_RSP,
                             value, GATT_AUTH_REQ_NO_MITM, GattWriteCallback, nullptr);
  }

  void AckRangingData(uint16_t ranging_counter, std::shared_ptr<RasTracker> tracker) {
    log::debug("ranging_counter:{}", ranging_counter);
    auto characteristic = tracker->FindCharacteristicByUuid(kRasControlPointCharacteristic);
    if (characteristic == nullptr) {
      log::warn("Can't find characteristic for RAS-CP");
      return;
    }
    tracker->handling_on_demand_data_ = false;
    std::vector<uint8_t> value(3);
    value[0] = (uint8_t)Opcode::ACK_RANGING_DATA;
    value[1] = (uint8_t)(ranging_counter & 0xFF);
    value[2] = (uint8_t)((ranging_counter >> 8) & 0xFF);
    BTA_GATTC_WriteCharValue(tracker->conn_id_, characteristic->value_handle, GATT_WRITE_NO_RSP,
                             value, GATT_AUTH_REQ_NO_MITM, GattWriteCallback, nullptr);
    if (ranging_counter != tracker->latest_ranging_counter_) {
      GetRangingData(tracker->latest_ranging_counter_, tracker);
    }
  }

  void GattWriteCallbackForVendorSpecificData(uint16_t conn_id, tGATT_STATUS status,
                                              uint16_t handle, const uint8_t* value,
                                              GattWriteCallbackData* data) {
    if (data != nullptr) {
      GattWriteCallbackData* structPtr = static_cast<GattWriteCallbackData*>(data);
      if (structPtr->type_ == CallbackDataType::VENDOR_SPECIFIC_REPLY) {
        log::info("Write vendor specific reply complete");
        auto tracker = FindTrackerByHandle(conn_id);
        tracker->writeReplyCounter_++;
        if (status == GATT_SUCCESS) {
          tracker->writeReplySuccessCounter_++;
        } else {
          log::error(
                  "Fail to write vendor specific reply conn_id {}, status {}, "
                  "handle {}",
                  conn_id, gatt_status_text(status), handle);
        }
        // All reply complete
        if (tracker->writeReplyCounter_ == tracker->vendor_specific_characteristics_.size()) {
          log::info(
                  "All vendor specific reply write complete, size {} "
                  "successCounter {}",
                  tracker->vendor_specific_characteristics_.size(),
                  tracker->writeReplySuccessCounter_);
          bool success = tracker->writeReplySuccessCounter_ ==
                         tracker->vendor_specific_characteristics_.size();
          tracker->writeReplyCounter_ = 0;
          tracker->writeReplySuccessCounter_ = 0;
          callbacks_->OnWriteVendorSpecificReplyComplete(tracker->address_for_cs_, success);
        }
        return;
      }
    }
  }

  void GattWriteCallback(uint16_t conn_id, tGATT_STATUS status, uint16_t handle,
                         const uint8_t* value) {
    if (status != GATT_SUCCESS) {
      log::error("Fail to write conn_id {}, status {}, handle {}", conn_id,
                 gatt_status_text(status), handle);
      auto tracker = FindTrackerByHandle(conn_id);
      if (tracker == nullptr) {
        log::warn("Can't find tracker for conn_id:{}", conn_id);
        return;
      }
      auto characteristic = tracker->FindCharacteristicByHandle(handle);
      if (characteristic == nullptr) {
        log::warn("Can't find characteristic for handle:{}", handle);
        return;
      }

      if (characteristic->uuid == kRasControlPointCharacteristic) {
        log::error("Write RAS-CP command fail");
        tracker->handling_on_demand_data_ = false;
      }
      return;
    }
  }

  static void GattWriteCallback(uint16_t conn_id, tGATT_STATUS status, uint16_t handle,
                                uint16_t len, const uint8_t* value, void* data) {
    if (instance != nullptr) {
      if (data != nullptr) {
        GattWriteCallbackData* structPtr = static_cast<GattWriteCallbackData*>(data);
        if (structPtr->type_ == CallbackDataType::VENDOR_SPECIFIC_REPLY) {
          instance->GattWriteCallbackForVendorSpecificData(conn_id, status, handle, value,
                                                           structPtr);
          return;
        }
      }
      instance->GattWriteCallback(conn_id, status, handle, value);
    }
  }

  void SubscribeCharacteristic(std::shared_ptr<RasTracker> tracker, const Uuid uuid) {
    auto characteristic = tracker->FindCharacteristicByUuid(uuid);
    if (characteristic == nullptr) {
      log::warn("Can't find characteristic 0x{:04x}", uuid.As16Bit());
      return;
    }
    uint16_t ccc_handle = FindCccHandle(characteristic);
    if (ccc_handle == GAP_INVALID_HANDLE) {
      log::warn("Can't find Client Characteristic Configuration descriptor");
      return;
    }

    tGATT_STATUS register_status = BTA_GATTC_RegisterForNotifications(gatt_if_, tracker->address_,
                                                                      characteristic->value_handle);
    if (register_status != GATT_SUCCESS) {
      log::error("Fail to register, {}", gatt_status_text(register_status));
      return;
    }

    std::vector<uint8_t> value(2);
    uint8_t* value_ptr = value.data();
    // Register notify is supported
    if (characteristic->properties & GATT_CHAR_PROP_BIT_NOTIFY) {
      UINT16_TO_STREAM(value_ptr, GATT_CHAR_CLIENT_CONFIG_NOTIFICATION);
    } else {
      UINT16_TO_STREAM(value_ptr, GATT_CHAR_CLIENT_CONFIG_INDICTION);
    }
    BTA_GATTC_WriteCharDescr(
        tracker->conn_id_, ccc_handle, value, GATT_AUTH_REQ_NONE,
        [](uint16_t conn_id, tGATT_STATUS status, uint16_t handle, uint16_t len,
           const uint8_t* value, void* data) {
          if (instance)
            instance->OnDescriptorWrite(conn_id, status, handle, len, value,
                                        data);
        },
        nullptr);
  }

  void OnDescriptorWrite(uint16_t conn_id, tGATT_STATUS status, uint16_t handle,
                         uint16_t len, const uint8_t* value, void* data) {
    log::info("conn_id:{}, handle:{}, status:{}", conn_id, handle,
              gatt_status_text(status));
  }

  void ListCharacteristic(std::shared_ptr<RasTracker> tracker) {
    tracker->vendor_specific_characteristics_.clear();
    for (auto& characteristic : tracker->service_->characteristics) {
      bool vendor_specific =
          !IsRangingServiceCharacteristic(characteristic.uuid);
      log::info(
          "{}Characteristic uuid:0x{:04x}, handle:0x{:04x}, "
          "properties:0x{:02x}, "
          "{}",
          vendor_specific ? "Vendor Specific " : "",
          characteristic.uuid.As16Bit(), characteristic.value_handle,
          characteristic.properties, getUuidName(characteristic.uuid));
      if (vendor_specific) {
        VendorSpecificCharacteristic vendor_specific_characteristic;
        vendor_specific_characteristic.characteristicUuid_ =
            characteristic.uuid;
        tracker->vendor_specific_characteristics_.emplace_back(
            vendor_specific_characteristic);
      }
      for (auto& descriptor : characteristic.descriptors) {
        log::info("\tDescriptor uuid:0x{:04x}, handle:0x{:04x}, {}",
                  descriptor.uuid.As16Bit(), descriptor.handle,
                  getUuidName(descriptor.uuid));
      }
    }
  }

  void ResolveAddress(tBLE_BD_ADDR& ble_bd_addr, const RawAddress& address) {
    ble_bd_addr.bda = address;
    ble_bd_addr.type = BLE_ADDR_RANDOM;
    maybe_resolve_address(&ble_bd_addr.bda, &ble_bd_addr.type);
  }

  void OnReadCharacteristicCallback(uint16_t conn_id, tGATT_STATUS status, uint16_t handle,
                                    uint16_t len, uint8_t* value, void* data) {
    log::info("conn_id: {}, handle: {}, len: {}", conn_id, handle, len);
    if (status != GATT_SUCCESS) {
      log::error("Fail with status {}", gatt_status_text(status));
      return;
    }
    auto tracker = FindTrackerByHandle(conn_id);
    if (tracker == nullptr) {
      log::warn("Can't find tracker for conn_id:{}", conn_id);
      return;
    }
    auto characteristic = tracker->FindCharacteristicByHandle(handle);
    if (characteristic == nullptr) {
      log::warn("Can't find characteristic for handle:{}", handle);
      return;
    }

    auto vendor_specific_characteristic =
            tracker->GetVendorSpecificCharacteristic(characteristic->uuid);
    if (vendor_specific_characteristic != nullptr) {
      log::info("Update vendor specific data, uuid: {}",
                vendor_specific_characteristic->characteristicUuid_);
      vendor_specific_characteristic->value_.clear();
      vendor_specific_characteristic->value_.reserve(len);
      vendor_specific_characteristic->value_.assign(value, value + len);
      return;
    }

    uint16_t uuid_16bit = characteristic->uuid.As16Bit();
    log::info("Handle uuid 0x{:04x}, {}", uuid_16bit, getUuidName(characteristic->uuid));

    switch (uuid_16bit) {
      case kRasFeaturesCharacteristic16bit: {
        if (len != kFeatureSize) {
          log::error("Invalid len for Ras features");
          return;
        }
        STREAM_TO_UINT32(tracker->remote_supported_features_, value);
        log::info("Remote supported features : {}",
                  getFeaturesString(tracker->remote_supported_features_));
        if (tracker->remote_supported_features_ & feature::kRealTimeRangingData) {
          log::info("Subscribe Real-time Ranging Data");
          SubscribeCharacteristic(tracker, kRasRealTimeRangingDataCharacteristic);
        } else {
          log::info("Subscribe On-demand Ranging Data");
          SubscribeCharacteristic(tracker, kRasOnDemandDataCharacteristic);
          SubscribeCharacteristic(tracker, kRasRangingDataReadyCharacteristic);
          SubscribeCharacteristic(tracker, kRasRangingDataOverWrittenCharacteristic);
        }
        uint16_t att_handle =
                tracker->FindCharacteristicByUuid(kRasRealTimeRangingDataCharacteristic)
                        ->value_handle;
        callbacks_->OnConnected(tracker->address_for_cs_, att_handle,
                                tracker->vendor_specific_characteristics_);
      } break;
      default:
        log::warn("Unexpected UUID");
    }
  }

  std::string getFeaturesString(uint32_t value) {
    std::stringstream ss;
    ss << value;
    if (value == 0) {
      ss << "|No feature supported";
    } else {
      if ((value & kRealTimeRangingData) != 0) {
        ss << "|Real-time Ranging Data";
      }
      if ((value & kRetrieveLostRangingDataSegments) != 0) {
        ss << "|Retrieve Lost Ranging Data Segments";
      }
      if ((value & kAbortOperation) != 0) {
        ss << "|Abort Operation";
      }
      if ((value & kFilterRangingData) != 0) {
        ss << "|Filter Ranging Data";
      }
    }
    return ss.str();
  }

  uint16_t FindCccHandle(const gatt::Characteristic* characteristic) {
    for (auto descriptor : characteristic->descriptors) {
      if (descriptor.uuid == kClientCharacteristicConfiguration) {
        return descriptor.handle;
      }
    }
    return GAP_INVALID_HANDLE;
  }

  std::shared_ptr<RasTracker> FindTrackerByHandle(uint16_t conn_id) const {
    for (auto tracker : trackers_) {
      if (tracker->conn_id_ == conn_id) {
        return tracker;
      }
    }
    return nullptr;
  }

  std::shared_ptr<RasTracker> FindTrackerByAddress(const RawAddress& address) const {
    for (auto tracker : trackers_) {
      if (tracker->address_ == address) {
        return tracker;
      }
    }
    return nullptr;
  }

private:
  uint16_t gatt_if_;
  std::list<std::shared_ptr<RasTracker>> trackers_;
  bluetooth::ras::RasClientCallbacks* callbacks_;
  GattWriteCallbackData gatt_write_callback_data_{CallbackDataType::VENDOR_SPECIFIC_REPLY};
};

}  // namespace

bluetooth::ras::RasClient* bluetooth::ras::GetRasClient() {
  if (instance == nullptr) {
    instance = new RasClientImpl();
  }
  return instance;
}
