/*
 * Copyright (C) 2016 The Android Open Source Project
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
 *
 * Changes from Qualcomm Innovation Center, Inc. are provided under the following license:
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

package com.android.bluetooth.gatt;

import android.bluetooth.BluetoothUuid;
import android.bluetooth.le.AdvertiseData;
import android.bluetooth.le.TransportDiscoveryData;
import android.os.ParcelUuid;
import android.util.Log;

import java.io.ByteArrayOutputStream;

class AdvertiseHelper {

    private static final String TAG = "AdvertiseHelper";

    private static final int DEVICE_NAME_MAX = 26;

    private static final int COMPLETE_LIST_16_BIT_SERVICE_UUIDS = 0X03;
    private static final int COMPLETE_LIST_32_BIT_SERVICE_UUIDS = 0X05;
    private static final int COMPLETE_LIST_128_BIT_SERVICE_UUIDS = 0X07;
    private static final int SHORTENED_LOCAL_NAME = 0X08;
    private static final int COMPLETE_LOCAL_NAME = 0X09;
    private static final int TX_POWER_LEVEL = 0x0A;
    private static final int LIST_16_BIT_SERVICE_SOLICITATION_UUIDS = 0X14;
    private static final int LIST_128_BIT_SERVICE_SOLICITATION_UUIDS = 0X15;
    private static final int SERVICE_DATA_16_BIT_UUID = 0X16;
    private static final int LIST_32_BIT_SERVICE_SOLICITATION_UUIDS = 0x1F;
    private static final int SERVICE_DATA_32_BIT_UUID = 0X20;
    private static final int SERVICE_DATA_128_BIT_UUID = 0X21;
    private static final int MANUFACTURER_SPECIFIC_DATA = 0XFF;

    public static byte[] advertiseDataToBytes(AdvertiseData data, String name, boolean encrypt) {

        if (data == null) {
            return new byte[0];
        }

        // Flags are added by lower layers of the stack, only if needed;
        // no need to add them here.

        ByteArrayOutputStream ret = new ByteArrayOutputStream();

        if (data.getIncludeDeviceName()
                && ((!encrypt && !data.getDeviceNameEnc())
                        || (encrypt && data.getDeviceNameEnc()))) {
            try {
                byte[] nameBytes = name.getBytes("UTF-8");

                int nameLength = nameBytes.length;
                byte type;

                // TODO(jpawlowski) put a better limit on device name!
                if (nameLength > DEVICE_NAME_MAX) {
                    nameLength = DEVICE_NAME_MAX;
                    type = SHORTENED_LOCAL_NAME;
                } else {
                    type = COMPLETE_LOCAL_NAME;
                }

                check_length(type, nameLength + 1);
                ret.write(nameLength + 1);
                ret.write(type);
                ret.write(nameBytes, 0, nameLength);

            } catch (java.io.UnsupportedEncodingException e) {
                Log.e(TAG, "Can't include name - encoding error!", e);
            }
        }

        if ((!encrypt && !data.getManufacturerSpecificDataEnc())
                || (encrypt && data.getManufacturerSpecificDataEnc())) {
            for (int i = 0; i < data.getManufacturerSpecificData().size(); i++) {
                int manufacturerId = data.getManufacturerSpecificData().keyAt(i);

                byte[] manufacturerData = data.getManufacturerSpecificData().get(manufacturerId);
                int dataLen = 2 + (manufacturerData == null ? 0 : manufacturerData.length);
                byte[] concated = new byte[dataLen];
                // First two bytes are manufacturer id in little-endian.
                concated[0] = (byte) (manufacturerId & 0xFF);
                concated[1] = (byte) ((manufacturerId >> 8) & 0xFF);
                if (manufacturerData != null) {
                    System.arraycopy(manufacturerData, 0, concated, 2, manufacturerData.length);
                }

                check_length(MANUFACTURER_SPECIFIC_DATA, concated.length + 1);
                ret.write(concated.length + 1);
                ret.write(MANUFACTURER_SPECIFIC_DATA);
                ret.write(concated, 0, concated.length);
            }
        }

        if (data.getIncludeTxPowerLevel()
                && ((!encrypt && !data.getTxPowerLevelEnc())
                        || (encrypt && data.getTxPowerLevelEnc()))) {
            ret.write(2 /* Length */);
            ret.write(TX_POWER_LEVEL);
            ret.write(0); // lower layers will fill this value.
        }

        if (data.getServiceUuids() != null
                && (!encrypt && !data.getServiceUuidsEnc()
                        || (encrypt && data.getServiceUuidsEnc()))) {
            ByteArrayOutputStream serviceUuids16 = new ByteArrayOutputStream();
            ByteArrayOutputStream serviceUuids32 = new ByteArrayOutputStream();
            ByteArrayOutputStream serviceUuids128 = new ByteArrayOutputStream();

            for (ParcelUuid parcelUuid : data.getServiceUuids()) {
                byte[] uuid = BluetoothUuid.uuidToBytes(parcelUuid);

                if (uuid.length == BluetoothUuid.UUID_BYTES_16_BIT) {
                    serviceUuids16.write(uuid, 0, uuid.length);
                } else if (uuid.length == BluetoothUuid.UUID_BYTES_32_BIT) {
                    serviceUuids32.write(uuid, 0, uuid.length);
                } else /*if (uuid.length == BluetoothUuid.UUID_BYTES_128_BIT)*/ {
                    serviceUuids128.write(uuid, 0, uuid.length);
                }
            }

            if (serviceUuids16.size() != 0) {
                check_length(COMPLETE_LIST_16_BIT_SERVICE_UUIDS, serviceUuids16.size() + 1);
                ret.write(serviceUuids16.size() + 1);
                ret.write(COMPLETE_LIST_16_BIT_SERVICE_UUIDS);
                ret.write(serviceUuids16.toByteArray(), 0, serviceUuids16.size());
            }

            if (serviceUuids32.size() != 0) {
                check_length(COMPLETE_LIST_32_BIT_SERVICE_UUIDS, serviceUuids32.size() + 1);
                ret.write(serviceUuids32.size() + 1);
                ret.write(COMPLETE_LIST_32_BIT_SERVICE_UUIDS);
                ret.write(serviceUuids32.toByteArray(), 0, serviceUuids32.size());
            }

            if (serviceUuids128.size() != 0) {
                check_length(COMPLETE_LIST_128_BIT_SERVICE_UUIDS, serviceUuids32.size() + 1);
                ret.write(serviceUuids128.size() + 1);
                ret.write(COMPLETE_LIST_128_BIT_SERVICE_UUIDS);
                ret.write(serviceUuids128.toByteArray(), 0, serviceUuids128.size());
            }
        }

        if (!data.getServiceData().isEmpty()
                && (!encrypt && !data.getServiceDataEnc()
                        || (encrypt && data.getServiceDataEnc()))) {
            for (ParcelUuid parcelUuid : data.getServiceData().keySet()) {
                byte[] serviceData = data.getServiceData().get(parcelUuid);

                byte[] uuid = BluetoothUuid.uuidToBytes(parcelUuid);
                int uuidLen = uuid.length;

                int dataLen = uuidLen + (serviceData == null ? 0 : serviceData.length);
                byte[] concated = new byte[dataLen];

                System.arraycopy(uuid, 0, concated, 0, uuidLen);

                if (serviceData != null) {
                    System.arraycopy(serviceData, 0, concated, uuidLen, serviceData.length);
                }

                if (uuid.length == BluetoothUuid.UUID_BYTES_16_BIT) {
                    check_length(SERVICE_DATA_16_BIT_UUID, concated.length + 1);
                    ret.write(concated.length + 1);
                    ret.write(SERVICE_DATA_16_BIT_UUID);
                    ret.write(concated, 0, concated.length);
                } else if (uuid.length == BluetoothUuid.UUID_BYTES_32_BIT) {
                    check_length(SERVICE_DATA_32_BIT_UUID, concated.length + 1);
                    ret.write(concated.length + 1);
                    ret.write(SERVICE_DATA_32_BIT_UUID);
                    ret.write(concated, 0, concated.length);
                } else /*if (uuid.length == BluetoothUuid.UUID_BYTES_128_BIT)*/ {
                    check_length(SERVICE_DATA_128_BIT_UUID, concated.length + 1);
                    ret.write(concated.length + 1);
                    ret.write(SERVICE_DATA_128_BIT_UUID);
                    ret.write(concated, 0, concated.length);
                }
            }
        }

        if (data.getServiceSolicitationUuids() != null
                && ((!encrypt && !data.getServiceSolicitationUuidsEnc())
                        || (encrypt && data.getServiceSolicitationUuidsEnc()))) {
            ByteArrayOutputStream serviceUuids16 = new ByteArrayOutputStream();
            ByteArrayOutputStream serviceUuids32 = new ByteArrayOutputStream();
            ByteArrayOutputStream serviceUuids128 = new ByteArrayOutputStream();

            for (ParcelUuid parcelUuid : data.getServiceSolicitationUuids()) {
                byte[] uuid = BluetoothUuid.uuidToBytes(parcelUuid);

                if (uuid.length == BluetoothUuid.UUID_BYTES_16_BIT) {
                    serviceUuids16.write(uuid, 0, uuid.length);
                } else if (uuid.length == BluetoothUuid.UUID_BYTES_32_BIT) {
                    serviceUuids32.write(uuid, 0, uuid.length);
                } else /*if (uuid.length == BluetoothUuid.UUID_BYTES_128_BIT)*/ {
                    serviceUuids128.write(uuid, 0, uuid.length);
                }
            }

            if (serviceUuids16.size() != 0) {
                check_length(LIST_16_BIT_SERVICE_SOLICITATION_UUIDS, serviceUuids16.size() + 1);
                ret.write(serviceUuids16.size() + 1);
                ret.write(LIST_16_BIT_SERVICE_SOLICITATION_UUIDS);
                ret.write(serviceUuids16.toByteArray(), 0, serviceUuids16.size());
            }

            if (serviceUuids32.size() != 0) {
                check_length(LIST_32_BIT_SERVICE_SOLICITATION_UUIDS, serviceUuids32.size() + 1);
                ret.write(serviceUuids32.size() + 1);
                ret.write(LIST_32_BIT_SERVICE_SOLICITATION_UUIDS);
                ret.write(serviceUuids32.toByteArray(), 0, serviceUuids32.size());
            }

            if (serviceUuids128.size() != 0) {
                check_length(LIST_128_BIT_SERVICE_SOLICITATION_UUIDS, serviceUuids128.size() + 1);
                ret.write(serviceUuids128.size() + 1);
                ret.write(LIST_128_BIT_SERVICE_SOLICITATION_UUIDS);
                ret.write(serviceUuids128.toByteArray(), 0, serviceUuids128.size());
            }
        }

        if ((!encrypt && !data.getTransportDiscoveryDataEnc())
                || (encrypt && data.getTransportDiscoveryDataEnc())) {
            for (TransportDiscoveryData transportDiscoveryData : data.getTransportDiscoveryData()) {
                ret.write(transportDiscoveryData.totalBytes());
                ret.write(
                        transportDiscoveryData.toByteArray(),
                        0,
                        transportDiscoveryData.totalBytes());
            }
        }

        return ret.toByteArray();
    }

    static void check_length(int type, int length) {
        if (length > 255) {
            Log.w(
                    TAG,
                    "Length ("
                            + length
                            + ") of data with type "
                            + Integer.toString(type, 16)
                            + " is greater than 255");
            throw new IllegalArgumentException("Length of data is greater than 255");
        }
    }
}
