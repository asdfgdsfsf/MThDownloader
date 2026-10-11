# MThDownloader 下载桥接（Chrome / Edge）

拦截浏览器下载，把下载地址通过 **Chrome Native Messaging** 同步给本机
`MThDownloader.exe`，由它进行多线程下载。Chrome 与 Edge 通用（同一套 MV3 扩展）。

## 工作原理

```
浏览器下载
   │  chrome.downloads.onCreated（下载刚创建）
   ▼
扩展 service worker
   │  1. 按需 cancel + erase 浏览器下载
   │  2. chrome.runtime.sendNativeMessage("com.mthdownloader.bridge")
   ▼
run_host.bat → mth_bridge_host.py（本地桥，stdio 协议）
   │  3. 读取 bridge_config.json 的命令行模板
   ▼
MThDownloader.exe <url>   （多线程下载）
```

- 桥接失败时，可自动“回退为浏览器自带下载”（选项页可关）。
- 右键链接 / 图片 / 音视频，可“使用 MThDownloader 下载”（支持 `magnet:`）。
- 全程只在本机 stdio 通信，不经过网络、不经过第三方服务器。

## 目录结构

```
mth-browser-bridge/
├─ build.js                      # 生成 dist/chrome、dist/edge 与宿主清单
├─ package.json
├─ config/
│  ├─ public_key.b64             # 扩展公钥（仅公钥，用于固定扩展 ID）
│  └─ extension_id.txt           # 派生出的扩展 ID（构建时生成）
├─ src/                          # 扩展源码（MV3）
│  ├─ manifest.json
│  ├─ background.js              # 下载拦截 + Native Messaging 转发
│  ├─ options.html / options.js / options.css
│  └─ icons/
├─ native-host/
│  ├─ mth_bridge_host.exe        # 原生消息宿主（独立 C exe，零依赖）
│  ├─ mth_bridge_host.c          # 宿主源码（MinGW gcc 编译）
│  ├─ mth_bridge_host.py         # 备用 Python 宿主（需要 Python 3）
│  ├─ run_host.bat               # 备用包装脚本（配合 Python 宿主使用）
│  ├─ bridge_config.json         # MThDownloader.exe 路径与命令行模板
│  ├─ manifest.template.json     # 宿主清单模板
│  ├─ com.mthdownloader.bridge.json  # 构建生成的宿主清单（含本机绝对路径）
│  ├─ install.bat                # 一键构建 + 注册 Chrome/Edge 宿主
│  └─ uninstall.bat
└─ test/                         # 静态校验 + 宿主协议单元测试
```

## 一键安装器（推荐）

`dist-setup\MThDownloader_Setup.exe`（已复制到工作区根目录 `MThDownloader_Setup.exe`）是单文件 GUI 安装器，双击运行即可：

- **自定义安装位置**：默认 `%LOCALAPPDATA%\MThDownloader`，可浏览修改；
- **下载位置**：默认取当前电脑“下载”文件夹（可改），安装时写入 `MThDownloader.ini` 的 `SaveDir` / `BtDir`；
- **自动安装到 Chrome / Edge**：写入 HKCU `...\Google\Chrome\Extensions\<id>` 与 `...\Microsoft\Edge\Extensions\<id>`（`path` 指向已签名的 `extension.crx`，`version`=1.0.0），重启浏览器后插件自动加载（已实测 Chrome 154 自动安装成功）；
  若浏览器未自动启用，可在 `chrome://extensions` / `edge://extensions` 开发者模式下加载安装目录里的 `extension` 文件夹；
- **开始菜单 / 桌面快捷方式**：默认创建，可在界面取消；
- **安装进度条**：实时显示各阶段进度；
- **卸载器**：写入“控制面板\程序\卸载”条目；也可直接运行安装目录里的 `MThDownloader_Uninstall.exe`；
- **安装完成后自动重启浏览器**：界面勾选后，安装结束会自动重启 Chrome / Edge 以加载插件（默认不勾选，避免误关浏览器）。

静默安装（自动化用）：
`MThDownloader_Setup.exe /S --install-dir=D:\path --download-dir=D:\Downloads [/restart] [/no-shortcuts] [/no-registry]`

静默卸载（自动化用）：`MThDownloader_Uninstall.exe /S [/keep-files]`

- 打包内容：`MThDownloader.exe`、`engine\aria2c.exe`、`MThDownloader.ini`、`MThDownloader.history.jsonl` 与浏览器插件、本地桥。

## 安装（Chrome 与 Edge 都要做）

1. 双击（或命令行执行）`native-host\install.bat`：
   - 生成 `dist\chrome`、`dist\edge` 与 `native-host\com.mthdownloader.bridge.json`；
   - 注册 `HKCU\Software\Google\Chrome\NativeMessagingHosts` 与
     `HKCU\Software\Microsoft\Edge\NativeMessagingHosts`。
2. 编辑 `native-host\bridge_config.json`，确认 `MThDownloader.exe` 路径正确：
   ```json
   {
     "executable": "C:\path\to\MThDownloader.exe",
     "args_template": ["{url}"],
     "allowed_schemes": ["http", "https", "ftp", "magnet"],
     "max_url_length": 8192
   }
   ```
3. Chrome：打开 `chrome://extensions` → 开启“开发者模式” → “加载已解压的扩展程序”
   → 选择 `dist\chrome`。
4. Edge：打开 `edge://extensions` → 开启“开发人员模式” → “加载解压缩的扩展”
   → 选择 `dist\edge`。
5. 点扩展图标 → “测试本地桥连接”，看到“连接成功”即完成。

> 卸载：运行 `native-host\uninstall.bat` 并移除扩展即可。

## 配置说明

`native-host\bridge_config.json`：

| 字段 | 含义 |
| --- | --- |
| `executable` | MThDownloader.exe 的绝对路径 |
| `args_template` | 启动参数模板，支持占位符 `{url}` `{filename}` `{referrer}` `{mime}` |
| `allowed_schemes` | 允许转发的 URL 协议白名单 |
| `max_url_length` | 最大 URL 长度，超长拒绝 |

如果 MThDownloader.exe 需要别的参数形式，例如
`MThDownloader.exe --url="https://..."`，把模板改成：
```json
"args_template": ["--url={url}"]
```

扩展选项页：

- **拦截浏览器下载**：总开关；
- **仅拦截这些扩展名**：逗号分隔，如 `zip,rar,7z,iso,exe,apk,mp4`，留空表示全部拦截；
- **桥接失败时回退为浏览器下载**：默认开启。

## 为什么扩展 ID 是固定的

扩展通过 `manifest.json` 的 `key`（公钥）固定 ID 为：

```
kdnpkapafkcegakehbepdhlemdobhnch
```

Chrome 与 Edge 都是 Chromium，ID 派生算法一致，因此两边的
`allowed_origins` 用同一个 `chrome-extension://<id>/` 即可。若改为上架商店发布，
商店会替换 key，需把发布后的实际 ID 填回 `native-host/manifest.template.json`
的 `allowed_origins` 并重新安装。

## 常见问题

- **“本地桥未连接 / Specified native messaging host not found”**
  先运行 `install.bat`，然后**重新加载扩展**（chrome://extensions 中点击刷新）。
- **点了下载没反应**
  检查 `bridge_config.json` 的 `executable` 路径是否为 MThDownloader.exe 的实际位置。
- **想临时关闭拦截**
  点扩展图标，取消“拦截浏览器下载”即可（也可只放行指定扩展名）。

## MThDownloader 命令行

- `MThDownloader.exe "<url>" [保存目录]` —— 打开主窗口，自动填入 URL 并**立即开始下载**；
  `magnet:` 链接自动切到 BT 页签。
- **单实例**：程序已在运行时再次带 URL 启动，会把 URL 转发给现有窗口后退出
  （浏览器批量转发不会拉起多个实例，也不会争抢 BT 引擎的 RPC 端口）。
- 示例：`MThDownloader.exe "https://example.com/file.zip"`、
  `MThDownloader.exe "magnet:?xt=urn:btih:..."`、
  `MThDownloader.exe "https://example.com/file.zip" "D:\downloads"`

## 安全说明

- 宿主**只信任本地 `bridge_config.json`** 里的可执行路径，不信任浏览器侧传入的路径；
- URL 仅放行 `allowed_schemes` 白名单（默认 http/https/ftp/magnet，禁止 `file:`、`javascript:` 等）；
- 通信仅通过本机标准输入/输出，无任何网络传输；
- 仓库只包含扩展**公钥**，不包含私钥、令牌或任何凭据（见 `.gitignore`）。

## 从源码构建（克隆仓库后）

仓库不含私钥与编译产物；首次构建需准备：

1. 编译原生消息宿主（需要 gcc/MinGW）：
   `gcc -O2 -o native-host/mth_bridge_host.exe native-host/mth_bridge_host.c`
   （`build_setup.py` 发现 exe 缺失时会自动尝试用 gcc 编译。）
2. 生成扩展签名私钥（二选一）：
   - 已有 `installer/extension_key.pem` 则跳过；
   - 否则用 Chrome 打包生成：`chrome.exe --pack-extension=<扩展目录>`，把生成的 `.pem`
     复制为 `installer/extension_key.pem`。
   `build_setup.py` 会从该 pem 自动派生公钥与扩展 ID 并写入 `config/`。
   > `extension_key.pem` 是私钥，已被 `.gitignore` 排除，**切勿提交**。
3. 构建安装器：`python installer/build_setup.py`，产物在 `dist-setup/MThDownloader_Setup.exe`。

本机 `native-host/bridge_config.json` 是机器相关文件（已忽略），参考
`native-host/bridge_config.example.json`；安装器会在安装时自动生成。

## 开发与测试

```bash
npm run build   # 构建 dist/chrome、dist/edge 与宿主清单
npm run check   # JS 语法检查
npm test        # 静态校验 + 本地桥协议单元测试
```

## 许可

MIT License，见 [LICENSE](LICENSE)。
