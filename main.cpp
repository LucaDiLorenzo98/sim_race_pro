// =============================================================================
// main.cpp  -  Application entry point
// =============================================================================
// SimRacePro Custom Driver Software
//
// Architecture overview
// ---------------------
// The application is split into two independent components that run on
// separate threads:
//
//   GUI thread  (this file)  - Creates the Win32 window, runs the standard
//                              Windows message loop, and handles all UI events.
//
//   Backend thread           - Manages the USB serial connection to the wheel
//                              box, reads telemetry from every supported game,
//                              computes force-feedback values, and sends them
//                              to the hardware. Defined in SimRacePro.cpp.
//
// Both components share a GuiState struct whose members are either protected
// by a mutex or declared std::atomic to guarantee thread safety.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _USE_MATH_DEFINES

#include "SimRacePro.h"

// Set to true when the executable is launched with the -debug command-line flag.
// Enables the FFB Debug button in the GUI and activates detailed log-file output.
bool g_debugMode = false;

// Shared state between the GUI thread and the backend thread.
static GuiState g_gui;

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int nCmdShow) {
    // Check for the optional -debug command-line switch.
    if (lpCmdLine && strstr(lpCmdLine, "-debug"))
        g_debugMode = true;

    // Create the main window before starting the backend so the backend can
    // safely call PostMessage(g_gui.hwnd, ...) from the very first line it runs.
    g_gui.running = true;
    if (!CreateGuiWindow(hInst, nCmdShow, g_gui)) return 1;

    // Launch the backend on its own thread. We store the thread object (rather
    // than detaching) so we can attempt a clean join on exit.
    std::thread backendThread(runBackend, std::ref(g_gui));

    // Standard Win32 message loop - drives the entire GUI.
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Signal the backend to stop. The backend checks g_gui.running in its
    // main loop and will exit cleanly on its own in the normal case.
    g_gui.running = false;

    // Wait up to 2 seconds for the backend to finish. joinable() only reflects
    // whether join()/detach() has been called - not whether the thread function
    // has returned - so it can't be used to poll for completion. Wait on the
    // native handle instead: if it's signaled within the timeout the thread
    // has actually finished and join() returns immediately; otherwise detach
    // rather than hang the process shutdown.
    if (backendThread.joinable()) {
        if (WaitForSingleObject(backendThread.native_handle(), 2000) == WAIT_OBJECT_0)
            backendThread.join();
        else
            backendThread.detach();
    }

    return (int)msg.wParam;
}
