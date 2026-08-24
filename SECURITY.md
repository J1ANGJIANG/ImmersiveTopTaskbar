# Security Policy

## 支持版本

只支持最新发布版本。

## 报告漏洞

请优先使用 GitHub Security Advisories 报告安全问题。不要在公开 issue 中直接粘贴未经脱敏的本地日志。

## 安全相关行为

ImmersiveTopTaskbar:

- 使用 TranslucentTB 的 ExplorerTAP 集成应用任务栏外观；
- 可能临时写入当前用户的 Windows 壳层主题注册表值；
- 可能临时修改 TranslucentTB 的最大化窗口外观；
- 会在 `%LOCALAPPDATA%\ImmersiveTopTaskbar` 写入本地诊断日志；
- 安装器默认安装到 `D:\ImmersiveTopTaskbar`；
- 安装器默认勾选开机自动启动；
- 启动后会检查配置的 GitHub Releases 最新版本；
- 用户主动选择时，可从 GitHub Release 下载新版安装包并启动安装器；
- 捐赠二维码嵌入在程序资源中，不会作为普通图片文件安装到用户目录。

程序不需要管理员权限，不会静默安装更新。
