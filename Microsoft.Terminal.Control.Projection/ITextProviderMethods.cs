using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using WinRT;

namespace ABI.Microsoft.UI.Xaml.Automation.Provider
{
    internal static  class ITextProviderMethods
    {
        public static global::Microsoft.UI.Xaml.Automation.Provider.ITextRangeProvider[]  GetSelection(IObjectReference obj)
        {
            var textProvider = obj.AsInterface<global::Microsoft.UI.Xaml.Automation.Provider.ITextProvider>();

            if(textProvider != null)
            {
                return textProvider.GetSelection();
            }

            throw new NotSupportedException();
        }

        public static global::Microsoft.UI.Xaml.Automation.Provider.ITextRangeProvider[] GetVisibleRanges(IObjectReference obj)
        {
            var textProvider = obj.AsInterface<global::Microsoft.UI.Xaml.Automation.Provider.ITextProvider>();

            if(textProvider != null)
            {
                return textProvider.GetVisibleRanges();
            }

            throw new NotSupportedException();
        }

        public static global::Microsoft.UI.Xaml.Automation.Provider.ITextRangeProvider RangeFromChild(IObjectReference obj, global::Microsoft.UI.Xaml.Automation.Provider.IRawElementProviderSimple childElement)
        {
            var textProvider = obj.AsInterface<global::Microsoft.UI.Xaml.Automation.Provider.ITextProvider>();

            if(textProvider != null)
            {
                return textProvider.RangeFromChild(childElement);
            }

            throw new NotSupportedException();
        }

        public static global::Microsoft.UI.Xaml.Automation.Provider.ITextRangeProvider RangeFromPoint(IObjectReference obj, global::Windows.Foundation.Point screenLocation)
        {
            var textProvider = obj.AsInterface<global::Microsoft.UI.Xaml.Automation.Provider.ITextProvider>();

            if(textProvider != null)
            {
                return textProvider.RangeFromPoint(screenLocation);
            }

            throw new NotSupportedException();
        }

        public static global::Microsoft.UI.Xaml.Automation.Provider.ITextRangeProvider get_DocumentRange(IObjectReference obj)
        {
            var textProvider = obj.AsInterface<global::Microsoft.UI.Xaml.Automation.Provider.ITextProvider>();

            if(textProvider != null)
            {
                return textProvider.DocumentRange;
            }

            throw new NotSupportedException();
        }

        public static global::Microsoft.UI.Xaml.Automation.SupportedTextSelection get_SupportedTextSelection(IObjectReference obj)
        {
            var textProvider = obj.AsInterface<global::Microsoft.UI.Xaml.Automation.Provider.ITextProvider>();

            if(textProvider != null)
            {
                return textProvider.SupportedTextSelection;
            }

            throw new NotSupportedException();
        }
    }
}
