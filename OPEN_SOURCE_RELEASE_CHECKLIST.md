# Open Source Release Checklist

推送到 GitHub 前确认：

- `.claude/`、`.reasonix/`、`.workbuddy/` 没有进入仓库。
- `build/`、`dist/`、`runtime_log.txt`、`CURRENT_HANDOFF_REPORT.md` 没有进入仓库。
- `.pdb`、`.ilk`、`.obj`、`.exe`、`.dmp`、截图和临时探针输出没有被暂存。
- `src/main.cpp` 中的 `kUpdateOwner` / `kUpdateRepo` 已指向 `J1ANGJIANG/ImmersiveTopTaskbar`。
- README 已说明默认 D 盘安装、默认开机启动、启动检测、更新检查、本地日志和系统主题修改。
- 已确认 MIT License 是你想采用的开源许可证。
