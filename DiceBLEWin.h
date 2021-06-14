#pragma once
#include <windows.h>
#include "IUnityInterface.h"

typedef void(*DebugCallback)(const char* message);
typedef void(*SendBluetoothMessageCallback)(const char* objectName, const char* methodName, const char* message);

extern "C"
{
    void UNITY_INTERFACE_EXPORT _winBluetoothLEConnectCallbacks(SendBluetoothMessageCallback sendMessageMethod, DebugCallback callbackMethod, DebugCallback warningMethod, DebugCallback errorMethod);
    void UNITY_INTERFACE_EXPORT _winBluetoothLEDisconnectCallbacks();

    void UNITY_INTERFACE_EXPORT _winBluetoothLELog(const char* message);
    void UNITY_INTERFACE_EXPORT _winBluetoothLEInitialize(bool asCentral, bool asPeripheral);
    void UNITY_INTERFACE_EXPORT _winBluetoothLEDeInitialize();
    void UNITY_INTERFACE_EXPORT _winBluetoothLEPauseMessages(bool isPaused);
    void UNITY_INTERFACE_EXPORT _winBluetoothLEScanForPeripheralsWithServices(const char* serviceUUIDsString, bool allowDuplicates, bool rssiOnly, bool clearPeripheralList);
    void UNITY_INTERFACE_EXPORT _winBluetoothLERetrieveListOfPeripheralsWithServices(const char* serviceUUIDsString);
    void UNITY_INTERFACE_EXPORT _winBluetoothLEStopScan();
    void UNITY_INTERFACE_EXPORT _winBluetoothLEConnectToPeripheral(const char* name);
    void UNITY_INTERFACE_EXPORT _winBluetoothLEDisconnectPeripheral(const char* name);
    void UNITY_INTERFACE_EXPORT _winBluetoothLEReadCharacteristic(const char* name, const char* service, const char* characteristic);
    void UNITY_INTERFACE_EXPORT _winBluetoothLEWriteCharacteristic(const char* name, const char* service, const char* characteristic, const unsigned char* data, int length, bool withResponse);
    void UNITY_INTERFACE_EXPORT _winBluetoothLESubscribeCharacteristic(const char* name, const char* service, const char* characteristic);
    void UNITY_INTERFACE_EXPORT _winBluetoothLEUnSubscribeCharacteristic(const char* name, const char* service, const char* characteristic);
    void UNITY_INTERFACE_EXPORT _winBluetoothLEDisconnectAll();
    void UNITY_INTERFACE_EXPORT _winBluetoothLEUpdate();


    void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces);
    void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload();
}