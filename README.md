# ttp_clienc

命令行编码适配器的独立源码实现，兼容原版 TTPlayer 编码器工厂及重建版宿主。
本 DLL 调用用户配置的外部编码器，不包含 FFmpeg、Nero、QAAC 等编码器程序。

## 构建与分发

```powershell
./build.ps1 -Package -PackageVersion 2026.10.19
```

需要现代 MSVC x86、CMake、Python 和 PowerShell。构建下载固定版本的 TinyXML-2 11.0.0、
VC-LTL 5.3.1 和 YY-Thunks 1.2.2；源码与下载内容仅存于 build，许可证留在仓库。
Release ZIP 仅包含 `AddIn/ttp_clienc.dll` 和 `SHA256SUMS.txt`。Action 仅手动触发，
默认构建打包；勾选 Release a Version 后发布，版本按北京时间日期及 pN 分配。
Action 不执行、上传私有测试。

## 兼容行为与修正

- 保留 DLL 旁 `ttp_clienc.xml` 的预设、当前索引、程序、参数、扩展名、位深和标签类型。
- `%s` 采用完整临时 WAV；没有 `%s` 时向编码器 stdin 传送 WAV。`%d` 是输出路径。
- `bps=8/16/24/32` 均为整数 PCM，`bps=0` 保留输入精度。原版标准输入为 64-bit float；
  另外接受 8/16/24/32-bit PCM、32/64-bit float 和相应 WAVEFORMATEXTENSIBLE。
- 保留外部程序相对宿主目录的路径解析，支持 Unicode 路径；不通过命令解释器启动程序。
- 不再修改整个播放器进程的 stdin。独立异步管道及子进程退出检查，避免失败仍报告成功。
  即使 Job 分配失败、孙进程继续持有 stdin，仍可在发起线程取消写入，不等待同步写线程退出。
- 先写临时输出，成功且标签处理完成后替换目标。直接路径接口失败保留现有目标；
  stream 接口的宿主可能在调用 DLL 前已清空目标，此时无法由 DLL 追回旧内容。
- 释放未完成会话回收子进程及临时文件；重复 Finish 返回原结果，不重新启动编码器。

## 已知边界

- 外部编码器自己必须兼容目标系统；本 DLL 的 XP 导入表通过不等于外部程序可在 XP 运行。
- WAV 超过 RIFF 4 GiB 上限明确返回错误；没有自行给旧编码器改为 RF64。
- 管道阻塞 30 秒、最终退出等待 5 分钟后返回超时。标准输出/错误写临时日志以避免阻塞；
  尚未提供日志查看 UI。
- 配置窗口保留功能字段，但未宣称原窗口的像素级一致。当前未恢复拖放预设重排。
- 旧宿主的标签最终写盘发生于 Metadata Release，旧接口不能返回该阶段的真实失败状态。
- 本地测试与原版 PCM 对照见 [验证记录](docs/VALIDATION_20261019.md)。

源码许可证 [MIT](LICENSE)；[TinyXML-2 许可证](third_party/tinyxml2/LICENSE.txt)。
