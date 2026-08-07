#include "iq_file_writer.hpp"

#include <QCoreApplication>

#include <filesystem>
#include <utility>

IqFileWriter::~IqFileWriter()
{
    QString ignored;
    close(ignored);
}

bool IqFileWriter::open(const QString& filePath, QString& errorMessage)
{
#ifdef _WIN32
    const std::filesystem::path path(filePath.toStdWString());
#else
    const std::filesystem::path path =
        std::filesystem::u8path(filePath.toUtf8().constData());
#endif
    stream_.open(path, std::ios::binary | std::ios::trunc);
    if (!stream_) {
        errorMessage = QCoreApplication::translate(
            "IqFileWriter", "无法创建 IQ 文件：%1").arg(filePath);
        return false;
    }
    thread_ = std::thread(&IqFileWriter::writeLoop, this);
    return true;
}

bool IqFileWriter::enqueue(const std::complex<float>* samples,
                           std::size_t count, QString& errorMessage)
{
    if (count == 0) return true;
    const std::size_t bytes = count * sizeof(std::complex<float>);
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_ || stopping_) {
        errorMessage = errorMessage_;
        return false;
    }
    if (queuedBytes_ + bytes > maxQueuedBytes_) {
        failed_ = true;
        stopping_ = true;
        errorMessage_ = QCoreApplication::translate(
            "IqFileWriter",
            "IQ 磁盘写入速度低于接收速度，缓存已达到 64 MiB；为避免生成不连续文件，采集已停止。");
        errorMessage = errorMessage_;
        condition_.notify_one();
        return false;
    }
    queue_.emplace_back(samples, samples + count);
    queuedBytes_ += bytes;
    condition_.notify_one();
    return true;
}

bool IqFileWriter::close(QString& errorMessage)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_one();
    if (thread_.joinable()) thread_.join();
    if (stream_.is_open()) stream_.close();

    std::lock_guard<std::mutex> lock(mutex_);
    errorMessage = errorMessage_;
    return !failed_;
}

std::uint64_t IqFileWriter::samplesWritten() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return samplesWritten_;
}

std::uint64_t IqFileWriter::bytesWritten() const
{
    return samplesWritten() * sizeof(std::complex<float>);
}

void IqFileWriter::writeLoop()
{
    while (true) {
        std::vector<std::complex<float>> block;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_) break;
                continue;
            }
            block = std::move(queue_.front());
            queue_.pop_front();
            queuedBytes_ -= block.size() * sizeof(std::complex<float>);
        }

        stream_.write(reinterpret_cast<const char*>(block.data()),
                      static_cast<std::streamsize>(
                          block.size() * sizeof(std::complex<float>)));
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stream_) {
            failed_ = true;
            stopping_ = true;
            errorMessage_ = QCoreApplication::translate(
                "IqFileWriter", "写入 IQ 文件失败，请检查磁盘空间和目录权限。");
            queue_.clear();
            queuedBytes_ = 0;
        } else {
            samplesWritten_ += block.size();
        }
    }
}
