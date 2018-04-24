#ifdef SLIC3R_GUI
#include "GUI.hpp"
#include "AboutDialog.hpp"

#if __APPLE__
#import <IOKit/pwr_mgt/IOPMLib.h>
#elif _WIN32
#include <Windows.h>
#pragma comment(lib, "user32.lib")
#endif
#include <string>

namespace Slic3r { namespace GUI {

std::string VAR_PATH;

void
about()
{
    AboutDialog dlg;
    dlg.ShowModal();
    dlg.Destroy();
}

#if __APPLE__
IOPMAssertionID assertionID;
#endif

void
disable_screensaver()
{
    #if __APPLE__
    CFStringRef reasonForActivity = CFSTR("Slic3r");
    IOReturn success = IOPMAssertionCreateWithName(kIOPMAssertionTypeNoDisplaySleep, 
        kIOPMAssertionLevelOn, reasonForActivity, &assertionID); 
    // ignore result: success == kIOReturnSuccess
    #elif _WIN32
    SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_CONTINUOUS);
    #endif
}

void
enable_screensaver()
{
    #if __APPLE__
    IOReturn success = IOPMAssertionRelease(assertionID);
    #elif _WIN32
    SetThreadExecutionState(ES_CONTINUOUS);
    #endif
}

} }
#endif
