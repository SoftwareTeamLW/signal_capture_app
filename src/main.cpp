#include <uhd/types/tune_request.hpp>
#include <uhd/usrp/multi_usrp.hpp>

#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

int main()
{
    // 设备选择参数
    const std::string device_args = "type=b200";

    // 本次控制 B210 的目标参数
    constexpr std::size_t channel = 0;
    constexpr double sample_rate = 5e6;       // 5 MS/s
    constexpr double center_frequency = 100e6; // 100 MHz
    constexpr double gain = 20.0;             // 20 dB
    constexpr double bandwidth = 5e6;         // 5 MHz

    try
    {
        std::cout << "Searching for B210..." << std::endl;

        // 创建USRP控制对象并连接设备
        auto usrp = uhd::usrp::multi_usrp::make(device_args);

        std::cout << "\nDevice connected successfully.\n";
        std::cout << usrp->get_pp_string() << std::endl;

        // B210使用板载参考时钟
        usrp->set_clock_source("internal");

        // 配置接收通道0
        usrp->set_rx_rate(sample_rate, channel);
        usrp->set_rx_freq(
            uhd::tune_request_t(center_frequency),
            channel
        );
        usrp->set_rx_gain(gain, channel);
        usrp->set_rx_bandwidth(bandwidth, channel);

        // 给硬件一点时间完成调谐
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        std::cout << std::fixed << std::setprecision(3);
        std::cout << "\nRX channel " << channel
                  << " configuration:\n";

        std::cout << "Sample rate : "
                  << usrp->get_rx_rate(channel) / 1e6
                  << " MS/s\n";

        std::cout << "Frequency   : "
                  << usrp->get_rx_freq(channel) / 1e6
                  << " MHz\n";

        std::cout << "Gain        : "
                  << usrp->get_rx_gain(channel)
                  << " dB\n";

        std::cout << "Bandwidth   : "
                  << usrp->get_rx_bandwidth(channel) / 1e6
                  << " MHz\n";

        std::cout << "\nB210 configuration completed." << std::endl;

        return 0;
    }
    catch (const uhd::exception& error)
    {
        std::cerr << "\nUHD error:\n"
                  << error.what() << std::endl;
        return 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << "\nProgram error:\n"
                  << error.what() << std::endl;
        return 1;
    }
}