# MThDownloader

MThDownloader 是一款原生 Win32 多线程下载器，轻量绿色、无需安装，专为极致下载速度打造。

它支持 HTTP/HTTPS 直链与 BT 种子、磁力链下载：多线程自动探测带宽增益、动态增减线程，把网速榨干；下载接近尾声时自动细化分块、均衡调度，告别最后 1% 龟速。文件名自动识别填写，你只需选好保存目录；种子文件直接拖进窗口即可开始。

配合浏览器扩展，网页点击下载自动转入 MThDownloader 全速下载，专属面板实时显示进度、大小与耗时。所有配置保存在软件目录，拷走即用，双击启动。

## 功能特性

- **极致多线程**：自动探测带宽增益，动态调整线程数（1~128 可调）
- **智能尾段加速**：队列将空时自动对半拆分剩余块，拆分下限可在界面调整（64~8192 KB）
- **BT / 磁力链**：内置 aria2 引擎（`engine/aria2c.exe`），支持拖入 `.torrent` 文件
- **强制自动命名**：Content-Disposition → URL 推断 → MIME 补后缀，用户只选目录
- **命令行接口**：`MThDownloader.exe "<url>" [保存目录]`，magnet 链接自动切 BT 页；单实例自动转发
- **断点续传**：`.part` 临时文件 + 分块状态持久化
- **下载历史**：`MThDownloader.history.jsonl` 记录每次下载的开始/完成/失败/取消
- **设置持久化**：参数写入 exe 旁 `MThDownloader.ini`，下次启动自动恢复

## 目录结构

```
MThDownloader/
├── downloader/          # 下载器主体（C++ Win32 源码）
│   ├── main.cpp         # 全部核心逻辑（约 1900 行）
│   ├── res.rc           # 资源文件（图标 + manifest）
│   ├── app.ico          # 程序图标
│   ├── app.manifest     # DPI 感知清单
│   └── build.bat        # 一键编译脚本（MinGW）
└── browser-bridge/      # 浏览器扩展 + 本地桥（原 mth-browser-bridge 项目）
    ├── src/             # Chrome/Edge 扩展（MV3）
    ├── native-host/     # Chrome Native Messaging 宿主（纯 C，零依赖）
    ├── installer/       # Windows 一键安装器工程（C#）
    └── test/            # 协议测试脚本
```

## 从源码构建

### 下载器（需要 MinGW-w64）

```bat
cd downloader
build.bat
```

产物 `MThDownloader.exe` 直接生成在脚本同级可执行目录；BT 功能需要把 `aria2c.exe` 放在 exe 旁的 `engine\` 子目录。

### 浏览器扩展 + 本地桥

```bash
cd browser-bridge
# 编译本地桥宿主（需要 gcc/MinGW）
gcc -O2 -o native-host/mth_bridge_host.exe native-host/mth_bridge_host.c
# 构建扩展与注册清单（需要 Node.js）
node build.js
```

随后：

1. 浏览器打开 `chrome://extensions` / `edge://extensions`，开启开发者模式，加载 `dist/chrome` 或 `dist/edge`；
2. 运行 `native-host/install.bat` 注册 Native Messaging 宿主；
3. 在扩展选项页填入 `MThDownloader.exe` 的完整路径并保存。

## 使用说明

- **HTTP 直链**：粘贴 URL，选择保存目录，点开始；
- **BT 种子**：切到 BT 页，选择或直接拖入 `.torrent` 文件，磁力链粘贴到来源框；
- **浏览器接管**：安装扩展后，网页点击下载会自动取消浏览器下载并转入 MThDownloader（可在扩展选项里关闭"保留浏览器历史记录"）；
- **下载面板**：点击扩展图标打开专属面板，实时查看所有下载的状态、大小与耗时。

## 作者

**Nikolabest**

## 许可证

[MIT License](LICENSE)
