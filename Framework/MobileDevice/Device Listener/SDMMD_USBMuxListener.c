/*
 *  SDMUSBmuxListener.c
 *  SDMMobileDevice
 *
 * Copyright (c) 2014, Samantha Marshall
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the 
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer 
 * 		in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of Samantha Marshall nor the names of its contributors may be used to endorse or promote products derived from this
 * 		software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef _SDM_MD_USBMUXLISTENER_C_
#define _SDM_MD_USBMUXLISTENER_C_

#include "SDMMD_USBMuxListener.h"
#include "SDMMD_USBMux_Protocol.h"
#include "SDMMD_AMDevice_Internal.h"
#include "SDMMD_USBMuxListener_Internal.h"
#include "SDMMD_MCP.h"
#include "SDMMD_MCP_Internal.h"
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/un.h>
#include "Core.h"

typedef struct USBMuxResponseCode {
	uint32_t code;
	CFStringRef string;
} ATR_PACK USBMuxResponseCode;

static uint32_t transactionId = 0;

void SDMMD_USBMuxSend(uint32_t sock, struct USBMuxPacket *packet);
void SDMMD_USBMuxReceive(uint32_t sock, struct USBMuxPacket *packet);

void SDMMD_USBMuxResponseCallback(void *context, struct USBMuxPacket *packet);
void SDMMD_USBMuxAttachedCallback(void *context, struct USBMuxPacket *packet);
void SDMMD_USBMuxDetachedCallback(void *context, struct USBMuxPacket *packet);
void SDMMD_USBMuxLogsCallback(void *context, struct USBMuxPacket *packet);
void SDMMD_USBMuxDeviceListCallback(void *context, struct USBMuxPacket *packet);
void SDMMD_USBMuxListenerListCallback(void *context, struct USBMuxPacket *packet);

struct USBMuxResponseCode SDMMD_USBMuxParseReponseCode(CFDictionaryRef dict)
{
	uint32_t code = 0;
	CFNumberRef resultCode = NULL;
	CFStringRef resultString = NULL;
	if (CFDictionaryContainsKey(dict, CFSTR("Number")))
		resultCode = CFDictionaryGetValue(dict, CFSTR("Number"));
	if (CFDictionaryContainsKey(dict, CFSTR("String")))
		resultString = CFDictionaryGetValue(dict, CFSTR("String"));

	if (resultCode) {
		CFNumberGetValue(resultCode, CFNumberGetType(resultCode), &code);
		switch (code) {
			case SDMMD_USBMuxResult_OK: {
				resultString = CFSTR("OK");
				break;
			}
			case SDMMD_USBMuxResult_BadCommand: {
				resultString = CFSTR("Bad Command");
				break;
			}
			case SDMMD_USBMuxResult_BadDevice: {
				resultString = CFSTR("Bad Device");
				break;
			}
			case SDMMD_USBMuxResult_ConnectionRefused: {
				resultString = CFSTR("Connection Refused by Device");
				break;
			}
			case SDMMD_USBMuxResult_Unknown0: {
				break;
			}
			case SDMMD_USBMuxResult_BadMessage: {
				resultString = CFSTR("Incorrect Message Contents");
				break;
			}
			case SDMMD_USBMuxResult_BadVersion: {
				resultString = CFSTR("Bad Protocol Version");
				break;
			}
			case SDMMD_USBMuxResult_Unknown2: {
				break;
			}
			default: {
				break;
			}
		}
	}
	return (struct USBMuxResponseCode){code, resultString};
}

void SDMMD_USBMuxResponseCallback(void *context, struct USBMuxPacket *packet)
{
	if (packet->payload) {
		struct USBMuxResponseCode response = SDMMD_USBMuxParseReponseCode(packet->payload);
		dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
			if (response.code) {
				printf("usbmuxd returned%s: %d - %s.\n", (response.code ? " error" : ""), response.code, (response.string ? CFStringGetCStringPtr(response.string, kCFStringEncodingUTF8) : "Unknown Error Description"));
			}
		});
		// Signal that a response was received, see SDMMD_USBMux_Protocol.c
		dispatch_semaphore_signal(((SDMMD_USBMuxListenerRef)context)->ivars.semaphore);
	}
}

void SDMMD_USBMuxAttachedCallback(void *context, struct USBMuxPacket *packet)
{
	SDMMD_AMDeviceRef newDevice = SDMMD_AMDeviceCreateFromProperties(packet->payload);
	if (newDevice && !CFArrayContainsValue(SDMMobileDevice->ivars.deviceList, CFRangeMake(0, CFArrayGetCount(SDMMobileDevice->ivars.deviceList)), newDevice)) {
		CFMutableArrayRef updateWithNew = CFArrayCreateMutableCopy(kCFAllocatorDefault, 0, SDMMobileDevice->ivars.deviceList);
		// give priority to usb over wifi
		if (newDevice->ivars.connection_type == kAMDeviceConnectionTypeUSB) {
			CFArrayAppendValue(updateWithNew, newDevice);
			dispatch_async(dispatch_get_main_queue(), ^{
				CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), kSDMMD_USBMuxListenerDeviceAttachedNotification, NULL, NULL, true);
			});
			CFSafeRelease(SDMMobileDevice->ivars.deviceList);
			SDMMobileDevice->ivars.deviceList = CFArrayCreateCopy(kCFAllocatorDefault, updateWithNew);
		}
		else if (newDevice->ivars.connection_type == kAMDeviceConnectionTypeWiFi) {
			// wifi
		}
		CFSafeRelease(updateWithNew);
	}
	CFSafeRelease(newDevice);

	dispatch_async(dispatch_get_main_queue(), ^{
		CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), kSDMMD_USBMuxListenerDeviceAttachedNotificationFinished, NULL, NULL, true);
	});
}

void SDMMD_USBMuxDetachedCallback(void *context, struct USBMuxPacket *packet)
{
	uint32_t detachedId;
	CFNumberRef deviceId = CFDictionaryGetValue(packet->payload, CFSTR("DeviceID"));
	CFNumberGetValue(deviceId, kCFNumberSInt64Type, &detachedId);
	CFMutableArrayRef updateWithRemove = CFArrayCreateMutableCopy(kCFAllocatorDefault, 0, SDMMobileDevice->ivars.deviceList);
	uint32_t removeCounter = 0;
	SDMMD_AMDeviceRef detachedDevice = NULL;
	for (uint32_t i = 0; i < CFArrayGetCount(SDMMobileDevice->ivars.deviceList); i++) {
		detachedDevice = (SDMMD_AMDeviceRef)CFArrayGetValueAtIndex(SDMMobileDevice->ivars.deviceList, i);
		// add something for then updating to use wifi if available.
		if (detachedId == SDMMD_AMDeviceGetConnectionID(detachedDevice)) {
			CFArrayRemoveValueAtIndex(updateWithRemove, i - removeCounter);
			removeCounter++;
			dispatch_async(dispatch_get_main_queue(), ^{
				CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), kSDMMD_USBMuxListenerDeviceDetachedNotification, NULL, NULL, true);
			});
		}
	}
	CFSafeRelease(SDMMobileDevice->ivars.deviceList);
	SDMMobileDevice->ivars.deviceList = CFArrayCreateCopy(kCFAllocatorDefault, updateWithRemove);

	CFSafeRelease(updateWithRemove);
	dispatch_async(dispatch_get_main_queue(), ^{
		CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), kSDMMD_USBMuxListenerDeviceDetachedNotificationFinished, NULL, NULL, true);
	});
}

void SDMMD_USBMuxLogsCallback(void *context, struct USBMuxPacket *packet)
{
	dispatch_semaphore_signal(((SDMMD_USBMuxListenerRef)context)->ivars.semaphore);
}

void SDMMD_USBMuxDeviceListCallback(void *context, struct USBMuxPacket *packet)
{
	CFArrayRef devices = CFDictionaryGetValue(packet->payload, CFSTR("DeviceList"));
	for (uint32_t i = 0; i < CFArrayGetCount(devices); i++) {
		SDMMD_AMDeviceRef deviceFromList = SDMMD_AMDeviceCreateFromProperties(CFArrayGetValueAtIndex(devices, i));
		if (deviceFromList && !CFArrayContainsValue(SDMMobileDevice->ivars.deviceList, CFRangeMake(0, CFArrayGetCount(SDMMobileDevice->ivars.deviceList)), deviceFromList)) {
			struct USBMuxPacket *devicePacket = calloc(1, sizeof(struct USBMuxPacket));
			memcpy(devicePacket, packet, sizeof(struct USBMuxPacket));
			devicePacket->payload = CFArrayGetValueAtIndex(devices, i);
			((SDMMD_USBMuxListenerRef)context)->ivars.attachedCallback(context, devicePacket);
		}
		CFSafeRelease(deviceFromList);
	}
	dispatch_semaphore_signal(((SDMMD_USBMuxListenerRef)context)->ivars.semaphore);
}

void SDMMD_USBMuxListenerListCallback(void *context, struct USBMuxPacket *packet)
{
	dispatch_semaphore_signal(((SDMMD_USBMuxListenerRef)context)->ivars.semaphore);
}

void SDMMD_USBMuxUnknownCallback(void *context, struct USBMuxPacket *packet)
{
	printf("Unknown response from usbmuxd!\n");
	if (packet->payload) {
		PrintCFType(packet->payload);
	}
	dispatch_semaphore_signal(((SDMMD_USBMuxListenerRef)context)->ivars.semaphore);
}

SDMMD_USBMuxListenerRef SDMMD_USBMuxCreate()
{
    printf("%s\n",__func__);
	SDMMD_USBMuxListenerRef listener = (SDMMD_USBMuxListenerRef)SDMMD_USBMuxListenerCreateEmpty();
	listener->ivars.socket = -1;
	listener->ivars.isActive = false;
	listener->ivars.operationQueue = dispatch_queue_create("com.samdmarshall.sdmmobiledevice.usbmux-operation-queue", DISPATCH_QUEUE_SERIAL);
	listener->ivars.socketQueue = dispatch_queue_create("com.samdmarshall.sdmmobiledevice.socketQueue", NULL);
	listener->ivars.responseCallback = SDMMD_USBMuxResponseCallback;
	listener->ivars.attachedCallback = SDMMD_USBMuxAttachedCallback;
	listener->ivars.detachedCallback = SDMMD_USBMuxDetachedCallback;
	listener->ivars.logsCallback = SDMMD_USBMuxLogsCallback;
	listener->ivars.deviceListCallback = SDMMD_USBMuxDeviceListCallback;
	listener->ivars.listenerListCallback = SDMMD_USBMuxListenerListCallback;
	listener->ivars.unknownCallback = SDMMD_USBMuxUnknownCallback;
	listener->ivars.responses = CFArrayCreateMutable(kCFAllocatorDefault, 0, NULL);
	return listener;
}

/*
 debugging traffic:
 sudo mv /var/run/usbmuxd /var/run/usbmuxx
 sudo socat -t100 -x -v UNIX-LISTEN:/var/run/usbmuxd,mode=777,reuseaddr,fork UNIX-CONNECT:/var/run/usbmuxx
 */

uint32_t SDMMD_ConnectToUSBMux(time_t recvTimeoutSec)
{
	int result = 0;

	// Initialize socket
	uint32_t sock = socket(AF_UNIX, SOCK_STREAM, 0);

	if (recvTimeoutSec != 0) {
		struct timeval timeout = {.tv_sec = recvTimeoutSec, .tv_usec = 0};
		if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) {
			int err = errno;
			printf("%s: setsockopt SO_RCVTIMEO failed: %d - %s\n", __FUNCTION__, err, strerror(err));
		}
	}

	// Set send/receive buffer sizes
	uint32_t bufSize = 0x00010400;
	if (!result) {
		setsockoptCond(sock, SOL_SOCKET, SO_SNDBUF, bufSize, {result = 1; });
	}

	if (!result) {
		setsockoptCond(sock, SOL_SOCKET, SO_RCVBUF, bufSize, {result = 2; });
	}

	if (!result) {
		uint32_t noPipe = 1; // Disable SIGPIPE on socket i/o error
		setsockoptCond(sock, SOL_SOCKET, SO_NOSIGPIPE, noPipe, {result = 3; });
	}

	if (!result) {
		// Create address structure to point to usbmuxd socket
		char *mux = "/var/run/usbmuxd";
		struct sockaddr_un address;
		address.sun_family = AF_UNIX;
		strncpy(address.sun_path, mux, sizeof(address.sun_path));
		address.sun_len = SUN_LEN(&address);

		// Connect socket
		if (connect(sock, (const struct sockaddr *)&address, sizeof(struct sockaddr_un))) {
			result = 4;
			int err = errno;
			printf("%s: connect socket failed: %d - %s\n", __FUNCTION__, err, strerror(err));
		}
	}

	if (!result) {
		// Set socket to blocking IO mode
		uint32_t nonblock = 0;
		if (ioctl(sock, FIONBIO, &nonblock)) {
			result = 5;
			int err = errno;
			printf("%s: ioctl FIONBIO failed: %d - %s\n", __FUNCTION__, err, strerror(err));
		}
	}

	if (result) {
		// Socket creation failed
		close(sock);
		sock = -1;
	}

	return sock;
}

sdmmd_return_t SDMMD_USBMuxConnectByPort(SDMMD_AMDeviceRef device, uint32_t port, uint32_t *socketConn)
{
	sdmmd_return_t result = kAMDMuxConnectError;
	// 10-sec recv timeout
	*socketConn = SDMMD_ConnectToUSBMux(10);
	if (*socketConn) {
		CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFNumberRef deviceNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &device->ivars.device_id);
		CFDictionarySetValue(dict, CFSTR("DeviceID"), deviceNum);
		CFSafeRelease(deviceNum);

		struct USBMuxPacket *connect = SDMMD_USBMuxCreatePacketType(kSDMMD_USBMuxPacketConnectType, dict);

		// Requesting socket connection for specified port number
		CFNumberRef portNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &port);
		CFDictionarySetValue((CFMutableDictionaryRef)connect->payload, CFSTR("PortNumber"), portNumber);
		CFSafeRelease(portNumber);

		SDMMD_USBMuxSend(*socketConn, connect);
		USBMuxPacketRelease(connect);

		struct USBMuxPacket *response = SDMMD_USBMuxCreatePacketEmpty();
		SDMMD_USBMuxReceive(*socketConn, response);

		// Check response for success, on failure result will be kAMDMuxConnectError
		if (response->payload) {

			CFStringRef msgType = NULL;
			if ((msgType = CFDictionaryGetValue(response->payload, CFSTR("MessageType"))) && CFEqual(msgType, CFSTR("Result"))) {

				CFNumberRef msgResult = NULL;
				SInt32 ok = kAMDSuccess;
				CFNumberRef okResult = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &ok);
				if ((msgResult = CFDictionaryGetValue(response->payload, CFSTR("Number"))) && CFEqual(msgResult, okResult)) {
					// Socket negotiation successful
					result = kAMDSuccess;
				}
				CFSafeRelease(okResult);
			}
		}

		USBMuxPacketRelease(response);

		CFSafeRelease(dict);
	}
	return result;
}

void SDMMD_USBMuxStartListener(SDMMD_USBMuxListenerRef *listener)
{
	__block uint64_t bad_packet_counter = 0;
	dispatch_sync(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        // no timeout for recv
		(*listener)->ivars.socket = SDMMD_ConnectToUSBMux(0);
		(*listener)->ivars.socketSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (*listener)->ivars.socket, 0, (*listener)->ivars.socketQueue);
		dispatch_source_set_event_handler((*listener)->ivars.socketSource, ^{
            //printf("socketSourceEventHandler: fired\n");
            
            // Allocate and receive packet
			struct USBMuxPacket *packet = (struct USBMuxPacket *)calloc(1, sizeof(struct USBMuxPacket));
			SDMMD_USBMuxReceive((*listener)->ivars.socket, packet);
            
            // Validate packet payload
			if (packet->payload != NULL) {
                
				if (CFDictionaryContainsKey(packet->payload, CFSTR("MessageType"))) {
                    
					CFStringRef type = CFDictionaryGetValue(packet->payload, CFSTR("MessageType"));
					if (CFStringCompare(type, SDMMD_USBMuxPacketMessage[kSDMMD_USBMuxPacketResultType], 0) == 0) {
                        // Packet ownership transfered to response handler
						CFArrayAppendValue((*listener)->ivars.responses, packet);
						(*listener)->ivars.responseCallback((*listener), packet);
					}
					else if (CFStringCompare(type, SDMMD_USBMuxPacketMessage[kSDMMD_USBMuxPacketAttachType], 0) == 0) {
						(*listener)->ivars.attachedCallback((*listener), packet);
                        // Destroy received packet
                        USBMuxPacketRelease(packet);
					}
					else if (CFStringCompare(type, SDMMD_USBMuxPacketMessage[kSDMMD_USBMuxPacketDetachType], 0) == 0) {
						(*listener)->ivars.detachedCallback((*listener), packet);
                        // Destroy received packet
                        USBMuxPacketRelease(packet);
					}
				}
				else {
                    // Packet ownership transfered to response handler
					CFArrayAppendValue((*listener)->ivars.responses, packet);
					if (CFDictionaryContainsKey(packet->payload, CFSTR("Logs"))) {
						(*listener)->ivars.logsCallback((*listener), packet);
					}
					else if (CFDictionaryContainsKey(packet->payload, CFSTR("DeviceList"))) {
						(*listener)->ivars.deviceListCallback((*listener), packet);
					}
					else if (CFDictionaryContainsKey(packet->payload, CFSTR("ListenerList"))) {
						(*listener)->ivars.listenerListCallback((*listener), packet);
					}
					else {
						(*listener)->ivars.unknownCallback((*listener), packet);
					}
				}
			}
			else if (packet->body.length == 0) {
				// ignore this zero length packet
                // Destroy received packet
                USBMuxPacketRelease(packet);
			}
			else {
				bad_packet_counter++;
                printf("%s: failed to decode CFPropertyList from packet payload\n",__FUNCTION__);
				if (bad_packet_counter > 10) {
					printf("eating bad packets, exiting...\n");
					exit(EXIT_FAILURE);
				}
                
                // Destroy received packet
                USBMuxPacketRelease(packet);
            }
		});
        dispatch_source_set_cancel_handler((*listener)->ivars.socketSource, ^{
            printf("%s: source canceled\n",__FUNCTION__);
        });
		dispatch_resume((*listener)->ivars.socketSource);
				
		while (!(*listener)->ivars.isActive) {
			struct USBMuxPacket *startListen = SDMMD_USBMuxCreatePacketType(kSDMMD_USBMuxPacketListenType, NULL);
			SDMMD_USBMuxListenerSend(*listener, &startListen);
			if (startListen->payload) {
				struct USBMuxResponseCode response = SDMMD_USBMuxParseReponseCode(startListen->payload);
				if (response.code == 0){
					(*listener)->ivars.isActive = true;
                }
				else {
                    printf("%s: non-zero response code. trying again. code:%i string:%s\n", __FUNCTION__, response.code, response.string ? CFStringGetCStringPtr(response.string, kCFStringEncodingUTF8):"");
                }
			}
			else {
                printf("%s: no response payload. trying again.\n",__FUNCTION__);
            }
			USBMuxPacketRelease(startListen);
		}
	});
}

struct USBMuxPacket *SDMMD_USBMuxCreatePacketEmpty(void)
{
	struct USBMuxPacket *packet = (struct USBMuxPacket *)calloc(1, sizeof(struct USBMuxPacket));
	return packet;
}

struct USBMuxPacket *SDMMD_USBMuxCreatePacketType(SDMMD_USBMuxPacketMessageType type, CFDictionaryRef dict)
{
	struct USBMuxPacket *packet = SDMMD_USBMuxCreatePacketEmpty();
	if (type == kSDMMD_USBMuxPacketListenType || type == kSDMMD_USBMuxPacketConnectType) {
		packet->timeout = dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC * 30);
	}
	else {
		packet->timeout = dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC * 5);
	}
	packet->body = (struct USBMuxPacketBody){16, 1, 8, transactionId};
	transactionId++;
	packet->payload = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue((CFMutableDictionaryRef)packet->payload, CFSTR("BundleID"), CFSTR("com.samdmarshall.sdmmobiledevice"));
	CFDictionarySetValue((CFMutableDictionaryRef)packet->payload, CFSTR("ClientVersionString"), CFSTR("usbmuxd-323")); // 344
	CFDictionarySetValue((CFMutableDictionaryRef)packet->payload, CFSTR("ProgName"), CFSTR("SDMMobileDevice"));
	uint32_t version = 3;
	CFNumberRef versionNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &version);
	CFDictionarySetValue((CFMutableDictionaryRef)packet->payload, CFSTR("kLibUSBMuxVersion"), versionNumber);
	CFSafeRelease(versionNumber);

	if (dict) {
		CFIndex count = CFDictionaryGetCount(dict);
		void *keys[count];
		void *values[count];
		CFDictionaryGetKeysAndValues(dict, (const void **)keys, (const void **)values);
		for (uint32_t i = 0; i < count; i++) {
			CFDictionarySetValue((CFMutableDictionaryRef)packet->payload, keys[i], values[i]);
		}
	}
	CFDictionarySetValue((CFMutableDictionaryRef)packet->payload, CFSTR("MessageType"), SDMMD_USBMuxPacketMessage[type]);
	if (type == kSDMMD_USBMuxPacketConnectType) {
		uint16_t port = 0x7ef2;
		CFNumberRef portNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt16Type, &port);
		CFDictionarySetValue((CFMutableDictionaryRef)packet->payload, CFSTR("PortNumber"), portNumber);
		CFSafeRelease(portNumber);
	}
	if (type == kSDMMD_USBMuxPacketListenType) {
		uint32_t connection = 0;
		CFNumberRef connectionType = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &connection);
		CFDictionarySetValue((CFMutableDictionaryRef)packet->payload, CFSTR("ConnType"), connectionType);
		CFSafeRelease(connectionType);
	}

	CFDataRef xmlData = CFPropertyListCreateXMLData(kCFAllocatorDefault, packet->payload);
	packet->body.length = 16 + (uint32_t)CFDataGetLength(xmlData);
	CFSafeRelease(xmlData);
	return packet;
}

void USBMuxPacketRelease(struct USBMuxPacket *packet)
{
	CFSafeRelease(packet->payload);
	Safe(free, packet);
}

#endif