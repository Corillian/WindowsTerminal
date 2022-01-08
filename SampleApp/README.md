# SampleApp

This is an example application showing a very simple integration of the Windows Terminal. It loads the global Windows Terminal settings, or uses defaults if they don't exist, and starts the default profile (PowerShell is the default). It does not register any of the command and event handlers, or do anything else, that a proper integration would do. You can look at [TerminalApp](https://github.com/microsoft/terminal/tree/main/src/cascadia/TerminalApp) to see how Windows Terminal actually works.