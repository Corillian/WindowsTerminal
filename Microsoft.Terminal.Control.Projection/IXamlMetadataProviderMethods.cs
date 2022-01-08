using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;
using WinRT;

namespace ABI.Microsoft.UI.Xaml.Markup
{
    internal static  class IXamlMetadataProviderMethods
    {
        public static global::Microsoft.UI.Xaml.Markup.IXamlType GetXamlType(IObjectReference obj, global::System.Type type)
        {
            var xamlMetadatProvider = obj.AsInterface<global::Microsoft.UI.Xaml.Markup.IXamlMetadataProvider>();

            if(xamlMetadatProvider != null)
            {
                return xamlMetadatProvider.GetXamlType(type);
            }

            throw new NotSupportedException();
        }

        public static global::Microsoft.UI.Xaml.Markup.IXamlType GetXamlType(IObjectReference obj, string fullName)
        {
            var xamlMetadatProvider = obj.AsInterface<global::Microsoft.UI.Xaml.Markup.IXamlMetadataProvider>();
            
            if(xamlMetadatProvider != null)
            {
                return xamlMetadatProvider.GetXamlType(fullName);
            }
            
            throw new NotSupportedException();
        }

        public static global::Microsoft.UI.Xaml.Markup.XmlnsDefinition[] GetXmlnsDefinitions(IObjectReference obj)
        {
            var xamlMetadatProvider = obj.AsInterface<global::Microsoft.UI.Xaml.Markup.IXamlMetadataProvider>();

            if(xamlMetadatProvider != null)
            {
                return xamlMetadatProvider.GetXmlnsDefinitions();
            }

            throw new NotSupportedException();
        }
    }
}
