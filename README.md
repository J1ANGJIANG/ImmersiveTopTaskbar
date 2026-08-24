# ImmersiveTopTaskbar

ImmersiveTopTaskbar 是一个 Windows 11 x64 托盘工具，用于顶部任务栏沉浸显示。当前前台窗口最大化时，它会根据窗口状态和采样颜色调整真实任务栏外观，让顶部任务栏更贴近当前窗口。

请先阅读 [PRIVACY.md](PRIVACY.md) 和 [SECURITY.md](SECURITY.md)，尤其是注册表、TranslucentTB 配置和本地日志相关说明。

## 安装须知

- 安装器默认安装到 `D:\ImmersiveTopTaskbar`。
- 安装器默认勾选“开机自动启动”。
- 安装完成后可选择立即运行。
- 程序不要求管理员权限。
- 程序依赖 TranslucentTB，并通过其 ExplorerTAP 组件应用任务栏外观。

这些是当前发行设定，开源版没有改动。

## 启动检测

程序启动时会进行运行环境检测，包括 TranslucentTB / ExplorerTAP 可用性、任务栏位置和相关外观状态。检测通过后会尽量减少重复提示。

启动后也会按当前源码设定异步检查 GitHub Releases 最新版本。该检查只访问 GitHub API、比较版本并弹出提示，不会自动下载或执行更新。

当前更新源为：

```cpp
constexpr auto kUpdateOwner = L"J1ANGJIANG";
constexpr auto kUpdateRepo = L"ImmersiveTopTaskbar";
```

## 运行

```bat
build\ImmersiveTopTaskbar.exe
```

托盘菜单可用于检查更新、查看关于信息和退出。

停止正在运行的实例：

```bat
build\ImmersiveTopTaskbar.exe --quit
```

异常退出后恢复 TranslucentTB 管理的外观：

```bat
build\ImmersiveTopTaskbar.exe --restore-ttb
```

异常退出后恢复 Windows 壳层主题：

```bat
build\ImmersiveTopTaskbar.exe --restore-shell-theme
```

## 构建

```bat
build.cmd
```

需要 MSVC Build Tools 或 Visual Studio，并安装 C++ 桌面开发组件。

## 本地日志

诊断日志写入：

```text
%LOCALAPPDATA%\ImmersiveTopTaskbar\log.txt
```

日志可能包含窗口标题、窗口类名、坐标、颜色、TranslucentTB 路径和时间信息。公开提交 issue 前请先检查和脱敏。
