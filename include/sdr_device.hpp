#pragma once

#include <string>
#include <vector>
#include <memory>

struct DiscoveredDevice
{
    std::string displayName;
    std::string connectionArgs;
};

struct DeviceDiscoveryResult
{
    std::vector<DiscoveredDevice> devices;
    std::string errorMessage;
};

struct DeviceConnectionResult
{
    bool success = false;
    std::string deviceModel;
    std::string interfaceType;
    std::string connectionArgsUsed;
    std::string deviceOptionsUsed;
    std::string deviceInfo;
    std::string errorMessage;
};

namespace uhd::usrp {
class multi_usrp;
}

class SdrDevice
{
public:
    DeviceDiscoveryResult findDevices(const std::string& deviceArgs = {}) const;
    DeviceConnectionResult connectDevice(
        const std::string& connectionArgs = {},
        const std::string& deviceOptions = {});

    void disconnectDevice();

    bool isConnected() const;

    // 返回同一个 UHD 设备的共享指针，不会复制或重新连接硬件。
    std::shared_ptr<uhd::usrp::multi_usrp> usrp() const;

private:
    std::shared_ptr<uhd::usrp::multi_usrp> usrp_;
};
