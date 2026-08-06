# LuoWave Signal Capture 当前功能与代码逻辑说明

本文档对应本交付包中的源码版本，以用户提供的 `signal_capture_app(6).zip` 为唯一基线，说明新增功能、线程模型、数据格式、主要调用链和使用方法。

## 1. 当前已实现功能

### 1.1 设备发现、选择与连接

- “设备连接状态”区域不再显示设备温度。
- 删除原“指定参数”手工输入框及“已用参数”显示项。
- 增加“选择设备”下拉框。
- 左侧设备状态区宽度调整为 340 px，下拉弹出列表至少 560 px；每个设备项还提供完整型号、序列号和 UHD args 悬停提示，便于区分多台 B210、X310 或其他设备。
- 点击“查找设备”后，`uhd::device::find()` 在后台任务中运行，不阻塞 GUI。
- 查找结果以“产品型号 + 序列号 + 地址/资源名”的形式加入下拉框。
- 每个下拉项内部保存 UHD 返回的完整 device args。用户选择设备并点击“连接设备”后，程序把该 args 传给 `multi_usrp::make()`，实现原来手工填写 `serial=...`、`addr=...` 或 `resource=RIO0` 的选机效果。
- “设备参数”输入框仍保留，用于 `master_clock_rate=...`、`clock_source=external` 等 UHD 初始化选项；它与下拉框中的选机参数在底层合并。
- 设备查找和设备连接分别使用 `QtConcurrent`，操作期间按钮自动禁用，完成后恢复。

### 1.2 连续采集与定长采集

- 连续采集使用 `STREAM_MODE_START_CONTINUOUS`，直到用户点击“停止采集”。
- 定长采集使用 `STREAM_MODE_NUM_SAMPS_AND_DONE`。
- 定长目标样本数按 UHD 实际采样率计算：

  `目标样本数 = round(采集时长 × 实际采样率)`

- 接收循环会限制最后一次 `recv()` 的请求长度，避免超过目标样本数。
- 用户可提前停止定长采集；此时程序发送停止流命令并安全结束。
- 定长采集或保存 IQ 时，中心频率、采样率、带宽和增益在任务期间锁定，保证时长、文件名和文件内容的含义一致。
- 不保存 IQ 的连续采集仍允许在线调整射频参数和频谱显示参数。

### 1.3 IQ 数据保存

- 勾选“保存复数 IQ 数据”后，点击“开始接收”会弹出保存对话框。
- 对话框既可选择目录，也可修改文件名；程序自动给出默认文件名，例如：

  `IQ_F100.000000MHz_SR5.000MSps_20260806_143025_fc32.bin`

- 默认文件名包含中心频率、采样率、时间戳和主机样本格式。
- 定长保存开始前会估算所需空间；空间不足时不会启动采集。
- `.bin` 文件是无文件头的二进制 `fc32` 数据：每个复数样本依次保存一个 32 位浮点 I 和一个 32 位浮点 Q，共 8 字节，采用运行主机的本机字节序。扩展名为通用裸二进制数据标识，不改变样本布局。
- 读取示例：

  - Python/NumPy：`np.fromfile(path, dtype=np.complex64)`
  - GNU Radio：使用 `File Source`，Output Type 选择 `Complex`（complex float32），文件头选择 `No`。
  - MATLAB：`fid=fopen(path,'rb'); x=fread(fid,Inf,'float32=>single'); fclose(fid); iq=complex(x(1:2:end),x(2:2:end));`

- 文件写入由独立 `std::thread` 完成。UHD 接收线程只复制数据到有界队列，不执行磁盘写操作。
- 写入队列上限为 64 MiB。如果磁盘持续低于接收速率，程序停止采集并报错，不会静默丢弃 IQ 块或生成看似正常但中间缺样本的文件。
- 任务结束时先排空写入队列，再关闭文件，并在日志和元数据区域显示实际写入样本数和字节数。

### 1.4 磁盘剩余容量

- 状态栏右下角显示当前保存位置所在磁盘的可用容量。
- 未选择 IQ 保存位置时显示用户主目录所在磁盘。
- 选择保存位置后自动切换到该位置，并每秒刷新一次。
- 鼠标悬停可查看当前用于容量查询的目录。

### 1.5 元数据显示

右侧“元数据显示”由只读双列表格展示，关闭编辑、选中和网格线，显示：

1. 主机更新时间；
2. UHD 设备时间戳；
3. 接收包数；
4. 样本总数；
5. UHD 当前状态；
6. 溢出次数；
7. 超时次数；
8. 接收通道；
9. 主机/链路数据格式；
10. IQ 保存状态。

接收线程每 250 ms 最多发送一次统计和元数据快照。GUI 只更新文本，不查询 UHD、不进行 FFT、不写文件，因此元数据显示不会阻塞接收线程。

### 1.6 原有功能保持

以下基线功能保持不变：

- 中心频率、采样率、前端带宽、增益、通道和天线设置；
- 实时频谱、瀑布图和时域波形；
- FFT 点数、窗函数、平均、最大保持、最小保持、当前迹线和输入补偿；
- 多 Marker、峰值搜索、下一峰值和 Marker 跟踪；
- 显示区域截图；
- 日志控制台；
- CPU 占用和实际显示帧率统计；
- 设备发现/连接期间的异步操作和按钮状态管理。

## 2. 线程模型

程序包含四类执行上下文：

| 执行上下文 | 主要职责 | 不执行的工作 |
|---|---|---|
| GUI 主线程 | 控件响应、图形刷新、日志、元数据文本、状态栏 | 不调用阻塞式设备发现/连接，不接收 UHD 数据，不写 IQ 文件 |
| QtConcurrent 任务 | UHD 设备发现、设备连接 | 不访问 GUI 控件 |
| `RxWorker` 接收线程 | UHD 参数配置、`recv()`、统计、频谱处理、生成低频率显示帧 | 不直接更新 GUI，不执行磁盘写入 |
| `IqFileWriter` 写入线程 | 顺序写入 fc32 IQ 数据、排空队列、关闭文件 | 不调用 UHD，不访问 GUI |

跨线程通信原则：

- 接收线程通过 Qt signals 向 GUI 提交显示帧、统计和元数据快照。
- GUI 对运行参数的修改先写入互斥锁保护的配置副本，再由接收线程应用 UHD 参数。
- 停止操作只写原子变量，`recv()` 使用 100 ms 短超时，因此停止按钮能及时生效。
- 定长模式、保存开关和 IQ 路径属于任务级不可变参数，不会被运行时显示配置覆盖。

## 3. 主要文件与职责

| 文件 | 职责 |
|---|---|
| `ui/main_window.ui` | 界面布局、设备下拉框、采集模式和右侧信息区 |
| `include/ui/main_window.hpp` / `src/ui/main_window.cpp` | GUI 状态机、异步发现/连接、保存对话框、元数据和磁盘容量显示 |
| `include/sdr_device.hpp` / `src/sdr_device.cpp` | UHD 设备发现、下拉项描述、连接参数合并、连接/断开 |
| `include/rx_config.hpp` | 一次接收任务的完整配置快照 |
| `include/rx_receiver.hpp` / `src/rx_worker.cpp` | 连续/定长 UHD 流、统计、元数据、频谱帧和 IQ 入队 |
| `include/iq_file_writer.hpp` / `src/iq_file_writer.cpp` | 有界队列和独立 IQ 文件写入线程 |
| `include/spectrum_processor.hpp` / `src/spectrum_processor.cpp` | FFT、窗函数、平均和保持处理 |
| `include/signal_plot_widgets.hpp` / `src/signal_plot_widgets.cpp` | 频谱、瀑布、时域和 Marker 绘制 |
| `CMakeLists.txt` | Qt/UHD/Boost 目标和新增写入模块编译配置 |

## 4. 关键调用流程

### 4.1 查找并连接设备

1. `MainWindow::findDevices()` 禁用相关按钮并启动 `QtConcurrent::run()`。
2. `SdrDevice::findDevices()` 调用 `uhd::device::find()`。
3. 完成回调清空并重建 `deviceSelectComboBox`；显示文本和完整 args 分开保存。
4. `MainWindow::connectDevice()` 读取当前下拉项 data，与“设备参数”合并后启动后台连接任务。
5. `SdrDevice::connectDevice()` 调用 `multi_usrp::make()`，返回型号、接口类型和设备信息。

### 4.2 开始采集

1. `currentRxConfig()` 对 GUI 参数做一次快照。
2. 若保存 IQ，GUI 选择路径、检查定长预计空间并写入 `config.iqFilePath`。
3. 创建 `QThread` 和 `RxWorker`，把 worker 移入接收线程。
4. Worker 配置 UHD，按模式发出连续或定长 stream command。
5. 接收循环统计元数据、生成约 50 FPS 的显示帧；元数据最多 4 Hz。
6. 若保存 IQ，接收块进入 `IqFileWriter` 队列，由独立线程顺序落盘。
7. 定长达到目标、用户停止或错误发生后停止流，排空文件队列并结束线程。

## 5. 编译方法

在已安装 Qt 6.11.1、UHD 4.9.0.0、Boost 1.85 和 MSVC 的 Windows 开发环境中执行：

```powershell
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
.\build-release\signal_capture_app.exe
```

如果 Qt Creator 已配置“MSVC 2026 64-bit”套件，也可直接重新运行 CMake 后构建。

## 6. 运行注意事项

- 高采样率保存对磁盘持续写入速度要求很高：理论写入量约为 `采样率 × 8 字节/秒`。例如 100 MSps 的 fc32 数据约为 762.9 MiB/s。
- `.bin` 为 `float32 I, float32 Q` 交错排列的原始数据文件，不含频率和采样率文件头；GNU Radio 与 MATLAB 均可直接按上述格式读取。频率和采样率信息位于默认文件名和程序日志中。若后续需要长期归档，建议增加同名 JSON sidecar 元数据文件。
- UHD `OVERFLOW` 表示主机未及时取走设备数据。程序会计数并警告；一旦硬件已经溢出，接收内容本身可能不连续。
- 64 MiB 写入队列用于吸收短时磁盘抖动，不应被视为可替代高速 SSD 的长期缓存。
- 本交付环境不包含 Windows Qt/UHD/MSVC 工具链，因此完成了 XML 解析、UI 对象引用、源码一致性和打包检查；最终硬件编译与 B210/X310 实机接收需在你的 Windows 开发机验证。
