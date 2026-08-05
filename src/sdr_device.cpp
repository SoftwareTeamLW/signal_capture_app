#include "sdr_device.hpp"

#include <uhd/device.hpp>

#include <algorithm>
#include <cctype>
#include <exception>

#include <uhd/usrp/multi_usrp.hpp>

namespace {

std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

// 这里只解析 UHD 已安全返回的文本，不访问不同设备差异很大的属性树。
// 无法确认 USB 2.0/3.0 时明确标为“速率未知”，避免猜测。
std::string detectInterfaceType(const std::string& deviceModel,
                                const std::string& deviceArgs,
                                const std::string& deviceInfo)
{
    const std::string text = lowerCopy(
        deviceModel + " " + deviceArgs + " " + deviceInfo);

    if (text.find("usb 3") != std::string::npos
        || text.find("usb3") != std::string::npos) {
        return "USB 3.0";
    }
    if (text.find("usb 2") != std::string::npos
        || text.find("usb2") != std::string::npos) {
        return "USB 2.0";
    }
    if (text.find("resource=") != std::string::npos
        || text.find("rio") != std::string::npos
        || text.find("pcie") != std::string::npos) {
        return "PCIe";
    }
    if (text.find("addr=") != std::string::npos
        || text.find("mgmt_addr=") != std::string::npos
        || text.find("ethernet") != std::string::npos
        || text.find("x300") != std::string::npos
        || text.find("x310") != std::string::npos) {
        return "Ethernet";
    }
    if (text.find("b200") != std::string::npos
        || text.find("b205") != std::string::npos
        || text.find("b210") != std::string::npos) {
        return "USB（速率未知）";
    }
    return "未知";
}

void fillConnectionDetails(DeviceConnectionResult& result,
                           const std::string& connectionArgs,
                           const std::string& deviceOptions)
{
    result.connectionArgsUsed = connectionArgs.empty()
        ? "默认（未指定）" : connectionArgs;
    result.deviceOptionsUsed = deviceOptions.empty()
        ? "默认（未指定）" : deviceOptions;
    result.interfaceType = detectInterfaceType(
        result.deviceModel, connectionArgs, result.deviceInfo);
}

std::string mergeDeviceArguments(const std::string& connectionArgs,
                                 const std::string& deviceOptions)
{
    if (connectionArgs.empty()) return deviceOptions;
    if (deviceOptions.empty()) return connectionArgs;
    return connectionArgs + "," + deviceOptions;
}

} // namespace

DeviceDiscoveryResult SdrDevice::findDevices(const std::string& deviceArgs) const
{
    DeviceDiscoveryResult result;

    try {
        const uhd::device_addr_t hint(deviceArgs);
        const uhd::device_addrs_t foundDevices = uhd::device::find(hint);

        result.devices.reserve(foundDevices.size());
        for (const auto& device : foundDevices) {
            result.devices.push_back(device.to_string());
        }
    } catch (const std::exception& exception) {
        result.errorMessage = exception.what();
    }

    return result;
}

DeviceConnectionResult SdrDevice::connectDevice(
    const std::string& connectionArgs,
    const std::string& deviceOptions)
{
    DeviceConnectionResult result;

    try {
        if (usrp_) {
            result.success = true;
            result.deviceModel = usrp_->get_mboard_name(0);
            result.deviceInfo = usrp_->get_pp_string();
            fillConnectionDetails(result, connectionArgs, deviceOptions);
            return result;
        }

        // 连接参数负责选中设备；设备参数负责 UHD 初始化选项。最终按 UHD
        // device args 语法合并，例如 serial=... 与 master_clock_rate=...。
        usrp_ = uhd::usrp::multi_usrp::make(
            mergeDeviceArguments(connectionArgs, deviceOptions));

        result.success = true;
        result.deviceModel = usrp_->get_mboard_name(0);
        result.deviceInfo = usrp_->get_pp_string();
        fillConnectionDetails(result, connectionArgs, deviceOptions);
    } catch (const std::exception& exception) {
        usrp_.reset();
        result.errorMessage = exception.what();
    }

    return result;
}

void SdrDevice::disconnectDevice()
{
    usrp_.reset();
}

bool SdrDevice::isConnected() const
{
    return static_cast<bool>(usrp_);
}

std::shared_ptr<uhd::usrp::multi_usrp> SdrDevice::usrp() const
{
    return usrp_;
}
