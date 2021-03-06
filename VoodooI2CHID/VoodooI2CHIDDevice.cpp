//
//  VoodooI2CHIDDevice.cpp
//  VoodooI2CHID
//
//  Created by Alexandre on 25/08/2017.
//  Copyright © 2017 Alexandre Daoud. All rights reserved.
//

#include <IOKit/hid/IOHIDDevice.h>
#include <kern/locks.h>

#include "VoodooI2CHIDDevice.hpp"
#include "../../../VoodooI2C/VoodooI2C/VoodooI2CDevice/VoodooI2CDeviceNub.hpp"

#define super IOHIDDevice
OSDefineMetaClassAndStructors(VoodooI2CHIDDevice, IOHIDDevice);

bool VoodooI2CHIDDevice::init(OSDictionary* properties) {
    if (!super::init(properties))
        return false;
    awake = true;
    read_in_progress = false;
    bool temp = false;
    reset_event = &temp;
    sim_report_buffer = 0;
    idle_counter = 0;
    i2chid_dbg = false;
    i2chid_mdata = 0;
    i2chid_pattern = 0;
    memset(&hid_descriptor, 0, sizeof(VoodooI2CHIDDeviceHIDDescriptor));
    
    client_lock = IOLockAlloc();
    
    clients = OSArray::withCapacity(1);

    if (!client_lock || !clients) {
        OSSafeReleaseNULL(clients);
        return false;
    }

    return true;
}

void VoodooI2CHIDDevice::free() {
    if (client_lock)
        IOLockFree(client_lock);

    super::free();
}

IOReturn VoodooI2CHIDDevice::getHIDDescriptor() {
    VoodooI2CHIDDeviceCommand command;
    command.c.reg = hid_descriptor_register;

    if (api->writeReadI2C(command.data, 2, (UInt8*)&hid_descriptor, (UInt16)sizeof(VoodooI2CHIDDeviceHIDDescriptor)) != kIOReturnSuccess) {
        IOLog("%s::%s Request for HID descriptor failed\n", getName(), name);
        return kIOReturnIOError;
    }
    
    return parseHIDDescriptor();
}

IOReturn VoodooI2CHIDDevice::parseHIDDescriptor() {
    if (hid_descriptor.bcdVersion != 0x0100) {
        IOLog("%s::%s Incorrect BCD version %d\n", getName(), name, hid_descriptor.bcdVersion);
        return kIOReturnInvalid;
    }
    
    if (hid_descriptor.wHIDDescLength != sizeof(VoodooI2CHIDDeviceHIDDescriptor)) {
        IOLog("%s::%s Unexpected size of HID descriptor\n", getName(), name);
        return kIOReturnInvalid;
    }

    OSDictionary* property_array = OSDictionary::withCapacity(1);
    property_array->setObject("HIDDescLength", OSNumber::withNumber(hid_descriptor.wHIDDescLength, 32));
    property_array->setObject("BCDVersion", OSNumber::withNumber(hid_descriptor.bcdVersion, 32));
    property_array->setObject("ReportDescLength", OSNumber::withNumber(hid_descriptor.wReportDescLength, 32));
    property_array->setObject("ReportDescRegister", OSNumber::withNumber(hid_descriptor.wReportDescRegister, 32));
    property_array->setObject("MaxInputLength", OSNumber::withNumber(hid_descriptor.wMaxInputLength, 32));
    property_array->setObject("InputRegister", OSNumber::withNumber(hid_descriptor.wInputRegister, 32));
    property_array->setObject("MaxOutputLength", OSNumber::withNumber(hid_descriptor.wMaxOutputLength, 32));
    property_array->setObject("OutputRegister", OSNumber::withNumber(hid_descriptor.wOutputRegister, 32));
    property_array->setObject("CommandRegister", OSNumber::withNumber(hid_descriptor.wCommandRegister, 32));
    property_array->setObject("DataRegister", OSNumber::withNumber(hid_descriptor.wDataRegister, 32));
    property_array->setObject("VendorID", OSNumber::withNumber(hid_descriptor.wVendorID, 32));
    property_array->setObject("ProductID", OSNumber::withNumber(hid_descriptor.wProductID, 32));
    property_array->setObject("VersionID", OSNumber::withNumber(hid_descriptor.wVersionID, 32));

    setProperty("HIDDescriptor", property_array);

    property_array->release();

    return kIOReturnSuccess;
}

IOReturn VoodooI2CHIDDevice::getHIDDescriptorAddress() {
    UInt32 guid_1 = 0x3CDFF6F7;
    UInt32 guid_2 = 0x45554267;
    UInt32 guid_3 = 0x0AB305AD;
    UInt32 guid_4 = 0xDE38893D;
    
    OSObject *result = NULL;
    OSObject *params[4];
    char buffer[16];
    
    memcpy(buffer, &guid_1, 4);
    memcpy(buffer + 4, &guid_2, 4);
    memcpy(buffer + 8, &guid_3, 4);
    memcpy(buffer + 12, &guid_4, 4);
    
    
    params[0] = OSData::withBytes(buffer, 16);
    params[1] = OSNumber::withNumber(0x1, 8);
    params[2] = OSNumber::withNumber(0x1, 8);
    params[3] = OSNumber::withNumber((unsigned long long)0x0, 8);
    
    acpi_device->evaluateObject("_DSM", &result, params, 4);
    if (!result)
        acpi_device->evaluateObject("XDSM", &result, params, 4);
    if (!result) {
        IOLog("%s::%s Could not find suitable _DSM or XDSM method in ACPI tables\n", getName(), name);
        return kIOReturnNotFound;
    }
    
    OSNumber* number = OSDynamicCast(OSNumber, result);
    if (number) {
        setProperty("HIDDescriptorAddress", number);
        hid_descriptor_register = number->unsigned16BitValue();
    }

    if (result)
        result->release();
    
    params[0]->release();
    params[1]->release();
    params[2]->release();
    params[3]->release();
    
    if (!number) {
        IOLog("%s::%s HID descriptor register invalid\n", getName(), name);
        return kIOReturnInvalid;
    }
    
    return kIOReturnSuccess;
}

void VoodooI2CHIDDevice::logHexDump(const void *data, int size) const {
    IOLog("%s::%s Buffer size = %d, hex dump:\n", getName(), name, size);
    int lines = size / 16;
    int bytes = size % 16;
    const uint8_t* m = reinterpret_cast<const uint8_t*>(data);
    for (int i=0; i<lines; ++i) {
        IOLog("%s::%s %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", getName(), name,
              m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        m += 16;
    }
    if (bytes != 0) {
        IOLog("%s::%s ", getName(), name);
        for (int i=0; i<bytes; ++i)
            IOLog("%02x ", m[i]);
        IOLog("\n");
    }
}

bool VoodooI2CHIDDevice::getInputReport() {
    IOBufferMemoryDescriptor* buffer;
    IOReturn ret;
    
    uint8_t *report = interrupt_simulator ? sim_report_buffer : (uint8_t*)IOMalloc(hid_descriptor.wMaxInputLength);
    if (interrupt_simulator)
        report[0] = report[1] = 0;

    ret = api->readI2C(report, hid_descriptor.wMaxInputLength);
    
    int return_size = (ret == kIOReturnSuccess) ? (report[0] | report[1] << 8) : 0;
    if (!return_size) {
        // IOLog("%s::%s Device sent a 0-length report\n", getName(), name);
        command_gate->commandWakeup(&reset_event);
        goto exit;
    }

    if (!ready_for_input)
        goto exit;

    if (return_size > hid_descriptor.wMaxInputLength) {
        // IOLog("%s: Incomplete report %d/%d\n", getName(), hid_descriptor.wMaxInputLength, return_size);
        goto exit;
    }

    buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, 0, return_size);
    buffer->writeBytes(0, report + 2, return_size - 2);
    
    ret = handleReport(buffer, kIOHIDReportTypeInput);

    if (ret != kIOReturnSuccess)
        IOLog("%s::%s Error handling input report: 0x%.8x\n", getName(), name, ret);
    
    buffer->release();
    if (!interrupt_simulator)
        IOFree(report, hid_descriptor.wMaxInputLength);
    
exit:
    read_in_progress = false;
    if (!interrupt_simulator)
        thread_terminate(current_thread());
  
    if (interrupt_simulator && return_size > 0 && return_size <= hid_descriptor.wMaxInputLength && ret == kIOReturnSuccess) {
        if (i2chid_dbg)
            logHexDump(report, return_size);
    
        if (i2chid_pattern != 0) {
            if (i2chid_dbg && return_size >= i2chid_pattern->getLength()) {
                if (i2chid_pattern->isEqualTo(report, i2chid_pattern->getLength()))
                    IOLog("%s::%s pattern matches\n", getName(), name);
                else
                    IOLog("%s::%s pattern does not match\n", getName(), name);
            }
            return (return_size >= i2chid_pattern->getLength() && i2chid_pattern->isEqualTo(report, i2chid_pattern->getLength()));
        }
        if (i2chid_mdata != 0) {
            return (return_size >= i2chid_mdata);
        }
    }
    return false;
}

IOReturn VoodooI2CHIDDevice::getReport(IOMemoryDescriptor* report, IOHIDReportType reportType, IOOptionBits options) {
    if (reportType != kIOHIDReportTypeFeature && reportType != kIOHIDReportTypeInput)
        return kIOReturnBadArgument;

    UInt8 args[3];
    IOReturn ret;
    int args_len = 0;
    UInt16 read_register = hid_descriptor.wDataRegister;
    UInt8 report_id = options & 0xFF;
    UInt8 raw_report_type = (reportType == kIOHIDReportTypeFeature) ? 0x03 : 0x01;

    UInt8* buffer = (UInt8*)IOMalloc(report->getLength());
    
    
    if (report_id >= 0x0F) {
        args[args_len++] = report_id;
        report_id = 0x0F;
    }

    args[args_len++] = read_register & 0xFF;
    args[args_len++] = read_register >> 8;
    
    UInt8 length = 4;
    
    read_in_progress = true;
    
    VoodooI2CHIDDeviceCommand* command = (VoodooI2CHIDDeviceCommand*)IOMalloc(4 + args_len);
    memset(command, 0, 4+args_len);
    command->c.reg = hid_descriptor.wCommandRegister;
    command->c.opcode = 0x02;
    command->c.report_type_id = report_id | raw_report_type << 4;
    
    UInt8* raw_command = (UInt8*)command;
    
    memcpy(raw_command + length, args, args_len);
    length += args_len;
    ret = api->writeReadI2C(raw_command, length, buffer, report->getLength());
    
    report->writeBytes(0, buffer+2, report->getLength()-2);
    
    IOFree(command, 4+args_len);
    
    read_in_progress = false;

    return ret;
}

bool VoodooI2CHIDDevice::interruptOccured(OSObject* owner, IOInterruptEventSource* src, int intCount) {
    if (read_in_progress)
        return false;
    if (!awake)
        return false;
    
    read_in_progress = true;
    bool result = false;
    
    if (!interrupt_simulator) {
        thread_t new_thread;
        kern_return_t ret = kernel_thread_start(OSMemberFunctionCast(thread_continue_t, this, &VoodooI2CHIDDevice::getInputReport), this, &new_thread);
        if (ret != KERN_SUCCESS) {
            read_in_progress = false;
            IOLog("%s::%s Thread error while attempting to get input report\n", getName(), name);
        } else {
            thread_deallocate(new_thread);
            result = true;
        }
    } else {
        result = VoodooI2CHIDDevice::getInputReport();
    }
    
    return result;
}

VoodooI2CHIDDevice* VoodooI2CHIDDevice::probe(IOService* provider, SInt32* score) {
    if (!super::probe(provider, score))
        return NULL;

    name = getMatchedName(provider);
    
    acpi_device = OSDynamicCast(IOACPIPlatformDevice, provider->getProperty("acpi-device"));
    //acpi_device->retain();
    
    if (!acpi_device) {
        IOLog("%s::%s Could not get ACPI device\n", getName(), name);
        return NULL;
    }
    
    // Sometimes an I2C HID will have power state methods, lets turn it on in case
    
    acpi_device->evaluateObject("_PS0");

    api = OSDynamicCast(VoodooI2CDeviceNub, provider);
    //api->retain();
    
    if (!api) {
        IOLog("%s::%s Could not get VoodooI2C API access\n", getName(), name);
        return NULL;
    }
    
    if (getHIDDescriptorAddress() != kIOReturnSuccess) {
        IOLog("%s::%s Could not get HID descriptor\n", getName(), name);
        return NULL;
    }

    if (getHIDDescriptor() != kIOReturnSuccess) {
        IOLog("%s::%s Could not get HID descriptor\n", getName(), name);
        return NULL;
    }
    
    sim_report_buffer = (unsigned char *)IOMalloc(hid_descriptor.wMaxInputLength);

    return this;
}

void VoodooI2CHIDDevice::releaseResources() {
    if (command_gate) {
        command_gate->disable();
        work_loop->removeEventSource(command_gate);
        command_gate->release();
        command_gate = NULL;
    }
    
    if (interrupt_simulator) {
        interrupt_simulator->disable();
        work_loop->removeEventSource(interrupt_simulator);
        interrupt_simulator->release();
        interrupt_simulator = NULL;
    }

    if (interrupt_source) {
        interrupt_source->disable();
        work_loop->removeEventSource(interrupt_source);
        interrupt_source->release();
        interrupt_source = NULL;
    }

    if (work_loop) {
        work_loop->release();
        work_loop = NULL;
    }
    
    if (acpi_device) {
        acpi_device->release();
        acpi_device = NULL;
    }
    
    if (api) {
        if (api->isOpen(this))
            api->close(this);
        api->release();
        api = NULL;
    }
    
    if (sim_report_buffer) {
        IOFree(sim_report_buffer, hid_descriptor.wMaxInputLength);
        sim_report_buffer = NULL;
    }
    
    if (i2chid_pattern) {
        i2chid_pattern->release();
        i2chid_pattern = NULL;
    }
}

IOReturn VoodooI2CHIDDevice::resetHIDDevice() {
    return command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooI2CHIDDevice::resetHIDDeviceGated));
}

IOReturn VoodooI2CHIDDevice::resetHIDDeviceGated() {
    setHIDPowerState(kVoodooI2CStateOn);

    read_in_progress = true;

    VoodooI2CHIDDeviceCommand command;
    command.c.reg = hid_descriptor.wCommandRegister;
    command.c.opcode = 0x01;
    command.c.report_type_id = 0;
    
    api->writeI2C(command.data, 4);
    IOSleep(100);
    
    AbsoluteTime absolute_time;

    // Device is required to complete a host-initiated reset in at most 6 seconds.

    nanoseconds_to_absolutetime(6000000000, &absolute_time);

    read_in_progress = false;

    IOReturn sleep = command_gate->commandSleep(&reset_event, absolute_time, THREAD_UNINT);

    if (sleep == THREAD_TIMED_OUT) {
        IOLog("%s::%s Timeout waiting for device to complete host initiated reset\n", getName(), name);
        return kIOReturnTimeout;
    }

    return kIOReturnSuccess;
}

IOReturn VoodooI2CHIDDevice::setHIDPowerState(VoodooI2CState state) {
    read_in_progress = true;
    VoodooI2CHIDDeviceCommand command;
    IOReturn ret = kIOReturnSuccess;
    int attempts = 3;
    do {
        command.c.reg = hid_descriptor.wCommandRegister;
        command.c.opcode = 0x08;
        command.c.report_type_id = state ? I2C_HID_PWR_ON : I2C_HID_PWR_SLEEP;

        ret = api->writeI2C(command.data, 4);
        IOSleep(100);
    } while (ret != kIOReturnSuccess && --attempts >= 0);
    read_in_progress = false;
    return ret;
}

IOReturn VoodooI2CHIDDevice::setReport(IOMemoryDescriptor* report, IOHIDReportType reportType, IOOptionBits options) {
    if (reportType != kIOHIDReportTypeFeature && reportType != kIOHIDReportTypeOutput)
        return kIOReturnBadArgument;
    
    UInt16 data_register = hid_descriptor.wDataRegister;
    UInt8 raw_report_type = (reportType == kIOHIDReportTypeFeature) ? 0x03 : 0x02;
    UInt8 idx = 0;
    UInt16 size;
    UInt16 arguments_length;
    UInt8 report_id = options & 0xFF;
    UInt8* buffer = (UInt8*)IOMalloc(report->getLength());
    report->readBytes(0, buffer, report->getLength());
    
    size = 2 +
    (report_id ? 1 : 0)     /* reportID */ +
    report->getLength()     /* buf */;

    arguments_length = (report_id >= 0x0F ? 1 : 0)  /* optional third byte */ +
    2                                               /* dataRegister */ +
    size                                            /* args */;
    
    UInt8* arguments = (UInt8*)IOMalloc(arguments_length);
    memset(arguments, 0, arguments_length);
    
    if (report_id >= 0x0F) {
        arguments[idx++] = report_id;
        report_id = 0x0F;
    }
    
    arguments[idx++] = data_register & 0xFF;
    arguments[idx++] = data_register >> 8;
    
    arguments[idx++] = size & 0xFF;
    arguments[idx++] = size >> 8;
    
    if (report_id)
        arguments[idx++] = report_id;
    
    memcpy(&arguments[idx], buffer, report->getLength());
    
    UInt8 length = 4;

    read_in_progress = true;

    VoodooI2CHIDDeviceCommand* command = (VoodooI2CHIDDeviceCommand*)IOMalloc(4 + arguments_length);
    memset(command, 0, 4+arguments_length);
    command->c.reg = hid_descriptor.wCommandRegister;
    command->c.opcode = 0x03;
    command->c.report_type_id = report_id | raw_report_type << 4;
    
    UInt8* raw_command = (UInt8*)command;
    
    memcpy(raw_command + length, arguments, arguments_length);
    length += arguments_length;
    IOReturn ret = api->writeI2C(raw_command, length);
    IOSleep(10);
    
    IOFree(command, 4+arguments_length);
    IOFree(arguments, arguments_length);

    read_in_progress = false;
    return ret;
}

IOReturn VoodooI2CHIDDevice::setPowerState(unsigned long whichState, IOService* whatDevice) {
    if (whatDevice != this)
        return kIOReturnInvalid;
    if (whichState == kVoodooI2CStateOff) {
        if (awake) {
            while (read_in_progress) {
                IOSleep(100);
            }

            setHIDPowerState(kVoodooI2CStateOff);
            
            IOLog("%s::%s Going to sleep\n", getName(), name);
            awake = false;
        }
    } else if (whichState == kVoodooI2CStateOn) {
        if (!awake) {
            awake = true;
            
            setHIDPowerState(kVoodooI2CStateOn);
            
            read_in_progress = true;

            VoodooI2CHIDDeviceCommand command;
            command.c.reg = hid_descriptor.wCommandRegister;
            command.c.opcode = 0x01;
            command.c.report_type_id = 0;

            api->writeI2C(command.data, 4);
            IOSleep(100);

            read_in_progress = false;
            
            IOLog("%s::%s Woke up\n", getName(), name);
        }
    }

    return kIOPMAckImplied;
}

bool VoodooI2CHIDDevice::handleStart(IOService* provider) {
    if (!IOHIDDevice::handleStart(provider)) {
        return false;
    }
    
    work_loop = getWorkLoop();
    
    if (!work_loop) {
        IOLog("%s::%s Could not get work loop\n", getName(), name);
        goto exit;
    }

    work_loop->retain();

    command_gate = IOCommandGate::commandGate(this);
    if (!command_gate || (work_loop->addEventSource(command_gate) != kIOReturnSuccess)) {
        IOLog("%s::%s Could not open command gate\n", getName(), name);
        goto exit;
    }

    acpi_device->retain();
    api->retain();

    if (!api->open(this)) {
        IOLog("%s::%s Could not open API\n", getName(), name);
        goto exit;
    }
    
    interrupt_source = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventAction, this, &VoodooI2CHIDDevice::interruptOccured), api, 0);
    if (!interrupt_source) {
        IOLog("%s::%s Warning: Could not get interrupt event source, using polling instead\n", getName(), name);
        interrupt_simulator = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &VoodooI2CHIDDevice::simulateInterrupt));
        
        if (!interrupt_simulator) {
            IOLog("%s::%s Could not get timer event source\n", getName(), name);
            goto exit;
        }
        work_loop->addEventSource(interrupt_simulator);
        interrupt_simulator->setTimeoutMS(200);
    } else {
        work_loop->addEventSource(interrupt_source);
        interrupt_source->enable();
    }

    resetHIDDevice();


    PMinit();
    api->joinPMtree(this);
    registerPowerDriver(this, VoodooI2CIOPMPowerStates, kVoodooI2CIOPMNumberPowerStates);
    
    // Give the reset a bit of time so that IOHIDDevice doesnt happen to start requesting the report
    // descriptor before the driver is ready

    IOSleep(100);

    return true;
exit:
    releaseResources();
    return false;
}

bool VoodooI2CHIDDevice::start(IOService* provider) {
    if (!super::start(provider))
        return false;
    
    uint32_t val = 0;
    
    // Check if debugging is enabled
    if (PE_parse_boot_argn("-i2chid_dbg", &val, sizeof(val)))
        i2chid_dbg = true;
    
    // Check if minimal data size is overriden
    if (PE_parse_boot_argn("i2chid_mdata", &val, sizeof(val)) && val > 0) {
        i2chid_mdata = val;
        IOLog("%s::%s Minimal data size is set to: %d\n", getName(), name, i2chid_mdata);
    } else {
        OSData *data = OSDynamicCast(OSData, provider->getProperty("i2chid_mdata"));
        if (data && data->getLength() == sizeof(int32_t)) {
            i2chid_mdata = *static_cast<const int32_t *>(data->getBytesNoCopy());
            IOLog("%s::%s Minimal data size is set from ioreg to: %d\n", getName(), name, i2chid_mdata);
        }
    }
    
    char i2chid_pattern_str[50] = {};
    if (PE_parse_boot_argn("_i2chid_pattern", i2chid_pattern_str, sizeof(i2chid_pattern_str)) && i2chid_pattern_str[0] != 0) {
        uint8_t str_len = strlen(i2chid_pattern_str);
        IOLog("%s::%s i2chid_pattern is set to: %s, len: %d\n", getName(), name, i2chid_pattern_str, str_len);
        for (int i = 0; i < (str_len / 2); i++)
        {
            unsigned int byte = 0;
            if (sscanf(&i2chid_pattern_str[2*i], "%02X", &byte) != 1)
                break;
            if (!i2chid_pattern)
                i2chid_pattern = OSData::withCapacity(0);
            i2chid_pattern->appendByte(byte, 1);
        }
        if (i2chid_dbg && i2chid_pattern)
            logHexDump(i2chid_pattern->getBytesNoCopy(), i2chid_pattern->getLength());
    } else {
        OSData *data = OSDynamicCast(OSData, provider->getProperty("i2chid_pattern"));
        if (data != 0 && data->getLength() != 0) {
            i2chid_pattern = OSData::withData(data);
            IOLog("%s::%s i2chid_pattern is set to value from ioreg, len: %d\n", getName(), name, i2chid_pattern->getLength());
            if (i2chid_dbg)
                logHexDump(i2chid_pattern->getBytesNoCopy(), i2chid_pattern->getLength());
        }
    }
    
    ready_for_input = true;
    
    setProperty("VoodooI2CServices Supported", kOSBooleanTrue);

    return true;
}

void VoodooI2CHIDDevice::stop(IOService* provider) {
    IOLockLock(client_lock);
    for(;;) {
        if (!clients->getCount()) {
            break;
        }
        
        IOLockSleep(client_lock, &client_lock, THREAD_UNINT);
    }
    IOLockUnlock(client_lock);
    
    releaseResources();
    OSSafeReleaseNULL(clients);
    PMstop();
    super::stop(provider);
}

IOReturn VoodooI2CHIDDevice::newReportDescriptor(IOMemoryDescriptor** descriptor) const {
    if (!hid_descriptor.wReportDescLength) {
        IOLog("%s::%s Invalid report descriptor size\n", getName(), name);
        return kIOReturnDeviceError;
    }

    VoodooI2CHIDDeviceCommand command;
    command.c.reg = hid_descriptor.wReportDescRegister;
    
    UInt8* buffer = reinterpret_cast<UInt8*>(IOMalloc(hid_descriptor.wReportDescLength));
    memset(buffer, 0, hid_descriptor.wReportDescLength);

    if (api->writeReadI2C(command.data, 2, buffer, hid_descriptor.wReportDescLength) != kIOReturnSuccess) {
        IOLog("%s::%s Could not get report descriptor\n", getName(), name);
        IOFree(buffer, hid_descriptor.wReportDescLength);
        return kIOReturnIOError;
    }

    IOBufferMemoryDescriptor* report_descriptor = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, 0, hid_descriptor.wReportDescLength);

    if (!report_descriptor) {
        IOLog("%s::%s Could not allocated buffer for report descriptor\n", getName(), name);
        return kIOReturnNoResources;
    }

    report_descriptor->writeBytes(0, buffer, hid_descriptor.wReportDescLength);
    *descriptor = report_descriptor;

    IOFree(buffer, hid_descriptor.wReportDescLength);

    return kIOReturnSuccess;
}

OSNumber* VoodooI2CHIDDevice::newVendorIDNumber() const {
    return OSNumber::withNumber(hid_descriptor.wVendorID, 16);
}

OSNumber* VoodooI2CHIDDevice::newProductIDNumber() const {
    return OSNumber::withNumber(hid_descriptor.wProductID, 16);
}

OSNumber* VoodooI2CHIDDevice::newVersionNumber() const {
    return OSNumber::withNumber(hid_descriptor.wVersionID, 16);
}

OSString* VoodooI2CHIDDevice::newTransportString() const {
    return OSString::withCString("I2C");
}

OSString* VoodooI2CHIDDevice::newManufacturerString() const {
    return OSString::withCString("Apple");
}

void VoodooI2CHIDDevice::simulateInterrupt(OSObject* owner, IOTimerEventSource* timer) {
    bool result = interruptOccured(owner, nullptr, 0);
    if (result)
        idle_counter = 0;
    UInt32 timeout = INTERRUPT_SIMULATOR_DEF_TIMEOUT;
    if (i2chid_mdata != 0 || i2chid_pattern != 0)
         timeout = (result || (++idle_counter < 500)) ? INTERRUPT_SIMULATOR_BUSY_TIMEOUT : INTERRUPT_SIMULATOR_IDLE_TIMEOUT;
    interrupt_simulator->setTimeoutMS(timeout);
}

bool VoodooI2CHIDDevice::open(IOService *forClient, IOOptionBits options, void *arg) {
    IOLockLock(client_lock);
    clients->setObject(forClient);
    IOUnlock(client_lock);
    
    return super::open(forClient, options, arg);
}

void VoodooI2CHIDDevice::close(IOService *forClient, IOOptionBits options) {
    IOLockLock(client_lock);
    
    for(int i = 0; i < clients->getCount(); i++) {
        OSObject* service = clients->getObject(i);
        
        if (service == forClient) {
            clients->removeObject(i);
            break;
        }
    }
    
    IOUnlock(client_lock);

    IOLockWakeup(client_lock, &client_lock, true);
    
    super::close(forClient, options);
}
