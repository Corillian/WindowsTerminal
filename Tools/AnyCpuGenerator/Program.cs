using System.Reflection;

namespace AnyCpuGenerator
{
    public static class Program
    {
        [STAThread]
        public static int Main(string[] args)
        {
            if(args.Length < 2 || args.Length > 3)
            {
                Console.WriteLine("Usage: AnyCpuGenerator.exe <source assembly> <dest assembly> <key pair>");
                return 1;
            }

            var sourcePath = Path.GetFullPath(args[0]);
            var destPath = Path.GetFullPath(args[1]);
            var keyPairPath = args.Length > 2 ? Path.GetFullPath(args[2]) : null;

            if(!File.Exists(args[0]))
            {
                Console.WriteLine($"Source assembly {args[0]} does not exist.");
                return 2;
            }

            if(sourcePath.Equals(destPath, System.Runtime.InteropServices.RuntimeInformation.IsOSPlatform(System.Runtime.InteropServices.OSPlatform.Windows) ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
            {
                Console.WriteLine("The source assembly and the destination assembly cannot be the same.");
                return 3;
            }

            if(keyPairPath != null && !File.Exists(keyPairPath))
            {
                Console.WriteLine($"Invalid key pair: {keyPairPath}");
                return 4;
            }

            Mono.Cecil.ReaderParameters parms = new(Mono.Cecil.ReadingMode.Immediate);
            using var module = Mono.Cecil.ModuleDefinition.ReadModule(sourcePath, parms);

            if((module.Attributes & Mono.Cecil.ModuleAttributes.ILOnly) == 0)
            {
                Console.WriteLine("Cannot convert assembly to 'AnyCPU' because it isn't IL Only.");
                return 5;
            }

            var destDir = Path.GetDirectoryName(destPath);

            if(destDir != null && !Directory.Exists(destDir))
            {
                Directory.CreateDirectory(destDir);
            }

            // x64: Architecture = AMD64, Attributes = ILOnly
            // x86: Architecture = I386, Attributes = ILOnly | Required32Bit
            // MSIL: Architecture = I386, Attributes = ILOnly
            module.Architecture = Mono.Cecil.TargetArchitecture.I386;
            module.Attributes &= ~(Mono.Cecil.ModuleAttributes.Required32Bit | Mono.Cecil.ModuleAttributes.Preferred32Bit);

            if(keyPairPath != null)
            {
                var writeParms = new Mono.Cecil.WriterParameters() { StrongNameKeyContainer = keyPairPath };

                if(!writeParms.HasStrongNameKey)
                {
                    Console.WriteLine($"Failed to load the strong name key pair: {keyPairPath}");
                    return 6;
                }

                module.Attributes |= Mono.Cecil.ModuleAttributes.StrongNameSigned;
                module.Write(destPath, writeParms);
            }
            else
            {
                module.Attributes &= ~Mono.Cecil.ModuleAttributes.StrongNameSigned;
                module.Write(destPath);
            }

            return 0;
        }
    }
}