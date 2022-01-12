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

            var allSettings = CascadiaSettings.LoadAll();
            var defaultProfileGuid = allSettings.GlobalSettings.DefaultProfile;
            var profiles = allSettings.ActiveProfiles;
            NewTerminalArgs newTermArgs = null;
            Profile profile;

            for(int i = 0; i < profiles.Count; ++i)
            {
                profile = profiles[i];

                if(profile.Guid == defaultProfileGuid)
                {
                    newTermArgs = new(i);
                    newTermArgs.Profile = profile.Guid.ToString();
                    break;
                }
            }

            if(newTermArgs == null)
            {
                profile = allSettings.ActiveProfiles.FirstOrDefault();

                if(profile != null)
                {
                    newTermArgs = new(0);
                    newTermArgs.Profile = profile.Guid.ToString();
                }
                else
                {
                    profile = allSettings.AllProfiles.First();
                    newTermArgs.Profile = profile.Guid.ToString();
                }
            }

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

            _rootPanel.Children.Add(term);
        }
    }
}
