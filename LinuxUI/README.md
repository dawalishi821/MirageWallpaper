<!-- Copyright © 2026 王孝慈. All rights reserved. -->
# Mirage Linux UI

依据当前 macOS SwiftUI 界面独立实现的 Qt Quick 客户端。没有复制 Linux 分支的客户端或桌面适配代码。

## 构建与运行

需要 C++20 编译器、CMake 3.24+ 和 Qt 6.8+。Ubuntu 26.04 可安装以下依赖：

```sh
sudo apt install cmake ninja-build qt6-base-dev qt6-declarative-dev \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-dialogs \
  qml6-module-qtquick-layouts qml6-module-qtquick-window qml6-module-qtquick-templates \
  qml6-module-qtquick-shapes qml6-module-qtqml-workerscript qt6-image-formats-plugins
cmake -S LinuxUI -B LinuxUI/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build LinuxUI/build
./LinuxUI/build/Mirage
```

界面资源与现有 macOS 翻译在构建时嵌入程序，运行时不依赖源码路径。设置使用 Qt 的用户配置目录，应用标识为 `Mirage/MirageLinuxUI`。壁纸目录只读；标题、标签、属性、收藏与播放列表的修改保存在独立配置中。移除壁纸不会删除原文件。

此阶段实现 UI、本地目录浏览和界面状态管理。Renderer、Steam、下载、系统屏保与更新服务尚未连接；相关操作显示实际不可用状态，不模拟成功。`Store.request` 是 UI 的操作信号，远端列表和任务列表由后续服务提供数据。

支持简体中文、繁体中文、英文以及深浅色外观。设置窗口的“取消”撤销预览，“好”提交设置。属性条件在带超时的独立 JavaScript 进程中执行。

## English

A Qt Quick implementation based on the current macOS SwiftUI interface. Requires Qt 6.8+, CMake 3.24+ and a C++20 compiler. Use the build commands above.

Wallpaper sources are read-only. UI preferences, property overrides, favorites and per-display playlists are stored under Qt's user configuration directory. Rendering, Steam, downloads, screen savers and updates are not connected in this UI stage. Their controls expose the unavailable state rather than simulating successful operations.
