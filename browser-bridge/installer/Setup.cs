using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Text;
using System.Windows.Forms;
using Microsoft.Win32;

namespace MThSetup
{
    public static class C
    {
        public static readonly string ExtensionId = "kdnpkapafkcegakehbepdhlemdobhnch";
        public static readonly string HostName = "com.mthdownloader.bridge";
        public static readonly char B = (char)92;
        public static readonly char Q = (char)34;
        public static readonly char Tab = (char)9;
        public static readonly char Lf = (char)10;
        public static readonly char Cr = (char)13;
    }

    public static class Program
    {
        [STAThread]
        public static void Main(string[] args)
        {
            Options o = Options.Parse(args);
            if (o.Silent)
            {
                int code = 1;
                try
                {
                    InstallerCore core = new InstallerCore(o.InstallDir, o.DownloadDir, o.SkipRegistry, o.CreateShortcuts, o.RestartBrowsers);
                    core.Run(null);
                    if (o.RestartBrowsers) core.RestartBrowserProcesses();
                    code = 0;
                }
                catch (Exception ex)
                {
                    try { File.WriteAllText(Path.Combine(Path.GetTempPath(), "mth_setup_error.log"), ex.ToString()); } catch { }
                }
                Environment.Exit(code);
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }

    public class Options
    {
        public bool Silent;
        public bool SkipRegistry;
        public bool CreateShortcuts = true;
        public bool RestartBrowsers;
        public string InstallDir;
        public string DownloadDir;

        public static Options Parse(string[] args)
        {
            Options o = new Options();
            o.InstallDir = InstallerCore.DefaultInstallDir();
            o.DownloadDir = InstallerCore.DefaultDownloadDir();
            foreach (string a in args)
            {
                string s = a.Trim();
                if (s == "/S" || s == "-S" || string.Equals(s, "/silent", StringComparison.OrdinalIgnoreCase))
                    o.Silent = true;
                else if (s == "--no-registry" || s == "/noreg")
                    o.SkipRegistry = true;
                else if (s == "--no-shortcuts" || s == "/noshortcuts")
                    o.CreateShortcuts = false;
                else if (s == "--restart" || s == "/restart")
                    o.RestartBrowsers = true;
                else if (s.StartsWith("/D="))
                    o.InstallDir = s.Substring(3);
                else if (s.StartsWith("--install-dir="))
                    o.InstallDir = s.Substring(14);
                else if (s.StartsWith("/DownloadDir="))
                    o.DownloadDir = s.Substring(13);
                else if (s.StartsWith("--download-dir="))
                    o.DownloadDir = s.Substring(15);
            }
            return o;
        }
    }

    public class InstallerCore
    {
        public string InstallDir;
        public string DownloadDir;
        public bool SkipRegistry;
        public bool CreateShortcuts;
        public bool RestartBrowsers;

        public InstallerCore(string installDir, string downloadDir, bool skipRegistry, bool shortcuts, bool restartBrowsers)
        {
            InstallDir = Path.GetFullPath(installDir);
            DownloadDir = Path.GetFullPath(downloadDir);
            SkipRegistry = skipRegistry;
            CreateShortcuts = shortcuts;
            RestartBrowsers = restartBrowsers;
        }

        public static string DefaultInstallDir()
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MThDownloader");
        }

        public static string DefaultDownloadDir()
        {
            object v = null;
            try
            {
                using (RegistryKey k = Registry.CurrentUser.OpenSubKey(
                    "Software" + C.B + "Microsoft" + C.B + "Windows" + C.B + "CurrentVersion" + C.B + "Explorer" + C.B + "Shell Folders"))
                {
                    if (k != null) v = k.GetValue("{374DE290-123F-4565-9164-39C4925E467B}");
                }
            }
            catch { }
            string p = v as string;
            if (string.IsNullOrEmpty(p))
                p = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
            return p;
        }

        void Report(Action<int, string> cb, int percent, string msg)
        {
            if (cb != null) cb(percent, msg);
        }

        public void Run(Action<int, string> progress)
        {
            if (string.IsNullOrEmpty(InstallDir)) throw new Exception("安装目录不能为空");
            if (string.IsNullOrEmpty(DownloadDir)) throw new Exception("下载目录不能为空");

            Report(progress, 0, "正在创建目录…");
            Directory.CreateDirectory(InstallDir);
            Directory.CreateDirectory(DownloadDir);

            Report(progress, 10, "正在释放文件…");
            ExtractPayload();

            Report(progress, 55, "正在生成本地桥配置…");
            WriteBridgeConfig();
            WriteHostManifest();

            Report(progress, 65, "正在更新下载位置…");
            WriteIni();

            if (!SkipRegistry)
            {
                Report(progress, 75, "正在注册浏览器…");
                RegisterNativeHosts();
                RegisterExternalExtensions();
            }

            if (CreateShortcuts)
            {
                Report(progress, 85, "正在创建快捷方式…");
                CreateStartMenuShortcut();
                CreateDesktopShortcut();
            }

            Report(progress, 92, "正在写入卸载信息…");
            RegisterUninstaller();

            Report(progress, 100, "安装完成");
        }

        static string Js(string s)
        {
            StringBuilder sb = new StringBuilder();
            foreach (char ch in s)
            {
                if (ch == C.B) sb.Append(C.B).Append(C.B);
                else if (ch == C.Q) sb.Append(C.B).Append(C.Q);
                else sb.Append(ch);
            }
            return sb.ToString();
        }

        void ExtractPayload()
        {
            Assembly asm = Assembly.GetExecutingAssembly();
            string manifest;
            using (Stream s = asm.GetManifestResourceStream("mth_payload_manifest"))
            using (StreamReader r = new StreamReader(s, Encoding.UTF8))
                manifest = r.ReadToEnd();

            string[] lines = manifest.Split(C.Lf);
            foreach (string line in lines)
            {
                string t = line.Trim().TrimEnd(C.Cr);
                if (t.Length == 0) continue;
                int idx = t.IndexOf(C.Tab);
                if (idx < 0) continue;
                string res = t.Substring(0, idx).Trim();
                string rel = t.Substring(idx + 1).Trim();
                string dest = Path.Combine(InstallDir, rel.Replace('/', Path.DirectorySeparatorChar));
                string dir = Path.GetDirectoryName(dest);
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                using (Stream s = asm.GetManifestResourceStream(res))
                {
                    if (s == null) throw new Exception("缺少嵌入资源: " + res);
                    using (FileStream f = File.Create(dest)) s.CopyTo(f);
                }
            }
        }

        void WriteBridgeConfig()
        {
            string exe = Path.Combine(InstallDir, "MThDownloader.exe");
            StringBuilder sb = new StringBuilder();
            sb.Append("{").Append(Environment.NewLine);
            sb.Append("  ").Append(C.Q).Append("executable").Append(C.Q).Append(": ").Append(C.Q).Append(Js(exe)).Append(C.Q).Append(",").Append(Environment.NewLine);
            sb.Append("  ").Append(C.Q).Append("args_template").Append(C.Q).Append(": [").Append(C.Q).Append("{url}").Append(C.Q).Append("],").Append(Environment.NewLine);
            sb.Append("  ").Append(C.Q).Append("allowed_schemes").Append(C.Q).Append(": [").Append(C.Q).Append("http").Append(C.Q).Append(", ").Append(C.Q).Append("https").Append(C.Q).Append(", ").Append(C.Q).Append("magnet").Append(C.Q).Append("],").Append(Environment.NewLine);
            sb.Append("  ").Append(C.Q).Append("max_url_length").Append(C.Q).Append(": 8192").Append(Environment.NewLine);
            sb.Append("}").Append(Environment.NewLine);
            string dir = Path.Combine(InstallDir, "native-host");
            Directory.CreateDirectory(dir);
            File.WriteAllText(Path.Combine(dir, "bridge_config.json"), sb.ToString(), new UTF8Encoding(false));
        }

        void WriteHostManifest()
        {
            string run = Path.Combine(InstallDir, "native-host", "mth_bridge_host.exe");
            StringBuilder sb = new StringBuilder();
            sb.Append("{").Append(Environment.NewLine);
            sb.Append("  ").Append(C.Q).Append("name").Append(C.Q).Append(": ").Append(C.Q).Append(C.HostName).Append(C.Q).Append(",").Append(Environment.NewLine);
            sb.Append("  ").Append(C.Q).Append("description").Append(C.Q).Append(": ").Append(C.Q).Append("MThDownloader browser bridge host").Append(C.Q).Append(",").Append(Environment.NewLine);
            sb.Append("  ").Append(C.Q).Append("path").Append(C.Q).Append(": ").Append(C.Q).Append(Js(run)).Append(C.Q).Append(",").Append(Environment.NewLine);
            sb.Append("  ").Append(C.Q).Append("type").Append(C.Q).Append(": ").Append(C.Q).Append("stdio").Append(C.Q).Append(",").Append(Environment.NewLine);
            sb.Append("  ").Append(C.Q).Append("allowed_origins").Append(C.Q).Append(": [").Append(Environment.NewLine);
            sb.Append("    ").Append(C.Q).Append("chrome-extension://").Append(C.ExtensionId).Append("/").Append(C.Q).Append(Environment.NewLine);
            sb.Append("  ]").Append(Environment.NewLine);
            sb.Append("}").Append(Environment.NewLine);
            string dir = Path.Combine(InstallDir, "native-host");
            File.WriteAllText(Path.Combine(dir, C.HostName + ".json"), sb.ToString(), new UTF8Encoding(false));
        }

        static string SetIniValue(string text, string key, string value)
        {
            string[] lines = text.Split(C.Lf);
            List<string> outLines = new List<string>();
            bool found = false;
            foreach (string raw in lines)
            {
                string l = raw.TrimEnd(C.Cr);
                if (l.StartsWith(key + "=", StringComparison.OrdinalIgnoreCase))
                {
                    outLines.Add(key + "=" + value);
                    found = true;
                }
                else
                {
                    outLines.Add(l);
                }
            }
            if (!found) outLines.Add(key + "=" + value);
            return string.Join(Environment.NewLine, outLines.ToArray());
        }

        void WriteIni()
        {
            string iniPath = Path.Combine(InstallDir, "MThDownloader.ini");
            string text = File.Exists(iniPath) ? File.ReadAllText(iniPath) : "";
            if (!text.Contains("[Settings]"))
                text = "[Settings]" + Environment.NewLine + text;
            text = SetIniValue(text, "SaveDir", DownloadDir);
            string bt = DownloadDir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
            text = SetIniValue(text, "BtDir", bt);
            File.WriteAllText(iniPath, text, new UTF8Encoding(false));
        }

        string[] BrowserRoots()
        {
            return new string[] {
                "Software" + C.B + "Google" + C.B + "Chrome",
                "Software" + C.B + "Google" + C.B + "Chrome Beta",
                "Software" + C.B + "Google" + C.B + "Chrome Dev",
                "Software" + C.B + "Microsoft" + C.B + "Edge",
                "Software" + C.B + "Microsoft" + C.B + "Edge Beta",
                "Software" + C.B + "Microsoft" + C.B + "Edge Dev"
            };
        }

        void RegisterNativeHosts()
        {
            string manifestPath = Path.Combine(InstallDir, "native-host", C.HostName + ".json");
            foreach (string root in BrowserRoots())
            {
                try
                {
                    using (RegistryKey k = Registry.CurrentUser.CreateSubKey(root + C.B + "NativeMessagingHosts" + C.B + C.HostName))
                        k.SetValue("", manifestPath, RegistryValueKind.String);
                }
                catch { }
            }
        }

        void RegisterExternalExtensions()
        {
            // Chrome/Edge 读取 HKCU ...\Extensions\<id>，path 指向已签名的 CRX 文件。
            string crxPath = Path.Combine(InstallDir, "extension.crx");
            foreach (string root in BrowserRoots())
            {
                try
                {
                    using (RegistryKey k = Registry.CurrentUser.CreateSubKey(root + C.B + "Extensions" + C.B + C.ExtensionId))
                    {
                        k.SetValue("path", crxPath, RegistryValueKind.String);
                        k.SetValue("version", "1.0.0", RegistryValueKind.String);
                    }
                }
                catch { }
                try { Registry.CurrentUser.DeleteSubKeyTree(root + C.B + "External Extensions" + C.B + C.ExtensionId, false); }
                catch { }
            }
        }

        void CreateShortcut(string lnkPath, string target)
        {
            try
            {
                Type t = Type.GetTypeFromProgID("WScript.Shell");
                if (t == null) return;
                object shell = Activator.CreateInstance(t);
                object sc = t.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { lnkPath });
                Type st = sc.GetType();
                st.InvokeMember("TargetPath", BindingFlags.SetProperty, null, sc, new object[] { target });
                st.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, sc, new object[] { InstallDir });
                st.InvokeMember("Description", BindingFlags.SetProperty, null, sc, new object[] { "MThDownloader" });
                st.InvokeMember("Save", BindingFlags.InvokeMethod, null, sc, null);
            }
            catch { }
        }

        void CreateStartMenuShortcut()
        {
            string programs = Environment.GetFolderPath(Environment.SpecialFolder.Programs);
            string dir = Path.Combine(programs, "MThDownloader");
            Directory.CreateDirectory(dir);
            CreateShortcut(Path.Combine(dir, "MThDownloader.lnk"), Path.Combine(InstallDir, "MThDownloader.exe"));
        }

        void CreateDesktopShortcut()
        {
            string desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
            CreateShortcut(Path.Combine(desktop, "MThDownloader.lnk"), Path.Combine(InstallDir, "MThDownloader.exe"));
        }

        void RegisterUninstaller()
        {
            string key = "Software" + C.B + "Microsoft" + C.B + "Windows" + C.B + "CurrentVersion" + C.B + "Uninstall" + C.B + "MThDownloader";
            string uninstaller = Path.Combine(InstallDir, "MThDownloader_Uninstall.exe");
            string icon = Path.Combine(InstallDir, "MThDownloader.exe");
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(key))
            {
                k.SetValue("DisplayName", "MThDownloader");
                k.SetValue("DisplayIcon", icon);
                k.SetValue("DisplayVersion", "1.0");
                k.SetValue("Publisher", "MThDownloader");
                k.SetValue("InstallLocation", InstallDir);
                k.SetValue("UninstallString", C.Q + uninstaller + C.Q);
                k.SetValue("NoModify", 1, RegistryValueKind.DWord);
                k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            }
        }

        public void RestartBrowserProcesses()
        {
            RestartBrowser("chrome.exe", "Google" + C.B + "Chrome" + C.B + "Application" + C.B + "chrome.exe");
            RestartBrowser("msedge.exe", "Microsoft" + C.B + "Edge" + C.B + "Application" + C.B + "msedge.exe");
        }

        void RestartBrowser(string processName, string appPathTail)
        {
            string exe = null;
            try
            {
                using (RegistryKey k = Registry.LocalMachine.OpenSubKey(
                    "Software" + C.B + "Microsoft" + C.B + "Windows" + C.B + "CurrentVersion" + C.B + "App Paths" + C.B + processName))
                {
                    if (k != null) exe = k.GetValue("") as string;
                }
            }
            catch { }
            if (string.IsNullOrEmpty(exe) || !File.Exists(exe))
            {
                string[] roots = new string[] {
                    Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
                    Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86)
                };
                foreach (string pf in roots)
                {
                    string candidate = Path.Combine(pf, appPathTail);
                    if (File.Exists(candidate)) { exe = candidate; break; }
                }
            }
            if (string.IsNullOrEmpty(exe)) return;
            KillProcess(processName);
            try { Process.Start(exe, "--restore-last-session"); } catch { }
        }

        void KillProcess(string imageName)
        {
            try
            {
                Process p = new Process();
                p.StartInfo.FileName = "taskkill";
                p.StartInfo.Arguments = "/IM " + imageName + " /F";
                p.StartInfo.CreateNoWindow = true;
                p.StartInfo.UseShellExecute = false;
                p.StartInfo.WindowStyle = ProcessWindowStyle.Hidden;
                p.Start();
                p.WaitForExit(5000);
            }
            catch { }
        }
    }

    public class MainForm : Form
    {
        TextBox txtInstall;
        TextBox txtDownload;
        CheckBox chkReg;
        CheckBox chkShortcuts;
        CheckBox chkRestart;
        Button btnInstall;
        Button btnCancel;
        Label lblStatus;
        ProgressBar pb;

        public MainForm()
        {
            Text = "MThDownloader 安装程序";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(540, 340);
            Font = new Font("Microsoft YaHei UI", 9F);

            Label l1 = new Label();
            l1.Text = "安装位置：";
            l1.Location = new Point(20, 22);
            l1.AutoSize = true;
            Controls.Add(l1);

            txtInstall = new TextBox();
            txtInstall.Location = new Point(20, 44);
            txtInstall.Size = new Size(380, 23);
            txtInstall.Text = InstallerCore.DefaultInstallDir();
            Controls.Add(txtInstall);

            Button b1 = new Button();
            b1.Text = "浏览…";
            b1.Location = new Point(410, 42);
            b1.Size = new Size(100, 27);
            b1.Click += delegate { Browse(txtInstall); };
            Controls.Add(b1);

            Label l2 = new Label();
            l2.Text = "下载位置（将写入 MThDownloader.ini，安装后可再改）：";
            l2.Location = new Point(20, 82);
            l2.AutoSize = true;
            Controls.Add(l2);

            txtDownload = new TextBox();
            txtDownload.Location = new Point(20, 104);
            txtDownload.Size = new Size(380, 23);
            txtDownload.Text = InstallerCore.DefaultDownloadDir();
            Controls.Add(txtDownload);

            Button b2 = new Button();
            b2.Text = "浏览…";
            b2.Location = new Point(410, 102);
            b2.Size = new Size(100, 27);
            b2.Click += delegate { Browse(txtDownload); };
            Controls.Add(b2);

            chkReg = new CheckBox();
            chkReg.Text = "自动安装到 Chrome / Edge（注册原生消息宿主与浏览器插件）";
            chkReg.Location = new Point(20, 142);
            chkReg.AutoSize = true;
            chkReg.Checked = true;
            Controls.Add(chkReg);

            chkShortcuts = new CheckBox();
            chkShortcuts.Text = "创建开始菜单与桌面快捷方式";
            chkShortcuts.Location = new Point(20, 168);
            chkShortcuts.AutoSize = true;
            chkShortcuts.Checked = true;
            Controls.Add(chkShortcuts);

            chkRestart = new CheckBox();
            chkRestart.Text = "安装完成后自动重启 Chrome / Edge（会关闭当前浏览器窗口）";
            chkRestart.Location = new Point(20, 194);
            chkRestart.AutoSize = true;
            chkRestart.Checked = false;
            Controls.Add(chkRestart);

            lblStatus = new Label();
            lblStatus.Text = "就绪";
            lblStatus.Location = new Point(20, 222);
            lblStatus.Size = new Size(500, 20);
            Controls.Add(lblStatus);

            pb = new ProgressBar();
            pb.Location = new Point(20, 246);
            pb.Size = new Size(500, 22);
            pb.Minimum = 0;
            pb.Maximum = 100;
            pb.Value = 0;
            Controls.Add(pb);

            btnInstall = new Button();
            btnInstall.Text = "安装";
            btnInstall.Location = new Point(200, 286);
            btnInstall.Size = new Size(110, 36);
            btnInstall.Click += delegate { DoInstall(); };
            Controls.Add(btnInstall);

            btnCancel = new Button();
            btnCancel.Text = "取消";
            btnCancel.Location = new Point(330, 286);
            btnCancel.Size = new Size(110, 36);
            btnCancel.Click += delegate { Close(); };
            Controls.Add(btnCancel);

            AcceptButton = btnInstall;
            CancelButton = btnCancel;
        }

        void Browse(TextBox tb)
        {
            using (FolderBrowserDialog d = new FolderBrowserDialog())
            {
                d.Description = "选择文件夹";
                try { d.SelectedPath = tb.Text; } catch { }
                if (d.ShowDialog(this) == DialogResult.OK)
                    tb.Text = d.SelectedPath;
            }
        }

        void DoInstall()
        {
            if (string.IsNullOrWhiteSpace(txtInstall.Text) || string.IsNullOrWhiteSpace(txtDownload.Text))
            {
                MessageBox.Show(this, "请填写安装位置与下载位置。", "提示", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            btnInstall.Enabled = false;
            btnCancel.Enabled = false;
            lblStatus.Text = "正在安装，请稍候…";
            Application.DoEvents();
            try
            {
                InstallerCore core = new InstallerCore(txtInstall.Text, txtDownload.Text, !chkReg.Checked, chkShortcuts.Checked, chkRestart.Checked);
                core.Run(delegate(int pct, string m)
                {
                    pb.Value = pct;
                    lblStatus.Text = m;
                    Application.DoEvents();
                });

                if (chkRestart.Checked)
                {
                    lblStatus.Text = "正在重启浏览器…";
                    Application.DoEvents();
                    core.RestartBrowserProcesses();
                }

                lblStatus.Text = "安装完成。";
                string doneMsg = "安装完成。" + Environment.NewLine +
                    "1) 下载位置已写入 MThDownloader.ini。" + Environment.NewLine +
                    "2) 插件已注册到 Chrome / Edge。" + Environment.NewLine;
                if (chkRestart.Checked)
                    doneMsg += "3) 浏览器已重启，插件应已加载。" + Environment.NewLine;
                else
                    doneMsg += "3) 请完全退出并重新打开 Chrome / Edge 以加载插件。" + Environment.NewLine;
                doneMsg += "4) 若浏览器未自动启用插件，请在 chrome://extensions 开发者模式下加载安装目录下的 extension 文件夹。";
                MessageBox.Show(this, doneMsg, "完成", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            catch (Exception ex)
            {
                lblStatus.Text = "安装失败。";
                MessageBox.Show(this, ex.Message, "错误", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            finally
            {
                btnInstall.Enabled = true;
                btnCancel.Enabled = true;
            }
        }
    }
}
