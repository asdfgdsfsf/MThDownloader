using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Windows.Forms;
using Microsoft.Win32;

namespace MThSetup
{
    public static class UC
    {
        public static readonly string ExtensionId = "kdnpkapafkcegakehbepdhlemdobhnch";
        public static readonly string HostName = "com.mthdownloader.bridge";
        public static readonly char B = (char)92;
        public static readonly char Q = (char)34;
    }

    public static class Program
    {
        [STAThread]
        public static void Main(string[] args)
        {
            bool silent = false;
            bool keepFiles = false;
            foreach (string a in args)
            {
                string s = a.Trim();
                if (s == "/S" || s == "-S" || string.Equals(s, "/silent", StringComparison.OrdinalIgnoreCase))
                    silent = true;
                else if (s == "/keep-files" || s == "--keep-files")
                    keepFiles = true;
            }
            if (silent)
            {
                int code = 1;
                try
                {
                    UninstallerCore core = new UninstallerCore();
                    core.Run(!keepFiles, null);
                    code = 0;
                }
                catch (Exception ex)
                {
                    try { File.WriteAllText(Path.Combine(Path.GetTempPath(), "mth_uninstall_error.log"), ex.ToString()); } catch { }
                }
                Environment.Exit(code);
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new UninstallForm());
        }
    }

    public class UninstallerCore
    {
        public string InstallDir
        {
            get { return Path.GetDirectoryName(Application.ExecutablePath); }
        }

        string[] BrowserRoots()
        {
            return new string[] {
                "Software" + UC.B + "Google" + UC.B + "Chrome",
                "Software" + UC.B + "Google" + UC.B + "Chrome Beta",
                "Software" + UC.B + "Google" + UC.B + "Chrome Dev",
                "Software" + UC.B + "Microsoft" + UC.B + "Edge",
                "Software" + UC.B + "Microsoft" + UC.B + "Edge Beta",
                "Software" + UC.B + "Microsoft" + UC.B + "Edge Dev"
            };
        }

        void Report(Action<int, string> cb, int percent, string msg)
        {
            if (cb != null) cb(percent, msg);
        }

        public void Run(bool deleteFiles, Action<int, string> progress)
        {
            Report(progress, 5, "正在移除浏览器注册…");
            RemoveRegistry();

            Report(progress, 35, "正在移除快捷方式…");
            RemoveShortcuts();

            if (deleteFiles)
            {
                Report(progress, 60, "正在删除安装目录…");
                DeleteFiles();
                Report(progress, 95, "正在清理…");
            }
            Report(progress, 100, "卸载完成");
        }

        void RemoveRegistry()
        {
            foreach (string root in BrowserRoots())
            {
                TryDeleteTree(root + UC.B + "NativeMessagingHosts" + UC.B + UC.HostName);
                TryDeleteTree(root + UC.B + "Extensions" + UC.B + UC.ExtensionId);
                TryDeleteTree(root + UC.B + "External Extensions" + UC.B + UC.ExtensionId);
            }
            TryDeleteTree("Software" + UC.B + "Microsoft" + UC.B + "Windows" + UC.B + "CurrentVersion" + UC.B + "Uninstall" + UC.B + "MThDownloader");
        }

        void TryDeleteTree(string key)
        {
            try { Registry.CurrentUser.DeleteSubKeyTree(key, false); }
            catch { }
        }

        void RemoveShortcuts()
        {
            try
            {
                string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), "MThDownloader");
                if (Directory.Exists(dir)) Directory.Delete(dir, true);
            }
            catch { }
            try
            {
                string desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                string lnk = Path.Combine(desktop, "MThDownloader.lnk");
                if (File.Exists(lnk)) File.Delete(lnk);
            }
            catch { }
        }

        void DeleteFiles()
        {
            string self = Application.ExecutablePath;
            foreach (string f in Directory.GetFiles(InstallDir, "*", SearchOption.AllDirectories))
            {
                if (string.Equals(f, self, StringComparison.OrdinalIgnoreCase)) continue;
                try { File.Delete(f); } catch { }
            }
            foreach (string d in Directory.GetDirectories(InstallDir, "*", SearchOption.AllDirectories))
            {
                try { Directory.Delete(d, true); } catch { }
            }
            ScheduleSelfDelete();
        }

        void ScheduleSelfDelete()
        {
            try
            {
                string args = "/c ping 127.0.0.1 -n 2 >nul & rd /s /q " + UC.Q + InstallDir + UC.Q;
                Process p = new Process();
                p.StartInfo.FileName = "cmd.exe";
                p.StartInfo.Arguments = args;
                p.StartInfo.CreateNoWindow = true;
                p.StartInfo.UseShellExecute = false;
                p.StartInfo.WindowStyle = ProcessWindowStyle.Hidden;
                p.Start();
            }
            catch { }
        }
    }

    public class UninstallForm : Form
    {
        CheckBox chkDelete;
        Button btnUninstall;
        Button btnCancel;
        Label lblStatus;
        ProgressBar pb;

        public UninstallForm()
        {
            Text = "MThDownloader 卸载程序";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(480, 260);
            Font = new Font("Microsoft YaHei UI", 9F);

            UninstallerCore core = new UninstallerCore();

            Label l1 = new Label();
            l1.Text = "将卸载：" + core.InstallDir;
            l1.Location = new Point(20, 24);
            l1.AutoSize = true;
            Controls.Add(l1);

            chkDelete = new CheckBox();
            chkDelete.Text = "同时删除安装目录及所有文件（含下载历史）";
            chkDelete.Location = new Point(20, 60);
            chkDelete.AutoSize = true;
            chkDelete.Checked = true;
            Controls.Add(chkDelete);

            lblStatus = new Label();
            lblStatus.Text = "就绪";
            lblStatus.Location = new Point(20, 100);
            lblStatus.Size = new Size(440, 20);
            Controls.Add(lblStatus);

            pb = new ProgressBar();
            pb.Location = new Point(20, 124);
            pb.Size = new Size(440, 22);
            pb.Minimum = 0;
            pb.Maximum = 100;
            Controls.Add(pb);

            btnUninstall = new Button();
            btnUninstall.Text = "卸载";
            btnUninstall.Location = new Point(180, 190);
            btnUninstall.Size = new Size(110, 36);
            btnUninstall.Click += delegate { DoUninstall(); };
            Controls.Add(btnUninstall);

            btnCancel = new Button();
            btnCancel.Text = "取消";
            btnCancel.Location = new Point(310, 190);
            btnCancel.Size = new Size(110, 36);
            btnCancel.Click += delegate { Close(); };
            Controls.Add(btnCancel);

            AcceptButton = btnUninstall;
            CancelButton = btnCancel;
        }

        void DoUninstall()
        {
            btnUninstall.Enabled = false;
            btnCancel.Enabled = false;
            try
            {
                UninstallerCore core = new UninstallerCore();
                core.Run(chkDelete.Checked, delegate(int pct, string msg)
                {
                    pb.Value = pct;
                    lblStatus.Text = msg;
                    Application.DoEvents();
                });
                lblStatus.Text = "卸载完成。";
                MessageBox.Show(this,
                    "卸载完成。" + Environment.NewLine +
                    "请完全退出并重新打开 Chrome / Edge 以移除插件。",
                    "完成", MessageBoxButtons.OK, MessageBoxIcon.Information);
                Close();
            }
            catch (Exception ex)
            {
                lblStatus.Text = "卸载失败。";
                MessageBox.Show(this, ex.Message, "错误", MessageBoxButtons.OK, MessageBoxIcon.Error);
                btnUninstall.Enabled = true;
                btnCancel.Enabled = true;
            }
        }
    }
}
