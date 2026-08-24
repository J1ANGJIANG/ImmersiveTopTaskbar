# Privacy

ImmersiveTopTaskbar 是本地 Windows 托盘工具。默认不收集、上传、出售或同步个人数据。

## 本地诊断日志

程序会写入本地诊断日志：

```text
%LOCALAPPDATA%\ImmersiveTopTaskbar\log.txt
```

日志可能包含窗口标题、窗口类名、窗口句柄、显示器矩形、采样颜色、时间信息和本地 TranslucentTB 路径。公开分享日志前请先检查和脱敏。

## 网络访问

程序启动后会访问配置的 GitHub Releases API，用于检查最新版本。

用户在“检查更新”页面选择自动下载安装时，程序会下载 GitHub Release 中的安装包附件到：

```text
%LOCALAPPDATA%\ImmersiveTopTaskbar\Updates
```

下载完成后，程序会再次询问是否启动安装器；不会静默安装。

## 意见反馈

托盘菜单的“意见反馈”入口会调用系统默认邮件客户端，并预填收件人、标题和反馈正文。程序不会内置邮箱密码或邮件服务 API Key，也不会绕过用户确认静默发送邮件。

如果当前构建没有配置反馈邮箱，程序会改为打开 GitHub Issues 页面作为备用反馈入口。

## 捐赠二维码

捐赠二维码嵌入在程序资源中，通过“检查更新”页面的“支持作者”按钮显示。安装器不会把二维码作为普通图片文件复制到用户安装目录。

## 系统状态

程序可能临时修改当前用户的 Windows 壳层主题注册表值，并临时同步 TranslucentTB 的最大化窗口外观。正常退出时程序会尝试恢复原始状态。
