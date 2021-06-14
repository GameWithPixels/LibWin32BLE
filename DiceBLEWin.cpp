
#include "stdafx.h"
#include "DiceBLEWin.h"
#include "Utils.h"

#pragma warning (disable: 4068)

#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <setupapi.h>
#include <devguid.h>
#include <regstr.h>
#include <bthdef.h>
#include <bluetoothleapis.h>
#include <comdef.h>

#include <iostream>
#include <sstream>
#include <string>
#include <locale>
#include <array>
#include <vector>
#include <algorithm>    // std::find_if
#include <regex>
#include <mutex>          // std::mutex, std::unique_lock, std::defer_lock

#pragma comment(lib, "SetupAPI")
#pragma comment(lib, "BluetoothApis.lib")

//#define LOG_TO_FILE

void LogToFile(const char* message)
{
#if defined(LOG_TO_FILE)
	char buf[256];
	sprintf_s(buf, 256, "%08x:%s", GetCurrentThreadId(), message);
	FILE* f = NULL;
	fopen_s(&f, "c:\\temp\\debuglog.txt", "a+");
	while (f == NULL) {
		Sleep(10);
		fopen_s(&f, "c:\\temp\\debuglog.txt", "a+");
	}
	fprintf(f, buf);
	fprintf(f, "\n");
	fclose(f);
#endif
}

inline void LogToFile(const std::string& message)
{
	LogToFile(message.data());
}

static DebugCallback debugLogCallback = nullptr;
static DebugCallback debugWarningCallback = nullptr;
static DebugCallback debugErrorCallback = nullptr;
static SendBluetoothMessageCallback sendMessageCallback = nullptr;

struct BLEDeviceInfo
{
	GUID containerId;
	std::string deviceName;
};

struct BLEServiceInfo
{
	GUID containerId; // Used to match service and devices...
	BTH_LE_UUID id;
	std::string name;
	std::string path;
	BLEDeviceInfo* device;
};

struct BLEConnectedServiceInfo
{
	HANDLE deviceHandle;
	BTH_LE_GATT_SERVICE gattService;
	BLEServiceInfo* service;
	std::vector<BTH_LE_GATT_CHARACTERISTIC> characteristics;
};

struct BLERegisteredCharacteristicInfo
{
	BLEConnectedServiceInfo* service;
	BTH_LE_GATT_CHARACTERISTIC characteristic;
	BLUETOOTH_GATT_EVENT_HANDLE characteristicHandle;
};

std::vector<BLEDeviceInfo*> devices;
std::vector<BLEServiceInfo*> services;
std::vector<BLEConnectedServiceInfo*> connectedServices;
std::vector<BLERegisteredCharacteristicInfo*> registeredCharacteristics;

enum class QueuedMessageType
{
	Message = 0,
	Log,
	Warning,
	Error,
};

struct QueuedMessage
{
	QueuedMessageType messageType;
	std::string message;
};

std::vector<QueuedMessage> messages;
std::mutex messageMutex;           // mutex for critical section

// --------------------------------------------------------------------------
// Called by mono side to hook up message handlers!
// --------------------------------------------------------------------------
void _winBluetoothLEConnectCallbacks(SendBluetoothMessageCallback sendMessageMethod, DebugCallback logMethod, DebugCallback warningMethod, DebugCallback errorMethod)
{
	sendMessageCallback = sendMessageMethod;
	debugLogCallback = logMethod;
	debugWarningCallback = warningMethod;
	debugErrorCallback = errorMethod;
	DebugLog("Hooked Debug Functions");
}

void _winBluetoothLEDisconnectCallbacks()
{
	sendMessageCallback = nullptr;
	debugLogCallback = nullptr;
	debugWarningCallback = nullptr;
	debugErrorCallback = nullptr;
}

// --------------------------------------------------------------------------
// Talks back to the mono side of things!
// --------------------------------------------------------------------------
void SendBluetoothMessage(const char* message)
{
	const std::lock_guard<std::mutex> lock{ messageMutex };
	messages.push_back({ QueuedMessageType::Message, std::string(message) });
}
inline void SendBluetoothMessage(const std::string& message)
{
	SendBluetoothMessage(message.data());
}

// --------------------------------------------------------------------------
// Sends a log to the mono side of things
// --------------------------------------------------------------------------
void DebugLog(const char* message)
{
	const std::lock_guard<std::mutex> lock{ messageMutex };
	messages.push_back({ QueuedMessageType::Log, std::string(message) });
}
inline void DebugLog(const std::string& message)
{
	DebugLog(message.data());
}

// --------------------------------------------------------------------------
// Sends a log to the mono side of things
// --------------------------------------------------------------------------
void DebugWarning(const char* message)
{
	const std::lock_guard<std::mutex> lock{ messageMutex };
	messages.push_back({ QueuedMessageType::Warning, std::string(message) });
}
inline void DebugWarning(const std::string& message)
{
	DebugWarning(message.data());
}

// --------------------------------------------------------------------------
// Sends a log to the mono side of things
// --------------------------------------------------------------------------
void DebugError(const char* message)
{
	const std::lock_guard<std::mutex> lock{ messageMutex };
	messages.push_back({ QueuedMessageType::Error, std::string(message) });
}
inline void DebugError(const std::string& message)
{
	DebugError(message.data());
}

// --------------------------------------------------------------------------
// Sends a bluetooth error message
// --------------------------------------------------------------------------
void SendError(const char* message)
{
	std::string errorMessage = "Error~";
	errorMessage.append(message);
	SendBluetoothMessage(errorMessage);
	DebugError(message);
}
inline void SendError(const std::string& message)
{
	SendError(message.data());
}

// --------------------------------------------------------------------------
// Sends a BLT out of memory error message
// --------------------------------------------------------------------------
void SendOutOfMemoryError(int size)
{
	SendError(std::string("Failed to allocate ").append(std::to_string(size)).append(" bytes of memory."));
}

// --------------------------------------------------------------------------
// Reads a device Property, used to retrieve device name, address, etc...
// --------------------------------------------------------------------------
std::string ReadProperty(HDEVINFO hDevInfo, PSP_DEVINFO_DATA pDeviceInfoData, DWORD property)
{
	DWORD regDataType;
	LPTSTR buffer = nullptr;
	DWORD buffersSize = 0;
	while (!SetupDiGetDeviceRegistryProperty(hDevInfo, pDeviceInfoData, property, &regDataType, (PBYTE)buffer, buffersSize, &buffersSize))
	{
		if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
		{
			// Change the buffer size.
			delete[] buffer;
			// Double the size to avoid problems on
			// W2k MBCS systems per KB 888609.
			buffer = new wchar_t[buffersSize * 2];
		}
		else
		{
			wchar_t buf[256];
			FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, 256, NULL);
			SendError(std::string("Could not read device property: ").append(BLEUtils::ToNarrow(buf)));
			break;
		}
	}

	std::string prop = "";
	if (buffer != nullptr)
	{
		prop = BLEUtils::ToNarrow(buffer);
		delete[] buffer;
	}
	return prop;
}

// --------------------------------------------------------------------------
// Reads a device's instance Id, used to generate the device path and later open handle to it
// --------------------------------------------------------------------------
std::string ReadDeviceInstanceId(HDEVINFO hDevInfo, PSP_DEVINFO_DATA pDeviceInfoData)
{
	LPTSTR deviceIdBuffer = nullptr;
	DWORD deviceIdBufferSize = 0;

	while (!SetupDiGetDeviceInstanceId(hDevInfo, pDeviceInfoData, deviceIdBuffer, deviceIdBufferSize, &deviceIdBufferSize))
	{
		if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
		{
			delete[] deviceIdBuffer;
			deviceIdBuffer = new wchar_t[deviceIdBufferSize * 2];
		}
		else
		{
			wchar_t buf[256];
			FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, 256, NULL);
			SendError(std::string("Could not read device instance Id: ").append(BLEUtils::ToNarrow(buf)));
			break;
		}
	}
	std::string id = "<no_id>";
	if (deviceIdBuffer != nullptr)
	{
		id = BLEUtils::ToNarrow(deviceIdBuffer);
		delete[] deviceIdBuffer;
	}
	return id;
}

// --------------------------------------------------------------------------
// Reads a device's interface details, we use this to get service GUIDs
// --------------------------------------------------------------------------
std::string ReadDeviceInterfaceDetails(HDEVINFO hDevInfo, PSP_DEVINFO_DATA pDeviceInfoData, PSP_DEVICE_INTERFACE_DATA pDeviceInterfaceData)
{
	PSP_DEVICE_INTERFACE_DETAIL_DATA pInterfaceDetailData = NULL;
	DWORD size = 0;
	while (!SetupDiGetDeviceInterfaceDetail(hDevInfo, pDeviceInterfaceData, NULL, size, &size, pDeviceInfoData))
	{
		if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
		{
			free(pInterfaceDetailData);
			pInterfaceDetailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(size);
			if (pInterfaceDetailData != nullptr)
			{
				RtlZeroMemory(pInterfaceDetailData, size);
				pInterfaceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
			}
			else
			{
				SendOutOfMemoryError(size);
				break;
			}
		}
		else
		{
			free(pInterfaceDetailData);
			pInterfaceDetailData = nullptr;

			wchar_t buf[256];
			FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, 256, NULL);
			SendError(std::string("Could not read device interface details: ").append(BLEUtils::ToNarrow(buf)));
			break;
		}
	}

	std::string ret = "<no path>";
	if (pInterfaceDetailData != nullptr)
	{
		ret = BLEUtils::ToNarrow(pInterfaceDetailData->DevicePath);
		free(pInterfaceDetailData);
	}

	return ret;
}

// --------------------------------------------------------------------------
// Finds all the bluetooth devices
// --------------------------------------------------------------------------
bool ScanBLEInterfaces()
{
	HDEVINFO hDevInfo;
	SP_DEVINFO_DATA DeviceInfoData;
	DWORD i;
	// Create a HDEVINFO with all present devices.
	hDevInfo = SetupDiGetClassDevs(&GUID_DEVCLASS_BLUETOOTH, 0, 0, DIGCF_PRESENT);

	if (hDevInfo == INVALID_HANDLE_VALUE)
	{
		wchar_t buf[256];
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, 256, NULL);
		SendError(std::string("Could not request bluetooth device list: ").append(BLEUtils::ToNarrow(buf)));
		return false;
	}

	// Enumerate through all devices in Set.
	DeviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
	for (i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &DeviceInfoData); i++)
	{
		// Check the hardware Id
		std::string hardwareId = ReadProperty(hDevInfo, &DeviceInfoData, SPDRP_HARDWAREID);

		// We're only interested in entries that start with either 'BTHLE\' or 'BTHLEDEVICE\'
		bool isDevice = hardwareId.find("BTHLE\\") == 0;
		bool isService = hardwareId.find("BTHLEDevice\\") == 0;
		if (!isDevice && !isService)
		{
			continue;
		}

		// Then grab the container GUID, this is what we use to match devices and services to the same physical device
		GUID containerGUID = BLEUtils::StringToGUID(ReadProperty(hDevInfo, &DeviceInfoData, SPDRP_BASE_CONTAINERID));

		if (isDevice)
		{
			// Only add new devices
			auto prev = std::find_if(devices.begin(), devices.end(), [&containerGUID](const BLEDeviceInfo* x) { return x->containerId == containerGUID; });
			if (prev == devices.end())
			{
				// Fetch the name!
				auto info = new BLEDeviceInfo();
				info->deviceName = ReadProperty(hDevInfo, &DeviceInfoData, SPDRP_FRIENDLYNAME);
				info->containerId = containerGUID;
				devices.push_back(info);
			}
		}

		if (isService)
		{
			// Fetch the GUID!
			std::regex guidRegex("\\{.*\\}");
			std::smatch match;
			if (std::regex_search(hardwareId, match, guidRegex))
			{
				std::string guidString = match.str();
				auto serviceId = BLEUtils::StringToBTHLEUUID(guidString);

				auto prev = std::find_if(services.begin(), services.end(),
					[&containerGUID, &serviceId](const BLEServiceInfo* x)
					{
						return x->containerId == containerGUID && x->id == serviceId;
					});

				if (prev == services.end())
				{
					auto service = new BLEServiceInfo();
					service->name = ReadProperty(hDevInfo, &DeviceInfoData, SPDRP_DEVICEDESC);

					service->containerId = containerGUID;
					service->id = serviceId;

					// Parse the device instance id to get the device path!
					std::string deviceId = ReadDeviceInstanceId(hDevInfo, &DeviceInfoData);

					// Create the device path
					std::string path = "\\\\?\\";
					std::replace(deviceId.begin(), deviceId.end(), '\\', '#');
					path.append(deviceId);
					path.append("#");
					path.append(guidString);
					service->path = path;

					services.push_back(service);
				}
			}
			else
			{
				SendError(std::string("Could not extract service GUID from the hardware ID \'").append(hardwareId).append("\'"));
			}
		}
	}

	// Set device pointers
	for (auto service : services)
	{
		auto devIt = std::find_if(devices.begin(), devices.end(), [service](BLEDeviceInfo* d) { return d->containerId == service->containerId; });
		if (devIt != devices.end())
		{
			service->device = *devIt;
		}
		else
		{
			SendError(std::string("Could not find the device that service ").append(BLEUtils::BTHLEGUIDToString(service->id)).append(" belongs to"));
		}
	}

	SetupDiDestroyDeviceInfoList(hDevInfo);

	return 0;
}

// --------------------------------------------------------------------------
// Iterates over all devices and sends notifications back for each one that matches the UUIDs passed in
// --------------------------------------------------------------------------
void notifyDevicesWithServices(const std::vector<BTH_LE_UUID>& uuids)
{
	std::vector<GUID> returnedDevices;
	// Find any device that has a service whose UUID matches one of the UUIDs passed in!
	for (auto service : services)
	{
		auto prev = std::find_if(connectedServices.begin(), connectedServices.end(), [service](const BLEConnectedServiceInfo* x) { return x->service == service; });
		if (prev == connectedServices.end())
		{
			// Not already connected, good!
			if (std::find(uuids.begin(), uuids.end(), service->id) != uuids.end())
			// Does this service match the UUID?
			{
				// Yes, send a message for each discovered peripheral
				// Sadly we don't have access to advertisement data, it is managed by Windows!
				std::string deviceDiscoveredMessage = "DiscoveredPeripheral~";
				deviceDiscoveredMessage.append(BLEUtils::GUIDToString(service->device->containerId));
				deviceDiscoveredMessage.append("~");
				deviceDiscoveredMessage.append(service->device->deviceName);
				SendBluetoothMessage(deviceDiscoveredMessage);
			}
		}
	}
}

// --------------------------------------------------------------------------
// Iterates over all devices and sends notifications back for each one
// --------------------------------------------------------------------------
void notifyAllDevices()
{
	// Find any device that has a service whose UUID matches one of the UUIDs passed in!
	for (auto device : devices)
	{
		// Send a message for each discovered peripheral
		// Sadly we don't have access to advertisement data...
		std::string deviceDiscoveredMessage = "DiscoveredPeripheral~";
		deviceDiscoveredMessage.append(BLEUtils::GUIDToString(device->containerId));
		deviceDiscoveredMessage.append("~");
		deviceDiscoveredMessage.append(device->deviceName);
		SendBluetoothMessage(deviceDiscoveredMessage);
	}
}

// --------------------------------------------------------------------------
// Iterates over all connected devices and sends notifications back for each one that matches the UUIDs passed in
// --------------------------------------------------------------------------
void notifyConnectedServices(const std::vector<BTH_LE_UUID>& uuids)
{
	// Send messages for all the ones
	for (auto service : connectedServices)
	{
		// Find any service that has a service whose UUID matches one of the UUIDs passed in!
		if (std::find_if(uuids.begin(), uuids.end(), [&service](const BTH_LE_UUID& uuid) { return service->service->id == uuid; }) != uuids.end())
		{
			std::string deviceDiscoveredMessage = "RetrievedConnectedPeripheral~";
			deviceDiscoveredMessage.append(BLEUtils::GUIDToString(service->service->device->containerId));
			deviceDiscoveredMessage.append("~");
			deviceDiscoveredMessage.append(service->service->device->deviceName);
			SendBluetoothMessage(deviceDiscoveredMessage);
		}
	}
}

// --------------------------------------------------------------------------
// Iterates over all connected devices and sends notifications back for each one
// --------------------------------------------------------------------------
void notifyAllConnected()
{
	// Send messages for all the ones
	for (auto service : connectedServices)
	{
		// Send a message for each discovered service
		std::string connectedDeviceRetrievedMessage = "RetrievedConnectedPeripheral~";
		connectedDeviceRetrievedMessage.append(BLEUtils::GUIDToString(service->service->device->containerId));
		connectedDeviceRetrievedMessage.append("~");
		connectedDeviceRetrievedMessage.append(service->service->device->deviceName);
		SendBluetoothMessage(connectedDeviceRetrievedMessage);
	}
}

// --------------------------------------------------------------------------
// Retrieves the GATT service struct that matches the given service
// --------------------------------------------------------------------------
bool GetGATTService(HANDLE serviceHandle, BTH_LE_GATT_SERVICE& outService)
{
	// Get GATT service
	PBTH_LE_GATT_SERVICE services = nullptr;
	USHORT serviceCount = 0;
	HRESULT hr = S_OK;
	while ((hr = BluetoothGATTGetServices(serviceHandle, serviceCount, services, &serviceCount, BLUETOOTH_GATT_FLAG_NONE)) != S_OK)
	{
		if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA))
		{
			// Change the buffer size.
			delete[] services;
			services = new BTH_LE_GATT_SERVICE[serviceCount];
		}
		else
		{
			_com_error err(hr);
			SendError(std::string("Could not retrieve service GATT info: ").append(BLEUtils::ToNarrow(err.ErrorMessage())));
			break;
		}
	}

	bool ret = hr == S_OK && serviceCount > 0 && services != nullptr;
	if (ret)
	{
		// Only grab the first service!
		outService = services[0];
	}
	delete[] services;
	return ret;
}


// --------------------------------------------------------------------------
// Retrieves the characteristics associated with a service
// --------------------------------------------------------------------------
std::vector<BTH_LE_GATT_CHARACTERISTIC> GetGATTCharacteristics(HANDLE serviceHandle, BTH_LE_GATT_SERVICE& gattService)
{
	std::vector<BTH_LE_GATT_CHARACTERISTIC> ret;
	PBTH_LE_GATT_CHARACTERISTIC characteristicsBuffer = nullptr;
	USHORT characteristicCount = 0;
	HRESULT hr = S_OK;
	while ((hr = BluetoothGATTGetCharacteristics(serviceHandle, &gattService, characteristicCount, characteristicsBuffer, &characteristicCount, BLUETOOTH_GATT_FLAG_NONE)) != S_OK)
	{
		if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA))
		{
			// Change the buffer size.
			ret.resize(characteristicCount);
			// And have the method write directly into the vector!
			characteristicsBuffer = ret.data();
		}
		else
		{
			_com_error err(hr);
			SendError(std::string("Could not retrieve service characteristics: ").append(BLEUtils::ToNarrow(err.ErrorMessage())));
			break;
		}
	}
	return ret;
}

// --------------------------------------------------------------------------
// Retrieves a characteristic's value, allocates memory because the data has variable size!
// --------------------------------------------------------------------------
PBTH_LE_GATT_CHARACTERISTIC_VALUE AllocAndReadCharacteristic(HANDLE serviceHandle, BTH_LE_GATT_CHARACTERISTIC* currGattChar)
{
	PBTH_LE_GATT_CHARACTERISTIC_VALUE pCharValueBuffer = nullptr;
	
	if (currGattChar->IsReadable)
	{
		// Determine Characteristic Value Buffer Size
		USHORT charValueDataSize = 0;
		HRESULT hr = S_OK;
		while ((hr = BluetoothGATTGetCharacteristicValue(serviceHandle, currGattChar, (ULONG)charValueDataSize, pCharValueBuffer, &charValueDataSize, BLUETOOTH_GATT_FLAG_NONE)) != S_OK)
		{
			if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA))
			{
				free(pCharValueBuffer);
				pCharValueBuffer = (PBTH_LE_GATT_CHARACTERISTIC_VALUE)malloc(charValueDataSize);
				if (pCharValueBuffer != nullptr)
				{
					RtlZeroMemory(pCharValueBuffer, charValueDataSize);
					pCharValueBuffer->DataSize = charValueDataSize;
				}
				else
				{
					SendOutOfMemoryError(charValueDataSize);
					break;
				}
			}
			else
			{
				free(pCharValueBuffer);
				pCharValueBuffer = nullptr;

				_com_error err(hr);
				SendError(std::string("Could not get characteristic ").append(BLEUtils::BTHLEGUIDToString(currGattChar->CharacteristicUuid)).append(" value: ").append(BLEUtils::ToNarrow(err.ErrorMessage())));
			}
		}
	}
	else
	{
		SendError(std::string("Characteristic ").append(BLEUtils::BTHLEGUIDToString(currGattChar->CharacteristicUuid)).append(" is not readable."));
	}

	return pCharValueBuffer;
}

// --------------------------------------------------------------------------
// Disconnects ALL connected services associated with a device!
// --------------------------------------------------------------------------
bool DisconnectServicesForDevice(GUID addressGUID)
{
	bool disconnectedService = false;
	for (auto servIt = connectedServices.begin(); servIt != connectedServices.end();)
	{
		auto cservice = *servIt;
		if (cservice->service->device->containerId == addressGUID)
		{
			// Do we have any registered characteristics?
			for (auto charIt = registeredCharacteristics.begin(); charIt != registeredCharacteristics.end();)
			{
				auto charInfo = *charIt;
				if (charInfo->service->service->device->containerId == addressGUID)
				{
					// We should unregister!
					HRESULT hr = BluetoothGATTUnregisterEvent(charInfo->characteristicHandle, BLUETOOTH_GATT_FLAG_NONE);
					if (hr == S_OK)
					{
						// Send message
						std::string registerCharacteristicMessage = "DidUpdateNotificationStateForCharacteristic~";
						registerCharacteristicMessage.append(BLEUtils::GUIDToString(addressGUID));
						registerCharacteristicMessage.append("~");
						registerCharacteristicMessage.append(BLEUtils::BTHLEGUIDToString(cservice->service->id));
						registerCharacteristicMessage.append("~");
						registerCharacteristicMessage.append(BLEUtils::BTHLEGUIDToString(charInfo->characteristic.CharacteristicUuid));
						SendBluetoothMessage(registerCharacteristicMessage);

						// Clean up
						delete charInfo;
						charIt = registeredCharacteristics.erase(charIt);
					}
					else
					{
						_com_error err(hr);
						SendError(std::string("Could not unregister from characteristic ").append(BLEUtils::GUIDToString(addressGUID)).append(" ").append(BLEUtils::ToNarrow(err.ErrorMessage())));
						// Next element!
						++charIt;
					}
				}
				else
				{
					// Next element!
					++charIt;
				}
			}

			if (CloseHandle(cservice->deviceHandle))
			{
				servIt = connectedServices.erase(servIt);
				delete cservice;
				disconnectedService = true;
			}
			else
			{
				SendError(std::string("Could not close handle to device ").append(BLEUtils::GUIDToString(addressGUID)));

			}
		}
		else
		{
			++servIt;
		}
	}

	return disconnectedService;
}

// --------------------------------------------------------------------------
// Retrieves the descriptors associated with a characteristic
// --------------------------------------------------------------------------
std::vector<BTH_LE_GATT_DESCRIPTOR> GetGATTDescriptors(HANDLE serviceHandle, PBTH_LE_GATT_CHARACTERISTIC characteristic)
{
	std::vector<BTH_LE_GATT_DESCRIPTOR> ret;
	PBTH_LE_GATT_DESCRIPTOR descriptorsBuffer = nullptr;
	USHORT descriptorsCount = 0;
	HRESULT hr = S_OK;
	while ((hr = BluetoothGATTGetDescriptors(serviceHandle, characteristic, descriptorsCount, descriptorsBuffer, &descriptorsCount, BLUETOOTH_GATT_FLAG_NONE)) != S_OK)
	{
		if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA))
		{
			// Change the buffer size.
			ret.resize(descriptorsCount);
			// And have the method write directly into the vector!
			descriptorsBuffer = ret.data();
		}
		else
		{
			_com_error err(hr);
			SendError(std::string("Could not retrieve characteristic descriptors: ").append(BLEUtils::ToNarrow(err.ErrorMessage())));
			break;
		}
	}
	return ret;
}

// --------------------------------------------------------------------------
// Retrieves a descriptor's value, allocates memory because the data has variable size!
// --------------------------------------------------------------------------
PBTH_LE_GATT_DESCRIPTOR_VALUE AllocAndReadDescriptor(HANDLE serviceHandle, PBTH_LE_GATT_DESCRIPTOR descriptor)
{
	// Determine Characteristic Value Buffer Size
	USHORT descValueDataSize = 0;
	PBTH_LE_GATT_DESCRIPTOR_VALUE pDescValueBuffer = nullptr;
	HRESULT hr = S_OK;
	while ((hr = BluetoothGATTGetDescriptorValue(serviceHandle, descriptor, (ULONG)descValueDataSize, pDescValueBuffer, &descValueDataSize, BLUETOOTH_GATT_FLAG_NONE)) != S_OK)
	{
		if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA))
		{
			free(pDescValueBuffer);
			pDescValueBuffer = (PBTH_LE_GATT_DESCRIPTOR_VALUE)malloc(descValueDataSize);
			if (pDescValueBuffer != nullptr)
			{
				RtlZeroMemory(pDescValueBuffer, descValueDataSize);
				pDescValueBuffer->DataSize = descValueDataSize;
			}
			else
			{
				SendOutOfMemoryError(descValueDataSize);
				break;
			}
		}
		else
		{
			free(pDescValueBuffer);
			pDescValueBuffer = nullptr;

			_com_error err(hr);
			SendError(std::string("Could not get descriptor value ").append(BLEUtils::BTHLEGUIDToString(descriptor->DescriptorUuid)).append(" value: ").append(BLEUtils::ToNarrow(err.ErrorMessage())));
			break;
		}
	}

	return pDescValueBuffer;
}

// --------------------------------------------------------------------------
// Logs some info from the mono side
// --------------------------------------------------------------------------
void _winBluetoothLELog(const char* message)
{
	DebugLog(message);
}

// --------------------------------------------------------------------------
// Initialize the bluetooth 'stack'
// --------------------------------------------------------------------------
void _winBluetoothLEInitialize(bool asCentral, bool asPeripheral)
{
	SendBluetoothMessage("Initialized");
}

// --------------------------------------------------------------------------
// Clean up!
// --------------------------------------------------------------------------
void _winBluetoothLEDeInitialize()
{
	_winBluetoothLEDisconnectAll();
	LogToFile("DeInitialized");
	if (sendMessageCallback != NULL)
	{
		sendMessageCallback("BluetoothLEReceiver", "OnBluetoothMessage", "DeInitialized");
	}

	devices.clear();
	services.clear();
	connectedServices.clear();
	registeredCharacteristics.clear();

	const std::lock_guard<std::mutex> lock{ messageMutex };
	messages.clear();
}

// --------------------------------------------------------------------------
// Pause sending messages back to the mono side
// --------------------------------------------------------------------------
void _winBluetoothLEPauseMessages(bool isPaused)
{
	
}

// --------------------------------------------------------------------------
// Scans all the bluetooth devices and notifies the mono side
// --------------------------------------------------------------------------
void _winBluetoothLEScanForPeripheralsWithServices(const char* serviceUUIDsString, bool allowDuplicates, bool rssiOnly, bool clearPeripheralList)
{
	// Devices are managed by windows, so we don't need to 'remember' old devices
	//devices.clear();
	//services.clear();
	//connectedServices.clear();
	//registeredCharacteristics.clear();

	// Scan for devices
	ScanBLEInterfaces();

	// Retrieve the devices with proper service UUID
	if (serviceUUIDsString != nullptr)
	{
		auto uuids = BLEUtils::GenerateGUIDList(serviceUUIDsString);
		notifyDevicesWithServices(uuids);
	}
	else
	{
		notifyAllDevices();
	}
}

// --------------------------------------------------------------------------
// Lists all the currently connected devices
// --------------------------------------------------------------------------
void _winBluetoothLERetrieveListOfPeripheralsWithServices(const char* serviceUUIDsString)
{
	if (serviceUUIDsString != nullptr)
	{
		auto uuids = BLEUtils::GenerateGUIDList(serviceUUIDsString);
		notifyConnectedServices(uuids);
	}
	else
	{
		notifyAllConnected();
	}
}

// --------------------------------------------------------------------------
// Stops scanning for bluetooth devices
// --------------------------------------------------------------------------
void _winBluetoothLEStopScan()
{
	// Nothing to do for now, scanning is handled by windows
}

// --------------------------------------------------------------------------
// Connects to a given device and list services/characteristics
// --------------------------------------------------------------------------
void _winBluetoothLEConnectToPeripheral(const char* address)
{
	if (address != nullptr)
	{
		// Iterate all the services for the given device
		bool firstService = true;
		GUID addressGUID = BLEUtils::StringToGUID(address);
		for (auto servIt = services.begin(); servIt != services.end(); ++servIt)
		{
			auto service = *servIt;
			if (service->containerId == addressGUID)
			{
				// Open a handle to the peripheral, and scan the services and characteristics
				HANDLE serviceHandle = CreateFile(BLEUtils::ToWide(service->path.data()).data(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
				if (serviceHandle != INVALID_HANDLE_VALUE)
				{
					// Remember we connected to the device, so we can clean up later!
					auto connInfo = new BLEConnectedServiceInfo();
					connInfo->service = service;
					connInfo->deviceHandle = serviceHandle;
					connectedServices.push_back(connInfo);

					// Notify that we connected to a service!
					if (firstService)
					{
						firstService = false;
						std::string connectedMessage = "ConnectedPeripheral~";
						connectedMessage.append(address);
						SendBluetoothMessage(connectedMessage);
					}

					// Get GATT service ids and characteristics
					if (GetGATTService(serviceHandle, connInfo->gattService))
					{
						// Check that the GATT service ID matches the service ID
						auto gattServiceUuidString = BLEUtils::BTHLEGUIDToString(connInfo->gattService.ServiceUuid);
						if (connInfo->gattService.ServiceUuid == service->id)
						{
							// Notify that we indeed got the GATT service info!
							std::string discoveredServiceMessage = "DiscoveredService~";
							discoveredServiceMessage.append(address);
							discoveredServiceMessage.append("~");
							discoveredServiceMessage.append(gattServiceUuidString);
							SendBluetoothMessage(discoveredServiceMessage);

							// Scan characteristics now!
							connInfo->characteristics = GetGATTCharacteristics(serviceHandle, connInfo->gattService);
							if (connInfo->characteristics.size() > 0)
							{
								for (auto& characteristic : connInfo->characteristics)
								{
									auto gattCharacteristicUuidString = BLEUtils::BTHLEGUIDToString(characteristic.CharacteristicUuid);

									// Notify that we got characteristic info
									std::string discoveredCharacteristicMessage = "DiscoveredCharacteristic~";
									discoveredCharacteristicMessage.append(address);
									discoveredCharacteristicMessage.append("~");
									discoveredCharacteristicMessage.append(gattServiceUuidString);
									discoveredCharacteristicMessage.append("~");
									discoveredCharacteristicMessage.append(gattCharacteristicUuidString);
									SendBluetoothMessage(discoveredCharacteristicMessage);
								}
							}
							else
							{
								SendError(std::string("Device ").append(address).append(" reported 0 characteristics."));
							}
						}
						else
						{
							SendError(std::string("GATT service id ").append(gattServiceUuidString).append(" does not match service id ").append(BLEUtils::BTHLEGUIDToString(service->id)));
						}
					}

					// No matter what we're done looking through the services
					return;
				}
			}
		}

		if (firstService)
		{
			SendError(std::string("Did not find any service for device ").append(address));
		}
	}
	else
	{
		SendError(std::string("Can't connect to Null device address"));
	}
}

// --------------------------------------------------------------------------
// Disconnects from a given device
// --------------------------------------------------------------------------
void _winBluetoothLEDisconnectPeripheral(const char* address)
{
	if (address != nullptr)
	{
		// Disconnect all services associated with this device
		GUID addressGUID = BLEUtils::StringToGUID(address);
		if (DisconnectServicesForDevice(addressGUID))
		{
			// Notify that we disconnected to a service!
			std::string connectedMessage = "DisconnectedPeripheral~";
			connectedMessage.append(BLEUtils::GUIDToString(addressGUID));
			SendBluetoothMessage(connectedMessage);
		}
	}
	else
	{
		SendError(std::string("Can't connect to Null device address"));
	}
}

// --------------------------------------------------------------------------
// Reads a characteristic from a device/service
// --------------------------------------------------------------------------
void _winBluetoothLEReadCharacteristic(const char* address, const char* service, const char* characteristic)
{
	if (address == nullptr)
	{
		SendError("Null address");
		return;
	}

	if (service == nullptr)
	{
		SendError("Null service");
		return;
	}

	if (characteristic == nullptr)
	{
		SendError("Null characteristic");
		return;
	}

	// Find connected service handle
	GUID addressGUID = BLEUtils::StringToGUID(address);
	BTH_LE_UUID serviceGUID = BLEUtils::StringToBTHLEUUID(service);
	auto servIt = std::find_if(connectedServices.begin(), connectedServices.end(), [addressGUID, serviceGUID](BLEConnectedServiceInfo* s) { return s->service->device->containerId == addressGUID && s->service->id == serviceGUID; });
	if (servIt != connectedServices.end())
	{
		// Find characteristic!
		BTH_LE_UUID characteristicGUID = BLEUtils::StringToBTHLEUUID(characteristic);
		auto cservice = *servIt;
		auto charIt = std::find_if(cservice->characteristics.begin(), cservice->characteristics.end(), [characteristicGUID](const BTH_LE_GATT_CHARACTERISTIC& c) { return c.CharacteristicUuid == characteristicGUID; });
		if (charIt != cservice->characteristics.end())
		{
			auto charVal = AllocAndReadCharacteristic(cservice->deviceHandle, &(*charIt));
			if (charVal != nullptr)
			{
				// Notify that we got characteristic info
				std::string readCharacteristicMessage = "DidUpdateValueForCharacteristic~";
				readCharacteristicMessage.append(address);
				readCharacteristicMessage.append("~");
				readCharacteristicMessage.append(characteristic);
				readCharacteristicMessage.append("~");
				readCharacteristicMessage.append(BLEUtils::Base64Encode(charVal->Data,charVal->DataSize));
				SendBluetoothMessage(readCharacteristicMessage);

				// Clean up!
				free(charVal);
			}
		}
		else
		{
			SendError(std::string("Could not find characteristic ").append(characteristic).append(" to read."));
		}
	}
	else
	{
		SendError(std::string("Could not find device ").append(address).append(" to read from."));
	}
}

// --------------------------------------------------------------------------
// Writes a characteristic to a device/service
// --------------------------------------------------------------------------
void _winBluetoothLEWriteCharacteristic(const char* address, const char* service, const char* characteristic, const unsigned char* data, int length, bool withResponse)
{
	if (address == nullptr)
	{
		SendError("Null address");
		return;
	}

	if (service == nullptr)
	{
		SendError("Null service");
		return;
	}

	if (characteristic == nullptr)
	{
		SendError("Null characteristic");
		return;
	}

	if (data == nullptr)
	{
		SendError("Null data");
		return;
	}

	// Find connected service handle
	GUID addressGUID = BLEUtils::StringToGUID(address);
	BTH_LE_UUID serviceGUID = BLEUtils::StringToBTHLEUUID(service);
	auto servIt = std::find_if(connectedServices.begin(), connectedServices.end(), [addressGUID, serviceGUID](BLEConnectedServiceInfo* s) { return s->service->device->containerId == addressGUID && s->service->id == serviceGUID; });
	if (servIt != connectedServices.end())
	{
		// Find characteristic!
		BTH_LE_UUID characteristicGUID = BLEUtils::StringToBTHLEUUID(characteristic);
		auto cservice = *servIt;

		// Find characteristic!
		auto charIt = std::find_if(cservice->characteristics.begin(), cservice->characteristics.end(), [characteristicGUID](const BTH_LE_GATT_CHARACTERISTIC& c) { return c.CharacteristicUuid == characteristicGUID; });
		if (charIt != cservice->characteristics.end())
		{
			ULONG charValueSize = length + sizeof(ULONG);
			PBTH_LE_GATT_CHARACTERISTIC_VALUE newCharVal = (PBTH_LE_GATT_CHARACTERISTIC_VALUE)malloc(charValueSize);
			if (newCharVal != nullptr)
			{
				RtlZeroMemory(newCharVal, charValueSize);
				newCharVal->DataSize = length;
				memcpy(newCharVal->Data, data, length);

				ULONG flags = BLUETOOTH_GATT_FLAG_NONE;
				if (withResponse)
					BLUETOOTH_GATT_FLAG_WRITE_WITHOUT_RESPONSE;
				HRESULT hr = BluetoothGATTSetCharacteristicValue(cservice->deviceHandle, &(*charIt), newCharVal, NULL, flags);
				if (hr != S_OK)
				{
					_com_error err(hr);
					SendError(std::string("Could not fetch characteristic value for ").append(characteristic).append(" ").append(BLEUtils::ToNarrow(err.ErrorMessage())));
				}

				// Clean up!
				free(newCharVal);
			}
			else
			{
				SendOutOfMemoryError(charValueSize);
			}
		}
		else
		{
			SendError(std::string("Could not find characteristic ").append(characteristic).append(" to write."));
		}
	}
	else
	{
		SendError(std::string("Could not find device ").append(address).append(" to write to."));
	}
}

// --------------------------------------------------------------------------
// Called when a characteristic value changes!
// --------------------------------------------------------------------------
void CALLBACK HandleBLENotification(BTH_LE_GATT_EVENT_TYPE EventType, PVOID EventOutParameter, PVOID Context)
{
	PBLUETOOTH_GATT_VALUE_CHANGED_EVENT ValueChangedEventParameters = (PBLUETOOTH_GATT_VALUE_CHANGED_EVENT)EventOutParameter;


	//// Notify that we got characteristic info
	//std::string wroteCharacteristicMessage = "DidWriteCharacteristic~";
	//wroteCharacteristicMessage.append(characteristic);
	//SendBluetoothMessage(wroteCharacteristicMessage);


	// Find the characteristic
	auto charInfo = (BLERegisteredCharacteristicInfo*)Context;
	auto charIt = std::find(registeredCharacteristics.begin(), registeredCharacteristics.end(), charInfo);
	if (charIt != registeredCharacteristics.end())
	{
		// Notify that we got characteristic info
		std::string readCharacteristicMessage = "DidUpdateValueForCharacteristic~";
		readCharacteristicMessage.append(BLEUtils::GUIDToString(charInfo->service->service->device->containerId));
		readCharacteristicMessage.append("~");
		readCharacteristicMessage.append(BLEUtils::BTHLEGUIDToString(charInfo->characteristic.CharacteristicUuid));
		readCharacteristicMessage.append("~");
		readCharacteristicMessage.append(BLEUtils::Base64Encode(ValueChangedEventParameters->CharacteristicValue->Data, (unsigned int)ValueChangedEventParameters->CharacteristicValueDataSize));
		SendBluetoothMessage(readCharacteristicMessage);
	}
	else
	{
		SendError(std::string("Received BLE notification for characteristic ").append(BLEUtils::BTHLEGUIDToString(charInfo->characteristic.CharacteristicUuid)).append(" which we did not register with."));
	}
}


// --------------------------------------------------------------------------
// Subscribe to a characteristic changing values!
// --------------------------------------------------------------------------
void _winBluetoothLESubscribeCharacteristic(const char* address, const char* service, const char* characteristic)
{
	if (address == nullptr)
	{
		SendError("Null address");
		return;
	}

	if (service == nullptr)
	{
		SendError("Null service");
		return;
	}

	if (characteristic == nullptr)
	{
		SendError("Null characteristic");
		return;
	}

	// Find connected service handle
	GUID addressGUID = BLEUtils::StringToGUID(address);
	BTH_LE_UUID serviceGUID = BLEUtils::StringToBTHLEUUID(service);
	auto servIt = std::find_if(connectedServices.begin(), connectedServices.end(), [addressGUID, serviceGUID](BLEConnectedServiceInfo* s) { return s->service->device->containerId == addressGUID && s->service->id == serviceGUID; });
	if (servIt != connectedServices.end())
	{
		// Find characteristic!
		BTH_LE_UUID characteristicGUID = BLEUtils::StringToBTHLEUUID(characteristic);
		auto cservice = *servIt;
		auto charIt = std::find_if(cservice->characteristics.begin(), cservice->characteristics.end(), [characteristicGUID](const BTH_LE_GATT_CHARACTERISTIC& c) { return c.CharacteristicUuid == characteristicGUID; });
		if (charIt != cservice->characteristics.end())
		{
			if (charIt->IsNotifiable)
			{
				// Set up the Client Characteristic Configuration Descriptor, so that we are 'allowed' to receive notifications!

				// Retrieve all descriptors for this characteristic
				auto descs = GetGATTDescriptors(cservice->deviceHandle, &(*charIt));

				// And find the client one
				auto descIt = std::find_if(descs.begin(), descs.end(), [](const BTH_LE_GATT_DESCRIPTOR& d) { return d.DescriptorType == ClientCharacteristicConfiguration; });
				if (descIt != descs.end())
				{
					// Got it, write to it now to indicate we want to be notified!
					BTH_LE_GATT_DESCRIPTOR_VALUE newValue;
					RtlZeroMemory(&newValue, sizeof(newValue));
					newValue.DescriptorType = ClientCharacteristicConfiguration;
					newValue.ClientCharacteristicConfiguration.IsSubscribeToNotification = TRUE;

					// Subscribe to an event.
					HRESULT hr = BluetoothGATTSetDescriptorValue(cservice->deviceHandle, &(*descIt), &newValue, BLUETOOTH_GATT_FLAG_NONE);
					if (hr == S_OK)
					{
						// set the appropriate callback function when the descriptor change value
						auto charInfo = new BLERegisteredCharacteristicInfo();
						charInfo->service = cservice;
						charInfo->characteristic = *charIt;

						BLUETOOTH_GATT_VALUE_CHANGED_EVENT_REGISTRATION EventParameterIn;
						EventParameterIn.Characteristics[0] = *charIt;
						EventParameterIn.NumCharacteristics = 1;

						hr = BluetoothGATTRegisterEvent( 
							cservice->deviceHandle,
							CharacteristicValueChangedEvent,
							(PVOID)&EventParameterIn,
							(PFNBLUETOOTH_GATT_EVENT_CALLBACK)HandleBLENotification,
							charInfo,
							&charInfo->characteristicHandle,
							BLUETOOTH_GATT_FLAG_NONE);
						if (hr == S_OK)
						{
							// Remember we registered with the characteristic
							registeredCharacteristics.push_back(charInfo);

							// Send message
							std::string registerCharacteristicMessage = "DidUpdateNotificationStateForCharacteristic~";
							registerCharacteristicMessage.append(address);
							registerCharacteristicMessage.append("~");
							registerCharacteristicMessage.append(characteristic);
							SendBluetoothMessage(registerCharacteristicMessage);
						}
						else
						{
							delete charInfo;
							_com_error err(hr);
							SendError(std::string("Could not register with characteristic ").append(address).append(" ").append(BLEUtils::ToNarrow(err.ErrorMessage())));
						}
					}
					else
					{
						_com_error err(hr);
						SendError(std::string("Could not set Client Config descriptor value for characteristic ").append(address).append(" ").append(BLEUtils::ToNarrow(err.ErrorMessage())));
					}
				}
				else
				{
					SendError(std::string("Could not find Client Config descriptor for characteristic ").append(characteristic));
				}
			}
			else
			{
				SendError(std::string("Characteristic ").append(characteristic).append(" is not Notifiable."));
			}
		}
		else
		{
			SendError(std::string("Could not find characteristic ").append(characteristic).append(" to subscribe to."));
		}
	}
	else
	{
		SendError(std::string("Could not find device ").append(address).append(" to subscribe to."));
	}
}

// --------------------------------------------------------------------------
// Unsubscribe! ;)
// --------------------------------------------------------------------------
void _winBluetoothLEUnSubscribeCharacteristic(const char* address, const char* service, const char* characteristic)
{
	if (address == nullptr)
	{
		SendError("Null address");
		return;
	}

	if (service == nullptr)
	{
		SendError("Null service");
		return;
	}

	if (characteristic == nullptr)
	{
		SendError("Null characteristic");
		return;
	}

	// Find registered characteristic!
	GUID addressGUID = BLEUtils::StringToGUID(address);
	BTH_LE_UUID serviceGUID = BLEUtils::StringToBTHLEUUID(service);
	BTH_LE_UUID characteristicGUID = BLEUtils::StringToBTHLEUUID(characteristic);
	auto charIt = std::find_if(
		registeredCharacteristics.begin(),
		registeredCharacteristics.end(),
		[addressGUID, serviceGUID, characteristicGUID] (BLERegisteredCharacteristicInfo* c)
		{
		// Check device!
		return	c->service->service->device->containerId == addressGUID &&
				c->service->service->id == serviceGUID &&
				c->characteristic.CharacteristicUuid == characteristicGUID;
		});

	if (charIt != registeredCharacteristics.end())
	{
		// Unregister
		auto charInfo = *charIt;
		HRESULT hr = BluetoothGATTUnregisterEvent(charInfo->characteristicHandle, BLUETOOTH_GATT_FLAG_NONE);
		if (hr == S_OK)
		{
			// Clean up
			delete charInfo;
			registeredCharacteristics.erase(charIt);

			// Send message
			std::string registerCharacteristicMessage = "DidUpdateNotificationStateForCharacteristic~";
			registerCharacteristicMessage.append(address);
			registerCharacteristicMessage.append("~");
			registerCharacteristicMessage.append(characteristic);
			SendBluetoothMessage(registerCharacteristicMessage);
		}
		else
		{
			_com_error err(hr);
			SendError(std::string("Could not unregister from characteristic event").append(address).append(" ").append(BLEUtils::ToNarrow(err.ErrorMessage())));
		}
	}
	else
	{
		SendError(std::string("Could not find notification registration data for characteristic ").append(characteristic));
	}
}

// --------------------------------------------------------------------------
// Clean up
// --------------------------------------------------------------------------
void _winBluetoothLEDisconnectAll()
{
	// Disconnect from devices if needed!
	for (auto device : devices)
	{
		if (DisconnectServicesForDevice(device->containerId))
		{
			// Notify that we disconnected to a service!
			std::string connectedMessage = "DisconnectedPeripheral~";
			connectedMessage.append(BLEUtils::GUIDToString(device->containerId));
			SendBluetoothMessage(connectedMessage);
		}
	}
	devices.clear();
}


void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
	LogToFile("Plugin Load");
}

// Unity plugin unload event
void UNITY_INTERFACE_API UnityPluginUnload()
{
	LogToFile("Plugin Unload");
	_winBluetoothLEDeInitialize();
}

BOOL WINAPI DllMain(_In_ HINSTANCE hinstDLL, _In_ DWORD fdwReason, _In_ LPVOID lpvReserved)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		LogToFile("DLL_PROCESS_ATTACH");
		break;
	case DLL_PROCESS_DETACH:
		LogToFile("DLL_PROCESS_DETACH");
		_winBluetoothLEDeInitialize();
		break;
	case DLL_THREAD_ATTACH:
		LogToFile("DLL_THREAD_ATTACH");
		break;
	case DLL_THREAD_DETACH:
		LogToFile("DLL_THREAD_DETACH");
		break;
	default:
		char buf[256];
		sprintf_s(buf, 256, "Other: %d", fdwReason);
		LogToFile(buf);
		break;
	}
	return TRUE;
}

void _winBluetoothLEUpdate()
{
	// Copy messages
	messageMutex.lock();
	std::vector<QueuedMessage> msgCopy{ std::move(messages) };
	messageMutex.unlock();

	for (auto& msg : msgCopy) {
		switch (msg.messageType)
		{
		case QueuedMessageType::Message:
			LogToFile("Message> " + msg.message);
			if (sendMessageCallback != nullptr)
			{
				sendMessageCallback("BluetoothLEReceiver", "OnBluetoothMessage", msg.message.data());
			}
			break;
		case QueuedMessageType::Log:
			LogToFile("Log> " + msg.message);
			if (debugLogCallback != nullptr)
			{
				debugLogCallback(msg.message.data());
			}
			break;
		case QueuedMessageType::Warning:
			LogToFile("Warning> " + msg.message);
			if (debugWarningCallback != nullptr)
			{
				debugWarningCallback(msg.message.data());
			}
			break;
		case QueuedMessageType::Error:
			LogToFile("Error> " + msg.message);
			if (debugErrorCallback != nullptr)
			{
				debugErrorCallback(msg.message.data());
			}
			break;
		}
	}
}
