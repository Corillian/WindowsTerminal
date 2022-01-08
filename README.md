![terminal-logos](https://user-images.githubusercontent.com/48369326/115790869-4c852b00-a37c-11eb-97f1-f61972c7800c.png)

# Welcome to the embeddable WinUI3 wrapper for Windows Terminal

This repository contains the source code for embedding the Windows Terminal in your own WinUI3 applications. According to the Windows Terminal roadmap, WinUI3 support isn't planned until at least sometime in 2023. Fortunately, you no longer have to wait that long. This repository will be updated as new versions of Windows Terminal are released.

If you were looking for a WPF wrapper the Windows Terminal team [already maintains one](https://github.com/microsoft/terminal/tree/main/src/cascadia/WpfTerminalControl). Lastly, the [Cascadia Code Font](https://github.com/Microsoft/Cascadia-Code) is _not_ packaged with these libraries.

Related repositories include:

* [Repo: Windows Terminal](https://github.com/microsoft/terminal)
* [Windows Terminal Documentation](https://docs.microsoft.com/windows/terminal)
  ([Repo: Contribute to the docs](https://github.com/MicrosoftDocs/terminal))
* [Console API Documentation](https://github.com/MicrosoftDocs/Console-Docs)
* [Cascadia Code Font](https://github.com/Microsoft/Cascadia-Code)
* [Windows Implementation Libraries](https://github.com/Microsoft/wil/wiki)

## Cloning and Building

The [Windows Terminal repository](https://github.com/microsoft/terminal) is linked as a submodule and contains its own submodules. To clone this repository make sure you recursively pull all submodules:

```
git clone --recurse-submodules https://github.com/Corillian/WindowsTerminal.git
```

Now the fun begins. For starters, **everything must be compiled with the same version of WinRT**. As of this writing, the Windows Terminal team is using a fairly old version of WinRT, however WinUI3 is much newer so I upgraded to the latest. Run `UpdateOpenConsoleCppWinRT.ps1` to do the upgrade. To further complicate matters, they maintain their own NuGet server for all of their dependencies and of course, upgrading WinRT breaks a bunch of stuff. To resolve this matter I created another script, `PatchOpenConsole.ps1`, to massage the OpenConsole submodule back into a compilable state.

Since we aren't having enough fun yet there are additional considerations for the first time you build this repository. The OpenConsole submodule needs to have its NuGet dependencies restored and a specific subset of projects built because there are some non-WinRT libraries and auto-generated files that get pulled into the WinUI3 wrappers. I have created another script, `Build.ps1`, to take care of this. Originally, I played around with having some MSBuild targets that would sort all of this out but there were issues with the child MSBuild processes pulling in build state from the parent so... I wrote another script. If I was an MSBuild ninja there's probably a way to get it working but - a script works and you only need to do this once:

```
.\Build.ps1 -restore
```

This will restore OpenConsole, build the requisite subset of OpenConsole dependencies, and then build the WinUI3 wrappers in `WindowsTerminal.sln`. If OpenConsole fails to build due to errors about hitting the heap limit of the C++ compiler just run `Build.ps1` (without `-restore`) until it stops complaining about heap limits and everything _should_ work. After you do this initial build you can then open `WindowsTerminal.sln` and build via Visual Studio as you normally would. In fact, if `Build.ps1` successfully builds OpenConsole but fails to build `WindowsTerminal.sln` you should try building `WindowsTerminal.sln` in Visual Studio because... welcome to WinRT.

## Some Thoughts on WinRT

This was my first project with WinRT. I don't know the exact history, you can probably Google it, but for those of you who've never heard of WinRT it's a thing that evolved from the Win8 UWP and C++/WRL stuff which in turn evolved from WPF. Like most everyone on Earth, I refused to ever touch UWP, however WinUI3, which actually looks pretty cool (that's why I did this), is saddled with all of this legacy UWP insanity. The best part of the insanity is that it has a name - WinRT.

If you stay exclusively in the land of C# things don't seem to be too bad. The moment you have to start dealing with C++/WinRT [prepare for hate and discontent](https://docs.microsoft.com/en-us/windows/uwp/cpp-and-winrt-apis/faq#why-am-i-getting-a--class-not-registered--exception-). The C++/WinRT build toolchain and its C# cousin, CsWinRT, currently seem to be [buggy](https://github.com/microsoft/CsWinRT/issues/756) and [flakey](https://github.com/microsoft/CsWinRT/issues/864). Yes, flakey, as in non-deterministic - sometimes you will build and it'll work - sometimes it won't. What changed? Who knows, but restarting Visual Studio seems to help, except when it doesn't.

If the organization of this repository makes you think to yourself "was the person who created this on drugs?" the answer is yes - [hard](https://github.com/Microsoft/xlang/issues/318), [hard](https://docs.microsoft.com/en-us/windows/uwp/cpp-and-winrt-apis/troubleshooting) drugs. To those of you who actually try to build this project I wish you all the best, and if you submit an issue complaining about how you can't get it to build I want to give you a heads up that I'm probably going to [take a shot](https://docs.microsoft.com/en-us/windows/apps/desktop/modernize/desktop-to-uwp-supported-api?tabs=csharp) and [ignore it](https://github.com/MicrosoftDocs/windows-uwp/blob/docs/windows-apps-src/cpp-and-winrt-apis/consume-apis.md).