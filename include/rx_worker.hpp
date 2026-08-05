#pragma once

#include <QObject>
#include <atomic>
#include <uhd/usrp/multi_usrp.hpp>

class RxWorker final : public QObject
{
    Q_OBJECT

public:
    explicit RxWorker(
        uhd::usrp::multi_usrp::sptr usrp,
        QObject* parent = nullptr);
    // 这个函数只修改原子变量，可以由主线程直接调用。
    void requestStop();

public slots:
    void startReceiving();

signals:
    void receptionStarted();
    void receptionStopped();
    void errorOccurred(const QString& message);
    void configurationCompleted(const QString& message);

private:
    std::atomic_bool stopRequested_{false};
    uhd::usrp::multi_usrp::sptr usrp_;
};