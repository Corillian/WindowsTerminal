using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using WinRT;

namespace ABI.Microsoft.UI.Xaml.Data
{
    internal static class IValueConverterMethods
    {
        public static object Convert(IObjectReference obj, object value, global::System.Type targetType, object parameter, string language)
        {
            var converter = obj.AsInterface<global::Microsoft.UI.Xaml.Data.IValueConverter>();

            if(converter != null)
            {
                return converter.Convert(value, targetType, parameter, language);
            }

            throw new NotSupportedException();
        }

        public static object ConvertBack(IObjectReference obj, object value, global::System.Type targetType, object parameter, string language)
        {
            var converter = obj.AsInterface<global::Microsoft.UI.Xaml.Data.IValueConverter>();

            if(converter != null)
            {
                return converter.Convert(value, targetType, parameter, language);
            }

            throw new NotSupportedException();
        }
    }
}
