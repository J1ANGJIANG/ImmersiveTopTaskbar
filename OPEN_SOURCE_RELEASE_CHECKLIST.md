# Open Source Release Checklist

推送到 GitHub 前确认：

- `.claude/`、`.reasonix/`、`.workbuddy/` 没有进入仓库。
- `build/`、`dist/`、`runtime_log.txt`、`CURRENT_HANDOFF_REPORT.md` 没有进入仓库。
- `.pdb`、`.ilk`、`.obj`、`.exe`、`.dmp`、截图和临时探针输出没有被暂存。
- `src/main.cpp` 中的 `kUpdateOwner` / `kUpdateRepo` 已指向 `J1ANGJIANG/ImmersiveTopTaskbar`。
- README 已说明默认 D 盘安装、默认开机启动、启动检测、更新检查、本地日志和系统主题修改。
- README 使用脱敏示意截图，不包含真实桌面、账号、窗口标题或托盘信息。
- `donate/` 中的二维码是有意公开的捐赠素材；安装器不会把它们作为普通图片文件复制到用户安装目录。
- Release 中需要上传 `ImmersiveTopTaskbar-Setup-1.1.0.exe`，否则自动下载安装入口会回退到 GitHub 手动安装页。
- 已确认 MIT License 是你想采用的开源许可证。
