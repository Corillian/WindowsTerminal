using Microsoft.Windows.ApplicationModel.DynamicDependency;
using System.Diagnostics;
using System.Reflection;

namespace SampleAppUnpackaged
{
    internal static class Program
    {
        static string StartupDirectory { get; } = Path.GetDirectoryName(typeof(Program).Assembly.Location) ?? "";

        static readonly CreatePackageDependencyOptions PackageDependencyOptions = new()
        {
            Architectures = PackageDependencyProcessorArchitectures.X64,
            LifetimeArtifactKind = PackageDependencyLifetimeArtifactKind.Process,
            VerifyDependencyResolution = true
        };

        /// <summary>
        ///  The main entry point for the application.
        /// </summary>
        [STAThread]
        static void Main(string[] args)
        {
            AppDomain.CurrentDomain.AssemblyResolve += CurrentDomain_AssemblyResolve;

            Bootstrap.Initialize(0x00010000);

#if DEBUG
            var vcLibs = Microsoft.Windows.ApplicationModel.DynamicDependency.PackageDependency.Create("Microsoft.VCLibs.140.00.Debug_8wekyb3d8bbwe", new Windows.ApplicationModel.PackageVersion(14, 0, 30704, 0), PackageDependencyOptions);
            var uwpVClibs = Microsoft.Windows.ApplicationModel.DynamicDependency.PackageDependency.Create("Microsoft.VCLibs.140.00.Debug.UWPDesktop_8wekyb3d8bbwe", new Windows.ApplicationModel.PackageVersion(14, 0, 30704, 0), PackageDependencyOptions);
#else
            var vcLibs = Microsoft.Windows.ApplicationModel.DynamicDependency.PackageDependency.Create("Microsoft.VCLibs.140.00_8wekyb3d8bbwe", new Windows.ApplicationModel.PackageVersion(14, 0, 30704, 0), PackageDependencyOptions);
            var uwpVClibs = Microsoft.Windows.ApplicationModel.DynamicDependency.PackageDependency.Create("Microsoft.VCLibs.140.00.UWPDesktop_8wekyb3d8bbwe", new Windows.ApplicationModel.PackageVersion(14, 0, 30704, 0), PackageDependencyOptions);
#endif

            try
            {
                var vcLibsCtx = vcLibs?.Add();
                var uwpVClibsCtx = uwpVClibs?.Add();

                try
                {
                    // NOTE: For some reason something changed with the WinForms build toolchain
                    // that now prevents this application from referencing the SampleApp.dll project.
                    // Unfortunately, that means we now have to dynamically load everything...

                    var sampleAppDll = Assembly.LoadFile(Path.Combine(StartupDirectory, "SampleApp.dll"));
                    var program = sampleAppDll.GetType("SampleApp.Program");

                    if(program == null)
                    {
                        throw new InvalidOperationException("Failed to locate SampleApp.Program.");
                    }

                    var mainMethod = program.GetMethod(
                        "Main",
                        BindingFlags.Static | BindingFlags.NonPublic,
                        new Type[] { typeof(string[]) });

                    if(mainMethod == null)
                    {
                        throw new InvalidOperationException("Failed to locate SampleApp.Program.Main().");
                    }

                    mainMethod.Invoke(null, new object[] { args });
                }
                finally
                {
                    uwpVClibsCtx?.Remove();
                    vcLibsCtx?.Remove();
                }
            }
            catch(Exception ex)
            {
                Debug.WriteLine(ex);

                throw;
            }
            finally
            {
                uwpVClibs.Delete();
                vcLibs.Delete();

                Bootstrap.Shutdown();
            }
        }

        private static Assembly? CurrentDomain_AssemblyResolve(object? sender, ResolveEventArgs args)
        {
            int indexOfComma = args.Name.IndexOf(',');

            if(indexOfComma == -1)
            {
                return null;
            }

            string asmName = args.Name[..indexOfComma];

            if(asmName.EndsWith(".resources"))
            {
                return null;
            }

            if(!asmName.EndsWith(".dll"))
            {
                asmName += ".dll";
            }

            return Assembly.LoadFile(Path.Combine(StartupDirectory, asmName));
        }
    }
}