#pragma once

#include <QString>

#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

// 独立磁盘线程：接收线程只复制一块 IQ 数据并入队，不执行文件 I/O。
class IqFileWriter final
{
public:
    IqFileWriter() = default;
    ~IqFileWriter();

    IqFileWriter(const IqFileWriter&) = delete;
    IqFileWriter& operator=(const IqFileWriter&) = delete;

    bool open(const QString& filePath, QString& errorMessage);
    bool enqueue(const std::complex<float>* samples, std::size_t count,
                 QString& errorMessage);
    bool close(QString& errorMessage);

    std::uint64_t samplesWritten() const;
    std::uint64_t bytesWritten() const;

private:
    void writeLoop();

    static constexpr std::size_t maxQueuedBytes_ = 64U * 1024U * 1024U;
    std::ofstream stream_;
    std::thread thread_;
    std::deque<std::vector<std::complex<float>>> queue_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t queuedBytes_ = 0;
    std::uint64_t samplesWritten_ = 0;
    bool stopping_ = false;
    bool failed_ = false;
    QString errorMessage_;
};
