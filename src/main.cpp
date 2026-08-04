#include <uhd/types/tune_request.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/stream.hpp>

#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <algorithm>
#include <complex>
#include <stdexcept>
#include <vector>

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
    constexpr double capture_duration = 1.0; // 采集1秒

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
        
        // 配置接收数据流的样本格式
        uhd::stream_args_t stream_args("fc32", "sc16");
        stream_args.channels = {channel};

        // 创建RX数据流对象，此时还没有开始接收样本
        auto rx_stream = usrp->get_rx_stream(stream_args);

        std::cout << "RX streamer created successfully.\n";
        std::cout << "Maximum samples per receive call: "
          << rx_stream->get_max_num_samps()
          << '\n';

        // 给硬件一点时间完成调谐
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 每次recv()调用使用的临时接收缓冲区
        const std::size_t buffer_size = rx_stream->get_max_num_samps();
        std::vector<std::complex<float>> receive_buffer(buffer_size);

        // 根据采样率和采集时长计算目标样本数
        const std::size_t target_samples =
            static_cast<std::size_t>(sample_rate * capture_duration);

        std::size_t total_received = 0;
        std::size_t overflow_count = 0;



        // 启动连续接收
        uhd::stream_cmd_t start_command(
            uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS
        );
        start_command.stream_now = true;
        rx_stream->issue_stream_cmd(start_command);

        std::cout << "\nReceiving IQ samples..." << std::endl;

        const auto capture_start = std::chrono::steady_clock::now();

        try
        {
            while (total_received < target_samples)
            {
                const std::size_t samples_to_receive = std::min(
                    buffer_size,
                    target_samples - total_received
                );

                uhd::rx_metadata_t metadata;

                const std::size_t received = rx_stream->recv(
                    receive_buffer.data(),
                    samples_to_receive,
                    metadata,
                    3.0,
                    false
                );

                if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_NONE)
                {
                    total_received += received;
                    continue;
                }

                if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
                {
                    ++overflow_count;
                    total_received += received;

                    std::cerr << "Warning: RX overflow detected." << std::endl;
                    continue;
                }

                if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT)
                {
                    throw std::runtime_error(
                        "RX timeout: no samples received."
                    );
                }

                throw std::runtime_error(
                    "RX metadata error: " + metadata.strerror()
                );
            }
        }
        catch (...)
        {
            // 即使接收发生异常，也先通知设备停止发送数据
            uhd::stream_cmd_t stop_command(
                uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS
            );
            rx_stream->issue_stream_cmd(stop_command);

            throw; // 将原来的异常继续交给外层catch处理
        }

        // 正常完成时停止连续接收
        uhd::stream_cmd_t stop_command(
            uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS
        );
        rx_stream->issue_stream_cmd(stop_command);




        
        const auto capture_end = std::chrono::steady_clock::now();

        const double elapsed_seconds =
            std::chrono::duration<double>(
                capture_end - capture_start
            ).count();

        std::cout << "IQ reception completed.\n";
        std::cout << "Target samples   : " << target_samples << '\n';
        std::cout << "Received samples : " << total_received << '\n';
        std::cout << "Elapsed time     : " << elapsed_seconds << " s\n";
        std::cout << "Average rate     : "
                << total_received / elapsed_seconds / 1e6
                << " MS/s\n";
        std::cout << "Overflow count   : " << overflow_count << '\n';



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