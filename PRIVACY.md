# Privacy

ImmersiveTopTaskbar 是本地 Windows 托盘工具。默认不收集、上传、出售或同步个人数据。

## 本地诊断日志

程序会写入本地诊断日志：

```text
%LOCALAPPDATA%\ImmersiveTopTaskbar\log.txt
```

日志可能包含窗口标题、窗口类名、窗口句柄、显示器矩形、采样颜色、时间信息和本地 TranslucentTB 路径。公开分享日志前请先检查和脱敏。

## 网络访问

程序启动后会按源码设定访问配置的 GitHub Releases API，用于检查最新版本。该逻辑只比较版本并提示用户，不会自动下载或执行更新。

## 系统状态

程序可能临时修改当前用户的 Windows 壳层主题注册表值，并临时同步 TranslucentTB 的最大化窗口外观。正常退出时程序会尝试恢复原始状态。
