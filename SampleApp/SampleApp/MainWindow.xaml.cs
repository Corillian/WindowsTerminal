using Microsoft.Terminal.Control;
using Microsoft.Terminal.Settings.Model;
using Microsoft.Terminal.TerminalConnection;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.ApplicationModel.DataTransfer;
using Windows.Foundation;
using Windows.Foundation.Collections;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace SampleApp
{
    /// <summary>
    /// An empty window that can be used on its own or navigated to within a Frame.
    /// </summary>
    public sealed partial class MainWindow : Window
    {
        TermControl _terminal;

        public MainWindow()
        {
            this.InitializeComponent();

            
        }

        private void OnTerminal_OpenHyperlink(object sender, OpenHyperlinkEventArgs args)
        {
            var procInfo = new ProcessStartInfo(args.Uri)
            {
                Verb = "open",
                UseShellExecute = true,
            };

            try
            {
                Process.Start(procInfo)?.Dispose();
            }
            catch(Exception ex)
            {
                Debug.WriteLine(ex);
            }
        }

        private void Window_Activated(object sender, WindowActivatedEventArgs args)
        {
            if(_terminal == null)
            {
                CascadiaSettings allSettings;

                try
                {
                    allSettings = CascadiaSettings.LoadAll();
                }
                catch
                {
                    allSettings = CascadiaSettings.LoadDefaults();
                }

                var defaultProfileGuid = allSettings.GlobalSettings.DefaultProfile;
                Profile profile = null;
                Profile profPwsh = null;
                Profile profWinPwsh = null;
                Profile profCmd = null;

                foreach(var prof in allSettings.ActiveProfiles)
                {
                    if(prof.Name.Equals("PowerShell", StringComparison.OrdinalIgnoreCase))
                    {
                        profPwsh = prof;
                    }
                    else if(prof.Name.Equals("Windows PowerShell", StringComparison.OrdinalIgnoreCase))
                    {
                        profWinPwsh = prof;
                    }
                    else if(prof.Name.Equals("Command Prompt", StringComparison.OrdinalIgnoreCase) || prof.Name.Equals("cmd", StringComparison.OrdinalIgnoreCase))
                    {
                        profCmd = prof;
                    }

                    if(prof.Guid == defaultProfileGuid)
                    {
                        profile = prof;
                        break;
                    }
                }

                if(profile == null)
                {
                    profile = profPwsh ?? profWinPwsh ?? profCmd ?? allSettings.ActiveProfiles.FirstOrDefault() ?? allSettings.AllProfiles.First();
                }

                int profileIndex = allSettings.ActiveProfiles?.IndexOf(profile) ?? -1;
                NewTerminalArgs newTermArgs = profileIndex == -1 ? new() : new(profileIndex)
                {
                    Profile = profile.Guid.ToString()
                };

                var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
                var createTerminalSettings = TerminalSettings.CreateWithNewTerminalArgs(allSettings, newTermArgs, null);
                var terminalSettings = createTerminalSettings.DefaultSettings;
                var connection = new ConptyConnection();

                if(hwnd != IntPtr.Zero)
                {
                    unchecked
                    {
                        connection.ReparentWindow((ulong)hwnd.ToInt64());
                    }
                }

                connection.ShowHide(true);

                connection.Initialize(ConptyConnection.CreateSettings(
                    terminalSettings.Commandline,
                    terminalSettings.StartingDirectory,
                    terminalSettings.StartingTitle,
                    new Dictionary<string, string>() { ["WT_PROFILE_ID"] = newTermArgs.Profile, ["WSLENV"] = "WT_PROFILE_ID" },
                    (uint)terminalSettings.InitialRows,
                    (uint)terminalSettings.InitialCols,
                    Guid.NewGuid()));

                this.Title = "SampleApp: " + terminalSettings.StartingTitle;

                // Give term control a child of the settings so that any overrides go in the child
                // This way, when we do a settings reload we just update the parent and the overrides remain
                _terminal = new TermControl(createTerminalSettings.DefaultSettings, createTerminalSettings.UnfocusedSettings, connection);

                // ConPTY assumes it's hidden at the start. If we're not, let it know now.
                _terminal.WindowVisibilityChanged(true);

                _terminal.OpenHyperlink += OnTerminal_OpenHyperlink;
                _terminal.CopyToClipboard += OnTerminal_CopyToClipboard;


                _rootPanel.Children.Add(_terminal);
            }
        }

        private void OnTerminal_CopyToClipboard(object sender, CopyToClipboardEventArgs args)
        {
            DataPackage dp = new() { RequestedOperation = DataPackageOperation.Copy };

            var formats = args.Formats;

            if(formats == null)
            {
                formats = CopyFormat.All;
            }

            dp.SetText(args.Text);

            if((formats & CopyFormat.HTML) == CopyFormat.HTML)
            {
                var html = args.Html;

                if(!string.IsNullOrEmpty(html))
                {
                    dp.SetHtmlFormat(html);
                }
            }

            if((formats & CopyFormat.RTF) == CopyFormat.RTF)
            {
                var rtf = args.Rtf;

                if(!string.IsNullOrEmpty(rtf))
                {
                    dp.SetRtf(rtf);
                }
            }

            try
            {
                Clipboard.SetContent(dp);
                Clipboard.Flush();
            }
            catch(Exception ex)
            {
                Debug.WriteLine(ex.ToString());
            }
        }
    }
}
