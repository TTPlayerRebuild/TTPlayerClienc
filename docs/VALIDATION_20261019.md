# CLI 编码适配器恢复与验证，2026-10-19

## 原版证据

原文件 `AddIn/ttp_clienc.dll` SHA-256：
`685db2973b9dd56cfa58bc0f5ee047d0c96667c1a9d96e0c1aa99a7e360e9db3`。

REA/Ghidra 的 `6020142E` 恢复 `%s` 临时 WAV / stdin 管道启动分支、`%d` 替换、
原程序全局 `SetStdHandle`；`6020172A` 在临时文件分支启动程序并无限等待，之后处理标签。
`6020115F` 的 QI 支持 IUnknown、Encoder、Metadata，不支持 stream encoder 扩展。
原 `60201358` 对输入位深检查 64，32-bit float 输入返回 E_NOTIMPL。

动态验证更正了一个仅看文字会产生的误判：XML `bps=32` 输出 **整数 32-bit PCM**；
`bps=0` 在原版 64-bit float 输入下保留 64-bit float。以实际 WAV 数据及原代码为准。
原版再次 Finish 会重新运行临时文件编码分支；新实现将 Finish 结果锁存为幂等操作。

## 测试

测试、原始证据和生成样本全部位于私有 `rebuild/tests/audio_plugins_fix_20261019/clienc`。
无用户音乐被修改，原 DLL 未覆盖。

- Win11：原版 10 个转换组合、新版 10 个对应组合（临时 WAV / 管道 × 0/8/16/24/32）。
- 逐字节比较各 WAV 的 data 块：**10/10 PCM 完全相同**。
- 原版 source-float 临时 WAV 的 fmt 块为 18 字节，新版为合法 16 字节；此容器头差异不影响 PCM。
- 新版另有 8 个案例：两条路径分别验证 named stream 接口、Unicode/空格路径、非零退出码、
  取消不读取 stdin 的子进程。失败/取消均保留直接路径接口原目标内容。
- 新版全部成功会话重复 Finish 返回原结果；所有案例验证父进程 stdin 未改变。
- 取消边界追加 2 个案例：子进程退出、孙进程继续持有 stdin；分别允许及强制拒绝 Job 分配。
  两种情况均约 0.3 秒返回 STG_E_WRITEFAULT，重复 Finish 不重试，原目标未修改。
  强制拒绝 Job 的孙进程确实仍活着，证明返回不依赖其退出；测试随后单独清理该进程。
  生产实现改用同调用线程的 overlapped named pipe 与 XP 可用的 CancelIo，移除了阻塞写线程和无限 join。
- Release x86 构建通过 XP/Win7 导入审计：6 个 DLL，123 个导入，无 VC 2012 依赖。
- XP 与 Win7 各完成一次 64-bit float → 16-bit PCM 管道转换；各调用结果均为 0，
  父进程 stdin 未改变。两边输出 SHA-256 与 Win11 同为
  `cee3baa4d7a05cd7d839b87b68d4dba8a2ffd4e3cf4611aa1d4b1d045684bc9e`。

最终异步管道 DLL（SHA-256 `80911b4e11c334ecc5762192c0f1f62300aa023e87dd67735116b06a34e1a8ba`）
也已在 XP/Win7 复测基础转换和强制 Job 失败的孙进程持管道案例：两系统均正确返回，
`stdin_preserved=true`，故障案例 `write/finish/repeat=2147680285`、`descendant_alive=1`。
测试尚未覆盖原播放器完整转换 UI、所有现有外部编码器、XP/Win7 完整故障矩阵、
真实宿主标签持久化或 4 GiB 输入。这些边界不能由本轮子进程替身测试替代。

## 构建依赖

TinyXML-2 11.0.0，官方版本：https://github.com/leethomason/tinyxml2/releases/tag/11.0.0 。
源码 ZIP SHA-256：`1c35b8d8a586125d5e43380fd2137abbfde645183ed1641f86c815359320412f`。
版本固定，不使用系统 XML 或旧 ttpcomm 私有 XML ABI。
