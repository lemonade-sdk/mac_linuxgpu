/* The IOKit calls libmlg_drm's transport makes, routed to the test kernel of
 * linuxu/tests/test_lx_transport.cpp (scripts/test-lx-transport.sh), which
 * hands them to the dext's own dispatch. Force-included before IOKitLib.h
 * in every translation unit of the test, so IOKitLib.h itself declares the
 * test kernel's functions with IOKit's exact signatures. */
#ifndef LX_TRANSPORT_IOKIT_FAKE_H
#define LX_TRANSPORT_IOKIT_FAKE_H

#define IOServiceGetMatchingServices	lxt_IOServiceGetMatchingServices
#define IOIteratorNext			lxt_IOIteratorNext
#define IOObjectRelease			lxt_IOObjectRelease
#define IORegistryEntryCreateCFProperty	lxt_IORegistryEntryCreateCFProperty
#define IORegistryEntryGetRegistryEntryID lxt_IORegistryEntryGetRegistryEntryID
#define IOServiceOpen			lxt_IOServiceOpen
#define IOServiceClose			lxt_IOServiceClose
#define IOConnectCallMethod		lxt_IOConnectCallMethod
#define IOConnectCallScalarMethod	lxt_IOConnectCallScalarMethod
#define IOConnectCallAsyncMethod	lxt_IOConnectCallAsyncMethod
#define IONotificationPortCreate	lxt_IONotificationPortCreate
#define IONotificationPortDestroy	lxt_IONotificationPortDestroy
#define IONotificationPortGetMachPort	lxt_IONotificationPortGetMachPort
#define IODispatchCalloutFromMessage	lxt_IODispatchCalloutFromMessage
#define IOConnectMapMemory64		lxt_IOConnectMapMemory64
#define IOConnectUnmapMemory64		lxt_IOConnectUnmapMemory64

#endif
