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
        public MainWindow()
        {
            this.InitializeComponent();

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

            var createTerminalSettings = TerminalSettings.CreateWithNewTerminalArgs(allSettings, newTermArgs, null);
            var terminalSettings = createTerminalSettings.DefaultSettings;
            var connection = new ConptyConnection();

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
            var childSettings = TerminalSettings.CreateWithParent(createTerminalSettings);
            var term = new TermControl(childSettings.DefaultSettings, connection)
            {
                UnfocusedAppearance = childSettings.UnfocusedSettings // It is okay for the unfocused settings to be null
            };

            term.OpenHyperlink += OnTerminal_OpenHyperlink;

            _rootPanel.Children.Add(term);
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
    }
}
