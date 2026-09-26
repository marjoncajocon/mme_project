/*
** sdlstub.c - every function SDL2's headers declare, empty: built into a
** library named as a system's SDL2 is (libSDL2-2.0.so.0, libSDL2-2.0.0.dylib),
** so build.bat links mme-sdl for Linux and macOS without their SDL2 at hand.
** The program loads the real one where it runs. Made from SDL 2.32.10's headers.
*/

void SDL_AddEventWatch (void);
void SDL_AddEventWatch (void) {}
void SDL_AddHintCallback (void);
void SDL_AddHintCallback (void) {}
void SDL_AddTimer (void);
void SDL_AddTimer (void) {}
void SDL_AllocFormat (void);
void SDL_AllocFormat (void) {}
void SDL_AllocPalette (void);
void SDL_AllocPalette (void) {}
void SDL_AllocRW (void);
void SDL_AllocRW (void) {}
void SDL_AndroidBackButton (void);
void SDL_AndroidBackButton (void) {}
void SDL_AndroidGetActivity (void);
void SDL_AndroidGetActivity (void) {}
void SDL_AndroidGetExternalStoragePath (void);
void SDL_AndroidGetExternalStoragePath (void) {}
void SDL_AndroidGetExternalStorageState (void);
void SDL_AndroidGetExternalStorageState (void) {}
void SDL_AndroidGetInternalStoragePath (void);
void SDL_AndroidGetInternalStoragePath (void) {}
void SDL_AndroidGetJNIEnv (void);
void SDL_AndroidGetJNIEnv (void) {}
void SDL_AndroidRequestPermission (void);
void SDL_AndroidRequestPermission (void) {}
void SDL_AndroidSendMessage (void);
void SDL_AndroidSendMessage (void) {}
void SDL_AndroidShowToast (void);
void SDL_AndroidShowToast (void) {}
void SDL_AtomicAdd (void);
void SDL_AtomicAdd (void) {}
void SDL_AtomicCAS (void);
void SDL_AtomicCAS (void) {}
void SDL_AtomicCASPtr (void);
void SDL_AtomicCASPtr (void) {}
void SDL_AtomicGet (void);
void SDL_AtomicGet (void) {}
void SDL_AtomicGetPtr (void);
void SDL_AtomicGetPtr (void) {}
void SDL_AtomicLock (void);
void SDL_AtomicLock (void) {}
void SDL_AtomicSet (void);
void SDL_AtomicSet (void) {}
void SDL_AtomicSetPtr (void);
void SDL_AtomicSetPtr (void) {}
void SDL_AtomicTryLock (void);
void SDL_AtomicTryLock (void) {}
void SDL_AtomicUnlock (void);
void SDL_AtomicUnlock (void) {}
void SDL_AudioInit (void);
void SDL_AudioInit (void) {}
void SDL_AudioQuit (void);
void SDL_AudioQuit (void) {}
void SDL_AudioStreamAvailable (void);
void SDL_AudioStreamAvailable (void) {}
void SDL_AudioStreamClear (void);
void SDL_AudioStreamClear (void) {}
void SDL_AudioStreamFlush (void);
void SDL_AudioStreamFlush (void) {}
void SDL_AudioStreamGet (void);
void SDL_AudioStreamGet (void) {}
void SDL_AudioStreamPut (void);
void SDL_AudioStreamPut (void) {}
void SDL_BuildAudioCVT (void);
void SDL_BuildAudioCVT (void) {}
void SDL_CalculateGammaRamp (void);
void SDL_CalculateGammaRamp (void) {}
void SDL_CaptureMouse (void);
void SDL_CaptureMouse (void) {}
void SDL_ClearComposition (void);
void SDL_ClearComposition (void) {}
void SDL_ClearError (void);
void SDL_ClearError (void) {}
void SDL_ClearHints (void);
void SDL_ClearHints (void) {}
void SDL_ClearQueuedAudio (void);
void SDL_ClearQueuedAudio (void) {}
void SDL_CloseAudio (void);
void SDL_CloseAudio (void) {}
void SDL_CloseAudioDevice (void);
void SDL_CloseAudioDevice (void) {}
void SDL_ComposeCustomBlendMode (void);
void SDL_ComposeCustomBlendMode (void) {}
void SDL_CondBroadcast (void);
void SDL_CondBroadcast (void) {}
void SDL_CondSignal (void);
void SDL_CondSignal (void) {}
void SDL_CondWait (void);
void SDL_CondWait (void) {}
void SDL_CondWaitTimeout (void);
void SDL_CondWaitTimeout (void) {}
void SDL_ConvertAudio (void);
void SDL_ConvertAudio (void) {}
void SDL_ConvertPixels (void);
void SDL_ConvertPixels (void) {}
void SDL_ConvertSurface (void);
void SDL_ConvertSurface (void) {}
void SDL_ConvertSurfaceFormat (void);
void SDL_ConvertSurfaceFormat (void) {}
void SDL_CreateColorCursor (void);
void SDL_CreateColorCursor (void) {}
void SDL_CreateCond (void);
void SDL_CreateCond (void) {}
void SDL_CreateCursor (void);
void SDL_CreateCursor (void) {}
void SDL_CreateMutex (void);
void SDL_CreateMutex (void) {}
void SDL_CreateRGBSurface (void);
void SDL_CreateRGBSurface (void) {}
void SDL_CreateRGBSurfaceFrom (void);
void SDL_CreateRGBSurfaceFrom (void) {}
void SDL_CreateRGBSurfaceWithFormat (void);
void SDL_CreateRGBSurfaceWithFormat (void) {}
void SDL_CreateRGBSurfaceWithFormatFrom (void);
void SDL_CreateRGBSurfaceWithFormatFrom (void) {}
void SDL_CreateRenderer (void);
void SDL_CreateRenderer (void) {}
void SDL_CreateSemaphore (void);
void SDL_CreateSemaphore (void) {}
void SDL_CreateShapedWindow (void);
void SDL_CreateShapedWindow (void) {}
void SDL_CreateSoftwareRenderer (void);
void SDL_CreateSoftwareRenderer (void) {}
void SDL_CreateSystemCursor (void);
void SDL_CreateSystemCursor (void) {}
void SDL_CreateTexture (void);
void SDL_CreateTexture (void) {}
void SDL_CreateTextureFromSurface (void);
void SDL_CreateTextureFromSurface (void) {}
void SDL_CreateThread (void);
void SDL_CreateThread (void) {}
void SDL_CreateThreadWithStackSize (void);
void SDL_CreateThreadWithStackSize (void) {}
void SDL_CreateWindow (void);
void SDL_CreateWindow (void) {}
void SDL_CreateWindowAndRenderer (void);
void SDL_CreateWindowAndRenderer (void) {}
void SDL_CreateWindowFrom (void);
void SDL_CreateWindowFrom (void) {}
void SDL_DXGIGetOutputInfo (void);
void SDL_DXGIGetOutputInfo (void) {}
void SDL_DelEventWatch (void);
void SDL_DelEventWatch (void) {}
void SDL_DelHintCallback (void);
void SDL_DelHintCallback (void) {}
void SDL_Delay (void);
void SDL_Delay (void) {}
void SDL_DequeueAudio (void);
void SDL_DequeueAudio (void) {}
void SDL_DestroyCond (void);
void SDL_DestroyCond (void) {}
void SDL_DestroyMutex (void);
void SDL_DestroyMutex (void) {}
void SDL_DestroyRenderer (void);
void SDL_DestroyRenderer (void) {}
void SDL_DestroySemaphore (void);
void SDL_DestroySemaphore (void) {}
void SDL_DestroyTexture (void);
void SDL_DestroyTexture (void) {}
void SDL_DestroyWindow (void);
void SDL_DestroyWindow (void) {}
void SDL_DestroyWindowSurface (void);
void SDL_DestroyWindowSurface (void) {}
void SDL_DetachThread (void);
void SDL_DetachThread (void) {}
void SDL_Direct3D9GetAdapterIndex (void);
void SDL_Direct3D9GetAdapterIndex (void) {}
void SDL_DisableScreenSaver (void);
void SDL_DisableScreenSaver (void) {}
void SDL_DuplicateSurface (void);
void SDL_DuplicateSurface (void) {}
void SDL_EnableScreenSaver (void);
void SDL_EnableScreenSaver (void) {}
void SDL_EncloseFPoints (void);
void SDL_EncloseFPoints (void) {}
void SDL_EnclosePoints (void);
void SDL_EnclosePoints (void) {}
void SDL_Error (void);
void SDL_Error (void) {}
void SDL_EventState (void);
void SDL_EventState (void) {}
void SDL_FillRect (void);
void SDL_FillRect (void) {}
void SDL_FillRects (void);
void SDL_FillRects (void) {}
void SDL_FilterEvents (void);
void SDL_FilterEvents (void) {}
void SDL_FlashWindow (void);
void SDL_FlashWindow (void) {}
void SDL_FlushEvent (void);
void SDL_FlushEvent (void) {}
void SDL_FlushEvents (void);
void SDL_FlushEvents (void) {}
void SDL_FreeAudioStream (void);
void SDL_FreeAudioStream (void) {}
void SDL_FreeCursor (void);
void SDL_FreeCursor (void) {}
void SDL_FreeFormat (void);
void SDL_FreeFormat (void) {}
void SDL_FreePalette (void);
void SDL_FreePalette (void) {}
void SDL_FreeRW (void);
void SDL_FreeRW (void) {}
void SDL_FreeSurface (void);
void SDL_FreeSurface (void) {}
void SDL_FreeWAV (void);
void SDL_FreeWAV (void) {}
void SDL_GDKGetDefaultUser (void);
void SDL_GDKGetDefaultUser (void) {}
void SDL_GDKGetTaskQueue (void);
void SDL_GDKGetTaskQueue (void) {}
void SDL_GDKRunApp (void);
void SDL_GDKRunApp (void) {}
void SDL_GDKSuspendComplete (void);
void SDL_GDKSuspendComplete (void) {}
void SDL_GL_BindTexture (void);
void SDL_GL_BindTexture (void) {}
void SDL_GL_CreateContext (void);
void SDL_GL_CreateContext (void) {}
void SDL_GL_DeleteContext (void);
void SDL_GL_DeleteContext (void) {}
void SDL_GL_ExtensionSupported (void);
void SDL_GL_ExtensionSupported (void) {}
void SDL_GL_GetAttribute (void);
void SDL_GL_GetAttribute (void) {}
void SDL_GL_GetCurrentContext (void);
void SDL_GL_GetCurrentContext (void) {}
void SDL_GL_GetCurrentWindow (void);
void SDL_GL_GetCurrentWindow (void) {}
void SDL_GL_GetDrawableSize (void);
void SDL_GL_GetDrawableSize (void) {}
void SDL_GL_GetProcAddress (void);
void SDL_GL_GetProcAddress (void) {}
void SDL_GL_GetSwapInterval (void);
void SDL_GL_GetSwapInterval (void) {}
void SDL_GL_LoadLibrary (void);
void SDL_GL_LoadLibrary (void) {}
void SDL_GL_MakeCurrent (void);
void SDL_GL_MakeCurrent (void) {}
void SDL_GL_ResetAttributes (void);
void SDL_GL_ResetAttributes (void) {}
void SDL_GL_SetAttribute (void);
void SDL_GL_SetAttribute (void) {}
void SDL_GL_SetSwapInterval (void);
void SDL_GL_SetSwapInterval (void) {}
void SDL_GL_SwapWindow (void);
void SDL_GL_SwapWindow (void) {}
void SDL_GL_UnbindTexture (void);
void SDL_GL_UnbindTexture (void) {}
void SDL_GL_UnloadLibrary (void);
void SDL_GL_UnloadLibrary (void) {}
void SDL_GUIDFromString (void);
void SDL_GUIDFromString (void) {}
void SDL_GUIDToString (void);
void SDL_GUIDToString (void) {}
void SDL_GameControllerAddMapping (void);
void SDL_GameControllerAddMapping (void) {}
void SDL_GameControllerAddMappingsFromRW (void);
void SDL_GameControllerAddMappingsFromRW (void) {}
void SDL_GameControllerClose (void);
void SDL_GameControllerClose (void) {}
void SDL_GameControllerEventState (void);
void SDL_GameControllerEventState (void) {}
void SDL_GameControllerFromInstanceID (void);
void SDL_GameControllerFromInstanceID (void) {}
void SDL_GameControllerFromPlayerIndex (void);
void SDL_GameControllerFromPlayerIndex (void) {}
void SDL_GameControllerGetAppleSFSymbolsNameForAxis (void);
void SDL_GameControllerGetAppleSFSymbolsNameForAxis (void) {}
void SDL_GameControllerGetAppleSFSymbolsNameForButton (void);
void SDL_GameControllerGetAppleSFSymbolsNameForButton (void) {}
void SDL_GameControllerGetAttached (void);
void SDL_GameControllerGetAttached (void) {}
void SDL_GameControllerGetAxis (void);
void SDL_GameControllerGetAxis (void) {}
void SDL_GameControllerGetAxisFromString (void);
void SDL_GameControllerGetAxisFromString (void) {}
void SDL_GameControllerGetBindForAxis (void);
void SDL_GameControllerGetBindForAxis (void) {}
void SDL_GameControllerGetBindForButton (void);
void SDL_GameControllerGetBindForButton (void) {}
void SDL_GameControllerGetButton (void);
void SDL_GameControllerGetButton (void) {}
void SDL_GameControllerGetButtonFromString (void);
void SDL_GameControllerGetButtonFromString (void) {}
void SDL_GameControllerGetFirmwareVersion (void);
void SDL_GameControllerGetFirmwareVersion (void) {}
void SDL_GameControllerGetJoystick (void);
void SDL_GameControllerGetJoystick (void) {}
void SDL_GameControllerGetNumTouchpadFingers (void);
void SDL_GameControllerGetNumTouchpadFingers (void) {}
void SDL_GameControllerGetNumTouchpads (void);
void SDL_GameControllerGetNumTouchpads (void) {}
void SDL_GameControllerGetPlayerIndex (void);
void SDL_GameControllerGetPlayerIndex (void) {}
void SDL_GameControllerGetProduct (void);
void SDL_GameControllerGetProduct (void) {}
void SDL_GameControllerGetProductVersion (void);
void SDL_GameControllerGetProductVersion (void) {}
void SDL_GameControllerGetSensorData (void);
void SDL_GameControllerGetSensorData (void) {}
void SDL_GameControllerGetSensorDataRate (void);
void SDL_GameControllerGetSensorDataRate (void) {}
void SDL_GameControllerGetSensorDataWithTimestamp (void);
void SDL_GameControllerGetSensorDataWithTimestamp (void) {}
void SDL_GameControllerGetSerial (void);
void SDL_GameControllerGetSerial (void) {}
void SDL_GameControllerGetSteamHandle (void);
void SDL_GameControllerGetSteamHandle (void) {}
void SDL_GameControllerGetStringForAxis (void);
void SDL_GameControllerGetStringForAxis (void) {}
void SDL_GameControllerGetStringForButton (void);
void SDL_GameControllerGetStringForButton (void) {}
void SDL_GameControllerGetTouchpadFinger (void);
void SDL_GameControllerGetTouchpadFinger (void) {}
void SDL_GameControllerGetType (void);
void SDL_GameControllerGetType (void) {}
void SDL_GameControllerGetVendor (void);
void SDL_GameControllerGetVendor (void) {}
void SDL_GameControllerHasAxis (void);
void SDL_GameControllerHasAxis (void) {}
void SDL_GameControllerHasButton (void);
void SDL_GameControllerHasButton (void) {}
void SDL_GameControllerHasLED (void);
void SDL_GameControllerHasLED (void) {}
void SDL_GameControllerHasRumble (void);
void SDL_GameControllerHasRumble (void) {}
void SDL_GameControllerHasRumbleTriggers (void);
void SDL_GameControllerHasRumbleTriggers (void) {}
void SDL_GameControllerHasSensor (void);
void SDL_GameControllerHasSensor (void) {}
void SDL_GameControllerIsSensorEnabled (void);
void SDL_GameControllerIsSensorEnabled (void) {}
void SDL_GameControllerMapping (void);
void SDL_GameControllerMapping (void) {}
void SDL_GameControllerMappingForDeviceIndex (void);
void SDL_GameControllerMappingForDeviceIndex (void) {}
void SDL_GameControllerMappingForGUID (void);
void SDL_GameControllerMappingForGUID (void) {}
void SDL_GameControllerMappingForIndex (void);
void SDL_GameControllerMappingForIndex (void) {}
void SDL_GameControllerName (void);
void SDL_GameControllerName (void) {}
void SDL_GameControllerNameForIndex (void);
void SDL_GameControllerNameForIndex (void) {}
void SDL_GameControllerNumMappings (void);
void SDL_GameControllerNumMappings (void) {}
void SDL_GameControllerOpen (void);
void SDL_GameControllerOpen (void) {}
void SDL_GameControllerPath (void);
void SDL_GameControllerPath (void) {}
void SDL_GameControllerPathForIndex (void);
void SDL_GameControllerPathForIndex (void) {}
void SDL_GameControllerRumble (void);
void SDL_GameControllerRumble (void) {}
void SDL_GameControllerRumbleTriggers (void);
void SDL_GameControllerRumbleTriggers (void) {}
void SDL_GameControllerSendEffect (void);
void SDL_GameControllerSendEffect (void) {}
void SDL_GameControllerSetLED (void);
void SDL_GameControllerSetLED (void) {}
void SDL_GameControllerSetPlayerIndex (void);
void SDL_GameControllerSetPlayerIndex (void) {}
void SDL_GameControllerSetSensorEnabled (void);
void SDL_GameControllerSetSensorEnabled (void) {}
void SDL_GameControllerTypeForIndex (void);
void SDL_GameControllerTypeForIndex (void) {}
void SDL_GameControllerUpdate (void);
void SDL_GameControllerUpdate (void) {}
void SDL_GetAndroidSDKVersion (void);
void SDL_GetAndroidSDKVersion (void) {}
void SDL_GetAssertionHandler (void);
void SDL_GetAssertionHandler (void) {}
void SDL_GetAssertionReport (void);
void SDL_GetAssertionReport (void) {}
void SDL_GetAudioDeviceName (void);
void SDL_GetAudioDeviceName (void) {}
void SDL_GetAudioDeviceSpec (void);
void SDL_GetAudioDeviceSpec (void) {}
void SDL_GetAudioDeviceStatus (void);
void SDL_GetAudioDeviceStatus (void) {}
void SDL_GetAudioDriver (void);
void SDL_GetAudioDriver (void) {}
void SDL_GetAudioStatus (void);
void SDL_GetAudioStatus (void) {}
void SDL_GetBasePath (void);
void SDL_GetBasePath (void) {}
void SDL_GetCPUCacheLineSize (void);
void SDL_GetCPUCacheLineSize (void) {}
void SDL_GetCPUCount (void);
void SDL_GetCPUCount (void) {}
void SDL_GetClipRect (void);
void SDL_GetClipRect (void) {}
void SDL_GetClipboardText (void);
void SDL_GetClipboardText (void) {}
void SDL_GetClosestDisplayMode (void);
void SDL_GetClosestDisplayMode (void) {}
void SDL_GetColorKey (void);
void SDL_GetColorKey (void) {}
void SDL_GetCurrentAudioDriver (void);
void SDL_GetCurrentAudioDriver (void) {}
void SDL_GetCurrentDisplayMode (void);
void SDL_GetCurrentDisplayMode (void) {}
void SDL_GetCurrentVideoDriver (void);
void SDL_GetCurrentVideoDriver (void) {}
void SDL_GetCursor (void);
void SDL_GetCursor (void) {}
void SDL_GetDefaultAssertionHandler (void);
void SDL_GetDefaultAssertionHandler (void) {}
void SDL_GetDefaultAudioInfo (void);
void SDL_GetDefaultAudioInfo (void) {}
void SDL_GetDefaultCursor (void);
void SDL_GetDefaultCursor (void) {}
void SDL_GetDesktopDisplayMode (void);
void SDL_GetDesktopDisplayMode (void) {}
void SDL_GetDisplayBounds (void);
void SDL_GetDisplayBounds (void) {}
void SDL_GetDisplayDPI (void);
void SDL_GetDisplayDPI (void) {}
void SDL_GetDisplayMode (void);
void SDL_GetDisplayMode (void) {}
void SDL_GetDisplayName (void);
void SDL_GetDisplayName (void) {}
void SDL_GetDisplayOrientation (void);
void SDL_GetDisplayOrientation (void) {}
void SDL_GetDisplayUsableBounds (void);
void SDL_GetDisplayUsableBounds (void) {}
void SDL_GetError (void);
void SDL_GetError (void) {}
void SDL_GetErrorMsg (void);
void SDL_GetErrorMsg (void) {}
void SDL_GetEventFilter (void);
void SDL_GetEventFilter (void) {}
void SDL_GetGlobalMouseState (void);
void SDL_GetGlobalMouseState (void) {}
void SDL_GetGrabbedWindow (void);
void SDL_GetGrabbedWindow (void) {}
void SDL_GetHint (void);
void SDL_GetHint (void) {}
void SDL_GetHintBoolean (void);
void SDL_GetHintBoolean (void) {}
void SDL_GetJoystickGUIDInfo (void);
void SDL_GetJoystickGUIDInfo (void) {}
void SDL_GetKeyFromName (void);
void SDL_GetKeyFromName (void) {}
void SDL_GetKeyFromScancode (void);
void SDL_GetKeyFromScancode (void) {}
void SDL_GetKeyName (void);
void SDL_GetKeyName (void) {}
void SDL_GetKeyboardFocus (void);
void SDL_GetKeyboardFocus (void) {}
void SDL_GetKeyboardState (void);
void SDL_GetKeyboardState (void) {}
void SDL_GetMemoryFunctions (void);
void SDL_GetMemoryFunctions (void) {}
void SDL_GetModState (void);
void SDL_GetModState (void) {}
void SDL_GetMouseFocus (void);
void SDL_GetMouseFocus (void) {}
void SDL_GetMouseState (void);
void SDL_GetMouseState (void) {}
void SDL_GetNumAllocations (void);
void SDL_GetNumAllocations (void) {}
void SDL_GetNumAudioDevices (void);
void SDL_GetNumAudioDevices (void) {}
void SDL_GetNumAudioDrivers (void);
void SDL_GetNumAudioDrivers (void) {}
void SDL_GetNumDisplayModes (void);
void SDL_GetNumDisplayModes (void) {}
void SDL_GetNumRenderDrivers (void);
void SDL_GetNumRenderDrivers (void) {}
void SDL_GetNumTouchDevices (void);
void SDL_GetNumTouchDevices (void) {}
void SDL_GetNumTouchFingers (void);
void SDL_GetNumTouchFingers (void) {}
void SDL_GetNumVideoDisplays (void);
void SDL_GetNumVideoDisplays (void) {}
void SDL_GetNumVideoDrivers (void);
void SDL_GetNumVideoDrivers (void) {}
void SDL_GetOriginalMemoryFunctions (void);
void SDL_GetOriginalMemoryFunctions (void) {}
void SDL_GetPerformanceCounter (void);
void SDL_GetPerformanceCounter (void) {}
void SDL_GetPerformanceFrequency (void);
void SDL_GetPerformanceFrequency (void) {}
void SDL_GetPixelFormatName (void);
void SDL_GetPixelFormatName (void) {}
void SDL_GetPlatform (void);
void SDL_GetPlatform (void) {}
void SDL_GetPointDisplayIndex (void);
void SDL_GetPointDisplayIndex (void) {}
void SDL_GetPowerInfo (void);
void SDL_GetPowerInfo (void) {}
void SDL_GetPrefPath (void);
void SDL_GetPrefPath (void) {}
void SDL_GetPreferredLocales (void);
void SDL_GetPreferredLocales (void) {}
void SDL_GetPrimarySelectionText (void);
void SDL_GetPrimarySelectionText (void) {}
void SDL_GetQueuedAudioSize (void);
void SDL_GetQueuedAudioSize (void) {}
void SDL_GetRGB (void);
void SDL_GetRGB (void) {}
void SDL_GetRGBA (void);
void SDL_GetRGBA (void) {}
void SDL_GetRectDisplayIndex (void);
void SDL_GetRectDisplayIndex (void) {}
void SDL_GetRelativeMouseMode (void);
void SDL_GetRelativeMouseMode (void) {}
void SDL_GetRelativeMouseState (void);
void SDL_GetRelativeMouseState (void) {}
void SDL_GetRenderDrawBlendMode (void);
void SDL_GetRenderDrawBlendMode (void) {}
void SDL_GetRenderDrawColor (void);
void SDL_GetRenderDrawColor (void) {}
void SDL_GetRenderDriverInfo (void);
void SDL_GetRenderDriverInfo (void) {}
void SDL_GetRenderTarget (void);
void SDL_GetRenderTarget (void) {}
void SDL_GetRenderer (void);
void SDL_GetRenderer (void) {}
void SDL_GetRendererInfo (void);
void SDL_GetRendererInfo (void) {}
void SDL_GetRendererOutputSize (void);
void SDL_GetRendererOutputSize (void) {}
void SDL_GetRevision (void);
void SDL_GetRevision (void) {}
void SDL_GetScancodeFromKey (void);
void SDL_GetScancodeFromKey (void) {}
void SDL_GetScancodeFromName (void);
void SDL_GetScancodeFromName (void) {}
void SDL_GetScancodeName (void);
void SDL_GetScancodeName (void) {}
void SDL_GetShapedWindowMode (void);
void SDL_GetShapedWindowMode (void) {}
void SDL_GetSurfaceAlphaMod (void);
void SDL_GetSurfaceAlphaMod (void) {}
void SDL_GetSurfaceBlendMode (void);
void SDL_GetSurfaceBlendMode (void) {}
void SDL_GetSurfaceColorMod (void);
void SDL_GetSurfaceColorMod (void) {}
void SDL_GetSystemRAM (void);
void SDL_GetSystemRAM (void) {}
void SDL_GetTextureAlphaMod (void);
void SDL_GetTextureAlphaMod (void) {}
void SDL_GetTextureBlendMode (void);
void SDL_GetTextureBlendMode (void) {}
void SDL_GetTextureColorMod (void);
void SDL_GetTextureColorMod (void) {}
void SDL_GetTextureScaleMode (void);
void SDL_GetTextureScaleMode (void) {}
void SDL_GetTextureUserData (void);
void SDL_GetTextureUserData (void) {}
void SDL_GetThreadID (void);
void SDL_GetThreadID (void) {}
void SDL_GetThreadName (void);
void SDL_GetThreadName (void) {}
void SDL_GetTicks (void);
void SDL_GetTicks (void) {}
void SDL_GetTicks64 (void);
void SDL_GetTicks64 (void) {}
void SDL_GetTouchDevice (void);
void SDL_GetTouchDevice (void) {}
void SDL_GetTouchDeviceType (void);
void SDL_GetTouchDeviceType (void) {}
void SDL_GetTouchFinger (void);
void SDL_GetTouchFinger (void) {}
void SDL_GetTouchName (void);
void SDL_GetTouchName (void) {}
void SDL_GetVersion (void);
void SDL_GetVersion (void) {}
void SDL_GetVideoDriver (void);
void SDL_GetVideoDriver (void) {}
void SDL_GetWindowBordersSize (void);
void SDL_GetWindowBordersSize (void) {}
void SDL_GetWindowBrightness (void);
void SDL_GetWindowBrightness (void) {}
void SDL_GetWindowData (void);
void SDL_GetWindowData (void) {}
void SDL_GetWindowDisplayIndex (void);
void SDL_GetWindowDisplayIndex (void) {}
void SDL_GetWindowDisplayMode (void);
void SDL_GetWindowDisplayMode (void) {}
void SDL_GetWindowFlags (void);
void SDL_GetWindowFlags (void) {}
void SDL_GetWindowFromID (void);
void SDL_GetWindowFromID (void) {}
void SDL_GetWindowGammaRamp (void);
void SDL_GetWindowGammaRamp (void) {}
void SDL_GetWindowGrab (void);
void SDL_GetWindowGrab (void) {}
void SDL_GetWindowICCProfile (void);
void SDL_GetWindowICCProfile (void) {}
void SDL_GetWindowID (void);
void SDL_GetWindowID (void) {}
void SDL_GetWindowKeyboardGrab (void);
void SDL_GetWindowKeyboardGrab (void) {}
void SDL_GetWindowMaximumSize (void);
void SDL_GetWindowMaximumSize (void) {}
void SDL_GetWindowMinimumSize (void);
void SDL_GetWindowMinimumSize (void) {}
void SDL_GetWindowMouseGrab (void);
void SDL_GetWindowMouseGrab (void) {}
void SDL_GetWindowMouseRect (void);
void SDL_GetWindowMouseRect (void) {}
void SDL_GetWindowOpacity (void);
void SDL_GetWindowOpacity (void) {}
void SDL_GetWindowPixelFormat (void);
void SDL_GetWindowPixelFormat (void) {}
void SDL_GetWindowPosition (void);
void SDL_GetWindowPosition (void) {}
void SDL_GetWindowSize (void);
void SDL_GetWindowSize (void) {}
void SDL_GetWindowSizeInPixels (void);
void SDL_GetWindowSizeInPixels (void) {}
void SDL_GetWindowSurface (void);
void SDL_GetWindowSurface (void) {}
void SDL_GetWindowTitle (void);
void SDL_GetWindowTitle (void) {}
void SDL_GetWindowWMInfo (void);
void SDL_GetWindowWMInfo (void) {}
void SDL_GetYUVConversionMode (void);
void SDL_GetYUVConversionMode (void) {}
void SDL_GetYUVConversionModeForResolution (void);
void SDL_GetYUVConversionModeForResolution (void) {}
void SDL_HapticClose (void);
void SDL_HapticClose (void) {}
void SDL_HapticDestroyEffect (void);
void SDL_HapticDestroyEffect (void) {}
void SDL_HapticEffectSupported (void);
void SDL_HapticEffectSupported (void) {}
void SDL_HapticGetEffectStatus (void);
void SDL_HapticGetEffectStatus (void) {}
void SDL_HapticIndex (void);
void SDL_HapticIndex (void) {}
void SDL_HapticName (void);
void SDL_HapticName (void) {}
void SDL_HapticNewEffect (void);
void SDL_HapticNewEffect (void) {}
void SDL_HapticNumAxes (void);
void SDL_HapticNumAxes (void) {}
void SDL_HapticNumEffects (void);
void SDL_HapticNumEffects (void) {}
void SDL_HapticNumEffectsPlaying (void);
void SDL_HapticNumEffectsPlaying (void) {}
void SDL_HapticOpen (void);
void SDL_HapticOpen (void) {}
void SDL_HapticOpenFromJoystick (void);
void SDL_HapticOpenFromJoystick (void) {}
void SDL_HapticOpenFromMouse (void);
void SDL_HapticOpenFromMouse (void) {}
void SDL_HapticOpened (void);
void SDL_HapticOpened (void) {}
void SDL_HapticPause (void);
void SDL_HapticPause (void) {}
void SDL_HapticQuery (void);
void SDL_HapticQuery (void) {}
void SDL_HapticRumbleInit (void);
void SDL_HapticRumbleInit (void) {}
void SDL_HapticRumblePlay (void);
void SDL_HapticRumblePlay (void) {}
void SDL_HapticRumbleStop (void);
void SDL_HapticRumbleStop (void) {}
void SDL_HapticRumbleSupported (void);
void SDL_HapticRumbleSupported (void) {}
void SDL_HapticRunEffect (void);
void SDL_HapticRunEffect (void) {}
void SDL_HapticSetAutocenter (void);
void SDL_HapticSetAutocenter (void) {}
void SDL_HapticSetGain (void);
void SDL_HapticSetGain (void) {}
void SDL_HapticStopAll (void);
void SDL_HapticStopAll (void) {}
void SDL_HapticStopEffect (void);
void SDL_HapticStopEffect (void) {}
void SDL_HapticUnpause (void);
void SDL_HapticUnpause (void) {}
void SDL_HapticUpdateEffect (void);
void SDL_HapticUpdateEffect (void) {}
void SDL_Has3DNow (void);
void SDL_Has3DNow (void) {}
void SDL_HasARMSIMD (void);
void SDL_HasARMSIMD (void) {}
void SDL_HasAVX (void);
void SDL_HasAVX (void) {}
void SDL_HasAVX2 (void);
void SDL_HasAVX2 (void) {}
void SDL_HasAVX512F (void);
void SDL_HasAVX512F (void) {}
void SDL_HasAltiVec (void);
void SDL_HasAltiVec (void) {}
void SDL_HasClipboardText (void);
void SDL_HasClipboardText (void) {}
void SDL_HasColorKey (void);
void SDL_HasColorKey (void) {}
void SDL_HasEvent (void);
void SDL_HasEvent (void) {}
void SDL_HasEvents (void);
void SDL_HasEvents (void) {}
void SDL_HasIntersection (void);
void SDL_HasIntersection (void) {}
void SDL_HasIntersectionF (void);
void SDL_HasIntersectionF (void) {}
void SDL_HasLASX (void);
void SDL_HasLASX (void) {}
void SDL_HasLSX (void);
void SDL_HasLSX (void) {}
void SDL_HasMMX (void);
void SDL_HasMMX (void) {}
void SDL_HasNEON (void);
void SDL_HasNEON (void) {}
void SDL_HasPrimarySelectionText (void);
void SDL_HasPrimarySelectionText (void) {}
void SDL_HasRDTSC (void);
void SDL_HasRDTSC (void) {}
void SDL_HasSSE (void);
void SDL_HasSSE (void) {}
void SDL_HasSSE2 (void);
void SDL_HasSSE2 (void) {}
void SDL_HasSSE3 (void);
void SDL_HasSSE3 (void) {}
void SDL_HasSSE41 (void);
void SDL_HasSSE41 (void) {}
void SDL_HasSSE42 (void);
void SDL_HasSSE42 (void) {}
void SDL_HasScreenKeyboardSupport (void);
void SDL_HasScreenKeyboardSupport (void) {}
void SDL_HasSurfaceRLE (void);
void SDL_HasSurfaceRLE (void) {}
void SDL_HasWindowSurface (void);
void SDL_HasWindowSurface (void) {}
void SDL_HideWindow (void);
void SDL_HideWindow (void) {}
void SDL_Init (void);
void SDL_Init (void) {}
void SDL_InitSubSystem (void);
void SDL_InitSubSystem (void) {}
void SDL_IntersectFRect (void);
void SDL_IntersectFRect (void) {}
void SDL_IntersectFRectAndLine (void);
void SDL_IntersectFRectAndLine (void) {}
void SDL_IntersectRect (void);
void SDL_IntersectRect (void) {}
void SDL_IntersectRectAndLine (void);
void SDL_IntersectRectAndLine (void) {}
void SDL_IsAndroidTV (void);
void SDL_IsAndroidTV (void) {}
void SDL_IsChromebook (void);
void SDL_IsChromebook (void) {}
void SDL_IsDeXMode (void);
void SDL_IsDeXMode (void) {}
void SDL_IsGameController (void);
void SDL_IsGameController (void) {}
void SDL_IsScreenKeyboardShown (void);
void SDL_IsScreenKeyboardShown (void) {}
void SDL_IsScreenSaverEnabled (void);
void SDL_IsScreenSaverEnabled (void) {}
void SDL_IsShapedWindow (void);
void SDL_IsShapedWindow (void) {}
void SDL_IsTablet (void);
void SDL_IsTablet (void) {}
void SDL_IsTextInputActive (void);
void SDL_IsTextInputActive (void) {}
void SDL_IsTextInputShown (void);
void SDL_IsTextInputShown (void) {}
void SDL_JoystickAttachVirtual (void);
void SDL_JoystickAttachVirtual (void) {}
void SDL_JoystickAttachVirtualEx (void);
void SDL_JoystickAttachVirtualEx (void) {}
void SDL_JoystickClose (void);
void SDL_JoystickClose (void) {}
void SDL_JoystickCurrentPowerLevel (void);
void SDL_JoystickCurrentPowerLevel (void) {}
void SDL_JoystickDetachVirtual (void);
void SDL_JoystickDetachVirtual (void) {}
void SDL_JoystickEventState (void);
void SDL_JoystickEventState (void) {}
void SDL_JoystickFromInstanceID (void);
void SDL_JoystickFromInstanceID (void) {}
void SDL_JoystickFromPlayerIndex (void);
void SDL_JoystickFromPlayerIndex (void) {}
void SDL_JoystickGetAttached (void);
void SDL_JoystickGetAttached (void) {}
void SDL_JoystickGetAxis (void);
void SDL_JoystickGetAxis (void) {}
void SDL_JoystickGetAxisInitialState (void);
void SDL_JoystickGetAxisInitialState (void) {}
void SDL_JoystickGetBall (void);
void SDL_JoystickGetBall (void) {}
void SDL_JoystickGetButton (void);
void SDL_JoystickGetButton (void) {}
void SDL_JoystickGetDeviceGUID (void);
void SDL_JoystickGetDeviceGUID (void) {}
void SDL_JoystickGetDeviceInstanceID (void);
void SDL_JoystickGetDeviceInstanceID (void) {}
void SDL_JoystickGetDevicePlayerIndex (void);
void SDL_JoystickGetDevicePlayerIndex (void) {}
void SDL_JoystickGetDeviceProduct (void);
void SDL_JoystickGetDeviceProduct (void) {}
void SDL_JoystickGetDeviceProductVersion (void);
void SDL_JoystickGetDeviceProductVersion (void) {}
void SDL_JoystickGetDeviceType (void);
void SDL_JoystickGetDeviceType (void) {}
void SDL_JoystickGetDeviceVendor (void);
void SDL_JoystickGetDeviceVendor (void) {}
void SDL_JoystickGetFirmwareVersion (void);
void SDL_JoystickGetFirmwareVersion (void) {}
void SDL_JoystickGetGUID (void);
void SDL_JoystickGetGUID (void) {}
void SDL_JoystickGetGUIDFromString (void);
void SDL_JoystickGetGUIDFromString (void) {}
void SDL_JoystickGetGUIDString (void);
void SDL_JoystickGetGUIDString (void) {}
void SDL_JoystickGetHat (void);
void SDL_JoystickGetHat (void) {}
void SDL_JoystickGetPlayerIndex (void);
void SDL_JoystickGetPlayerIndex (void) {}
void SDL_JoystickGetProduct (void);
void SDL_JoystickGetProduct (void) {}
void SDL_JoystickGetProductVersion (void);
void SDL_JoystickGetProductVersion (void) {}
void SDL_JoystickGetSerial (void);
void SDL_JoystickGetSerial (void) {}
void SDL_JoystickGetType (void);
void SDL_JoystickGetType (void) {}
void SDL_JoystickGetVendor (void);
void SDL_JoystickGetVendor (void) {}
void SDL_JoystickHasLED (void);
void SDL_JoystickHasLED (void) {}
void SDL_JoystickHasRumble (void);
void SDL_JoystickHasRumble (void) {}
void SDL_JoystickHasRumbleTriggers (void);
void SDL_JoystickHasRumbleTriggers (void) {}
void SDL_JoystickInstanceID (void);
void SDL_JoystickInstanceID (void) {}
void SDL_JoystickIsHaptic (void);
void SDL_JoystickIsHaptic (void) {}
void SDL_JoystickIsVirtual (void);
void SDL_JoystickIsVirtual (void) {}
void SDL_JoystickName (void);
void SDL_JoystickName (void) {}
void SDL_JoystickNameForIndex (void);
void SDL_JoystickNameForIndex (void) {}
void SDL_JoystickNumAxes (void);
void SDL_JoystickNumAxes (void) {}
void SDL_JoystickNumBalls (void);
void SDL_JoystickNumBalls (void) {}
void SDL_JoystickNumButtons (void);
void SDL_JoystickNumButtons (void) {}
void SDL_JoystickNumHats (void);
void SDL_JoystickNumHats (void) {}
void SDL_JoystickOpen (void);
void SDL_JoystickOpen (void) {}
void SDL_JoystickPath (void);
void SDL_JoystickPath (void) {}
void SDL_JoystickPathForIndex (void);
void SDL_JoystickPathForIndex (void) {}
void SDL_JoystickRumble (void);
void SDL_JoystickRumble (void) {}
void SDL_JoystickRumbleTriggers (void);
void SDL_JoystickRumbleTriggers (void) {}
void SDL_JoystickSendEffect (void);
void SDL_JoystickSendEffect (void) {}
void SDL_JoystickSetLED (void);
void SDL_JoystickSetLED (void) {}
void SDL_JoystickSetPlayerIndex (void);
void SDL_JoystickSetPlayerIndex (void) {}
void SDL_JoystickSetVirtualAxis (void);
void SDL_JoystickSetVirtualAxis (void) {}
void SDL_JoystickSetVirtualButton (void);
void SDL_JoystickSetVirtualButton (void) {}
void SDL_JoystickSetVirtualHat (void);
void SDL_JoystickSetVirtualHat (void) {}
void SDL_JoystickUpdate (void);
void SDL_JoystickUpdate (void) {}
void SDL_LinuxSetThreadPriority (void);
void SDL_LinuxSetThreadPriority (void) {}
void SDL_LinuxSetThreadPriorityAndPolicy (void);
void SDL_LinuxSetThreadPriorityAndPolicy (void) {}
void SDL_LoadBMP_RW (void);
void SDL_LoadBMP_RW (void) {}
void SDL_LoadDollarTemplates (void);
void SDL_LoadDollarTemplates (void) {}
void SDL_LoadFile (void);
void SDL_LoadFile (void) {}
void SDL_LoadFile_RW (void);
void SDL_LoadFile_RW (void) {}
void SDL_LoadFunction (void);
void SDL_LoadFunction (void) {}
void SDL_LoadObject (void);
void SDL_LoadObject (void) {}
void SDL_LoadWAV_RW (void);
void SDL_LoadWAV_RW (void) {}
void SDL_LockAudio (void);
void SDL_LockAudio (void) {}
void SDL_LockAudioDevice (void);
void SDL_LockAudioDevice (void) {}
void SDL_LockJoysticks (void);
void SDL_LockJoysticks (void) {}
void SDL_LockMutex (void);
void SDL_LockMutex (void) {}
void SDL_LockSensors (void);
void SDL_LockSensors (void) {}
void SDL_LockSurface (void);
void SDL_LockSurface (void) {}
void SDL_LockTexture (void);
void SDL_LockTexture (void) {}
void SDL_LockTextureToSurface (void);
void SDL_LockTextureToSurface (void) {}
void SDL_Log (void);
void SDL_Log (void) {}
void SDL_LogCritical (void);
void SDL_LogCritical (void) {}
void SDL_LogDebug (void);
void SDL_LogDebug (void) {}
void SDL_LogError (void);
void SDL_LogError (void) {}
void SDL_LogGetOutputFunction (void);
void SDL_LogGetOutputFunction (void) {}
void SDL_LogGetPriority (void);
void SDL_LogGetPriority (void) {}
void SDL_LogInfo (void);
void SDL_LogInfo (void) {}
void SDL_LogMessage (void);
void SDL_LogMessage (void) {}
void SDL_LogMessageV (void);
void SDL_LogMessageV (void) {}
void SDL_LogResetPriorities (void);
void SDL_LogResetPriorities (void) {}
void SDL_LogSetAllPriority (void);
void SDL_LogSetAllPriority (void) {}
void SDL_LogSetOutputFunction (void);
void SDL_LogSetOutputFunction (void) {}
void SDL_LogSetPriority (void);
void SDL_LogSetPriority (void) {}
void SDL_LogVerbose (void);
void SDL_LogVerbose (void) {}
void SDL_LogWarn (void);
void SDL_LogWarn (void) {}
void SDL_LowerBlit (void);
void SDL_LowerBlit (void) {}
void SDL_LowerBlitScaled (void);
void SDL_LowerBlitScaled (void) {}
void SDL_MapRGB (void);
void SDL_MapRGB (void) {}
void SDL_MapRGBA (void);
void SDL_MapRGBA (void) {}
void SDL_MasksToPixelFormatEnum (void);
void SDL_MasksToPixelFormatEnum (void) {}
void SDL_MaximizeWindow (void);
void SDL_MaximizeWindow (void) {}
void SDL_MemoryBarrierAcquireFunction (void);
void SDL_MemoryBarrierAcquireFunction (void) {}
void SDL_MemoryBarrierReleaseFunction (void);
void SDL_MemoryBarrierReleaseFunction (void) {}
void SDL_Metal_CreateView (void);
void SDL_Metal_CreateView (void) {}
void SDL_Metal_DestroyView (void);
void SDL_Metal_DestroyView (void) {}
void SDL_Metal_GetDrawableSize (void);
void SDL_Metal_GetDrawableSize (void) {}
void SDL_Metal_GetLayer (void);
void SDL_Metal_GetLayer (void) {}
void SDL_MinimizeWindow (void);
void SDL_MinimizeWindow (void) {}
void SDL_MixAudio (void);
void SDL_MixAudio (void) {}
void SDL_MixAudioFormat (void);
void SDL_MixAudioFormat (void) {}
void SDL_MouseIsHaptic (void);
void SDL_MouseIsHaptic (void) {}
void SDL_NewAudioStream (void);
void SDL_NewAudioStream (void) {}
void SDL_NumHaptics (void);
void SDL_NumHaptics (void) {}
void SDL_NumJoysticks (void);
void SDL_NumJoysticks (void) {}
void SDL_NumSensors (void);
void SDL_NumSensors (void) {}
void SDL_OnApplicationDidBecomeActive (void);
void SDL_OnApplicationDidBecomeActive (void) {}
void SDL_OnApplicationDidChangeStatusBarOrientation (void);
void SDL_OnApplicationDidChangeStatusBarOrientation (void) {}
void SDL_OnApplicationDidEnterBackground (void);
void SDL_OnApplicationDidEnterBackground (void) {}
void SDL_OnApplicationDidReceiveMemoryWarning (void);
void SDL_OnApplicationDidReceiveMemoryWarning (void) {}
void SDL_OnApplicationWillEnterForeground (void);
void SDL_OnApplicationWillEnterForeground (void) {}
void SDL_OnApplicationWillResignActive (void);
void SDL_OnApplicationWillResignActive (void) {}
void SDL_OnApplicationWillTerminate (void);
void SDL_OnApplicationWillTerminate (void) {}
void SDL_OpenAudio (void);
void SDL_OpenAudio (void) {}
void SDL_OpenAudioDevice (void);
void SDL_OpenAudioDevice (void) {}
void SDL_OpenURL (void);
void SDL_OpenURL (void) {}
void SDL_PauseAudio (void);
void SDL_PauseAudio (void) {}
void SDL_PauseAudioDevice (void);
void SDL_PauseAudioDevice (void) {}
void SDL_PeepEvents (void);
void SDL_PeepEvents (void) {}
void SDL_PixelFormatEnumToMasks (void);
void SDL_PixelFormatEnumToMasks (void) {}
void SDL_PollEvent (void);
void SDL_PollEvent (void) {}
void SDL_PremultiplyAlpha (void);
void SDL_PremultiplyAlpha (void) {}
void SDL_PumpEvents (void);
void SDL_PumpEvents (void) {}
void SDL_PushEvent (void);
void SDL_PushEvent (void) {}
void SDL_QueryTexture (void);
void SDL_QueryTexture (void) {}
void SDL_QueueAudio (void);
void SDL_QueueAudio (void) {}
void SDL_Quit (void);
void SDL_Quit (void) {}
void SDL_QuitSubSystem (void);
void SDL_QuitSubSystem (void) {}
void SDL_RWFromConstMem (void);
void SDL_RWFromConstMem (void) {}
void SDL_RWFromFP (void);
void SDL_RWFromFP (void) {}
void SDL_RWFromFile (void);
void SDL_RWFromFile (void) {}
void SDL_RWFromMem (void);
void SDL_RWFromMem (void) {}
void SDL_RWclose (void);
void SDL_RWclose (void) {}
void SDL_RWread (void);
void SDL_RWread (void) {}
void SDL_RWseek (void);
void SDL_RWseek (void) {}
void SDL_RWsize (void);
void SDL_RWsize (void) {}
void SDL_RWtell (void);
void SDL_RWtell (void) {}
void SDL_RWwrite (void);
void SDL_RWwrite (void) {}
void SDL_RaiseWindow (void);
void SDL_RaiseWindow (void) {}
void SDL_ReadBE16 (void);
void SDL_ReadBE16 (void) {}
void SDL_ReadBE32 (void);
void SDL_ReadBE32 (void) {}
void SDL_ReadBE64 (void);
void SDL_ReadBE64 (void) {}
void SDL_ReadLE16 (void);
void SDL_ReadLE16 (void) {}
void SDL_ReadLE32 (void);
void SDL_ReadLE32 (void) {}
void SDL_ReadLE64 (void);
void SDL_ReadLE64 (void) {}
void SDL_ReadU8 (void);
void SDL_ReadU8 (void) {}
void SDL_RecordGesture (void);
void SDL_RecordGesture (void) {}
void SDL_RegisterApp (void);
void SDL_RegisterApp (void) {}
void SDL_RegisterEvents (void);
void SDL_RegisterEvents (void) {}
void SDL_RemoveTimer (void);
void SDL_RemoveTimer (void) {}
void SDL_RenderClear (void);
void SDL_RenderClear (void) {}
void SDL_RenderCopy (void);
void SDL_RenderCopy (void) {}
void SDL_RenderCopyEx (void);
void SDL_RenderCopyEx (void) {}
void SDL_RenderCopyExF (void);
void SDL_RenderCopyExF (void) {}
void SDL_RenderCopyF (void);
void SDL_RenderCopyF (void) {}
void SDL_RenderDrawLine (void);
void SDL_RenderDrawLine (void) {}
void SDL_RenderDrawLineF (void);
void SDL_RenderDrawLineF (void) {}
void SDL_RenderDrawLines (void);
void SDL_RenderDrawLines (void) {}
void SDL_RenderDrawLinesF (void);
void SDL_RenderDrawLinesF (void) {}
void SDL_RenderDrawPoint (void);
void SDL_RenderDrawPoint (void) {}
void SDL_RenderDrawPointF (void);
void SDL_RenderDrawPointF (void) {}
void SDL_RenderDrawPoints (void);
void SDL_RenderDrawPoints (void) {}
void SDL_RenderDrawPointsF (void);
void SDL_RenderDrawPointsF (void) {}
void SDL_RenderDrawRect (void);
void SDL_RenderDrawRect (void) {}
void SDL_RenderDrawRectF (void);
void SDL_RenderDrawRectF (void) {}
void SDL_RenderDrawRects (void);
void SDL_RenderDrawRects (void) {}
void SDL_RenderDrawRectsF (void);
void SDL_RenderDrawRectsF (void) {}
void SDL_RenderFillRect (void);
void SDL_RenderFillRect (void) {}
void SDL_RenderFillRectF (void);
void SDL_RenderFillRectF (void) {}
void SDL_RenderFillRects (void);
void SDL_RenderFillRects (void) {}
void SDL_RenderFillRectsF (void);
void SDL_RenderFillRectsF (void) {}
void SDL_RenderFlush (void);
void SDL_RenderFlush (void) {}
void SDL_RenderGeometry (void);
void SDL_RenderGeometry (void) {}
void SDL_RenderGeometryRaw (void);
void SDL_RenderGeometryRaw (void) {}
void SDL_RenderGetClipRect (void);
void SDL_RenderGetClipRect (void) {}
void SDL_RenderGetD3D11Device (void);
void SDL_RenderGetD3D11Device (void) {}
void SDL_RenderGetD3D12Device (void);
void SDL_RenderGetD3D12Device (void) {}
void SDL_RenderGetD3D9Device (void);
void SDL_RenderGetD3D9Device (void) {}
void SDL_RenderGetIntegerScale (void);
void SDL_RenderGetIntegerScale (void) {}
void SDL_RenderGetLogicalSize (void);
void SDL_RenderGetLogicalSize (void) {}
void SDL_RenderGetMetalCommandEncoder (void);
void SDL_RenderGetMetalCommandEncoder (void) {}
void SDL_RenderGetMetalLayer (void);
void SDL_RenderGetMetalLayer (void) {}
void SDL_RenderGetScale (void);
void SDL_RenderGetScale (void) {}
void SDL_RenderGetViewport (void);
void SDL_RenderGetViewport (void) {}
void SDL_RenderGetWindow (void);
void SDL_RenderGetWindow (void) {}
void SDL_RenderIsClipEnabled (void);
void SDL_RenderIsClipEnabled (void) {}
void SDL_RenderLogicalToWindow (void);
void SDL_RenderLogicalToWindow (void) {}
void SDL_RenderPresent (void);
void SDL_RenderPresent (void) {}
void SDL_RenderReadPixels (void);
void SDL_RenderReadPixels (void) {}
void SDL_RenderSetClipRect (void);
void SDL_RenderSetClipRect (void) {}
void SDL_RenderSetIntegerScale (void);
void SDL_RenderSetIntegerScale (void) {}
void SDL_RenderSetLogicalSize (void);
void SDL_RenderSetLogicalSize (void) {}
void SDL_RenderSetScale (void);
void SDL_RenderSetScale (void) {}
void SDL_RenderSetVSync (void);
void SDL_RenderSetVSync (void) {}
void SDL_RenderSetViewport (void);
void SDL_RenderSetViewport (void) {}
void SDL_RenderTargetSupported (void);
void SDL_RenderTargetSupported (void) {}
void SDL_RenderWindowToLogical (void);
void SDL_RenderWindowToLogical (void) {}
void SDL_ReportAssertion (void);
void SDL_ReportAssertion (void) {}
void SDL_ResetAssertionReport (void);
void SDL_ResetAssertionReport (void) {}
void SDL_ResetHint (void);
void SDL_ResetHint (void) {}
void SDL_ResetHints (void);
void SDL_ResetHints (void) {}
void SDL_ResetKeyboard (void);
void SDL_ResetKeyboard (void) {}
void SDL_RestoreWindow (void);
void SDL_RestoreWindow (void) {}
void SDL_SIMDAlloc (void);
void SDL_SIMDAlloc (void) {}
void SDL_SIMDFree (void);
void SDL_SIMDFree (void) {}
void SDL_SIMDGetAlignment (void);
void SDL_SIMDGetAlignment (void) {}
void SDL_SIMDRealloc (void);
void SDL_SIMDRealloc (void) {}
void SDL_SaveAllDollarTemplates (void);
void SDL_SaveAllDollarTemplates (void) {}
void SDL_SaveBMP_RW (void);
void SDL_SaveBMP_RW (void) {}
void SDL_SaveDollarTemplate (void);
void SDL_SaveDollarTemplate (void) {}
void SDL_SemPost (void);
void SDL_SemPost (void) {}
void SDL_SemTryWait (void);
void SDL_SemTryWait (void) {}
void SDL_SemValue (void);
void SDL_SemValue (void) {}
void SDL_SemWait (void);
void SDL_SemWait (void) {}
void SDL_SemWaitTimeout (void);
void SDL_SemWaitTimeout (void) {}
void SDL_SensorClose (void);
void SDL_SensorClose (void) {}
void SDL_SensorFromInstanceID (void);
void SDL_SensorFromInstanceID (void) {}
void SDL_SensorGetData (void);
void SDL_SensorGetData (void) {}
void SDL_SensorGetDataWithTimestamp (void);
void SDL_SensorGetDataWithTimestamp (void) {}
void SDL_SensorGetDeviceInstanceID (void);
void SDL_SensorGetDeviceInstanceID (void) {}
void SDL_SensorGetDeviceName (void);
void SDL_SensorGetDeviceName (void) {}
void SDL_SensorGetDeviceNonPortableType (void);
void SDL_SensorGetDeviceNonPortableType (void) {}
void SDL_SensorGetDeviceType (void);
void SDL_SensorGetDeviceType (void) {}
void SDL_SensorGetInstanceID (void);
void SDL_SensorGetInstanceID (void) {}
void SDL_SensorGetName (void);
void SDL_SensorGetName (void) {}
void SDL_SensorGetNonPortableType (void);
void SDL_SensorGetNonPortableType (void) {}
void SDL_SensorGetType (void);
void SDL_SensorGetType (void) {}
void SDL_SensorOpen (void);
void SDL_SensorOpen (void) {}
void SDL_SensorUpdate (void);
void SDL_SensorUpdate (void) {}
void SDL_SetAssertionHandler (void);
void SDL_SetAssertionHandler (void) {}
void SDL_SetClipRect (void);
void SDL_SetClipRect (void) {}
void SDL_SetClipboardText (void);
void SDL_SetClipboardText (void) {}
void SDL_SetColorKey (void);
void SDL_SetColorKey (void) {}
void SDL_SetCursor (void);
void SDL_SetCursor (void) {}
void SDL_SetError (void);
void SDL_SetError (void) {}
void SDL_SetEventFilter (void);
void SDL_SetEventFilter (void) {}
void SDL_SetHint (void);
void SDL_SetHint (void) {}
void SDL_SetHintWithPriority (void);
void SDL_SetHintWithPriority (void) {}
void SDL_SetMainReady (void);
void SDL_SetMainReady (void) {}
void SDL_SetMemoryFunctions (void);
void SDL_SetMemoryFunctions (void) {}
void SDL_SetModState (void);
void SDL_SetModState (void) {}
void SDL_SetPaletteColors (void);
void SDL_SetPaletteColors (void) {}
void SDL_SetPixelFormatPalette (void);
void SDL_SetPixelFormatPalette (void) {}
void SDL_SetPrimarySelectionText (void);
void SDL_SetPrimarySelectionText (void) {}
void SDL_SetRelativeMouseMode (void);
void SDL_SetRelativeMouseMode (void) {}
void SDL_SetRenderDrawBlendMode (void);
void SDL_SetRenderDrawBlendMode (void) {}
void SDL_SetRenderDrawColor (void);
void SDL_SetRenderDrawColor (void) {}
void SDL_SetRenderTarget (void);
void SDL_SetRenderTarget (void) {}
void SDL_SetSurfaceAlphaMod (void);
void SDL_SetSurfaceAlphaMod (void) {}
void SDL_SetSurfaceBlendMode (void);
void SDL_SetSurfaceBlendMode (void) {}
void SDL_SetSurfaceColorMod (void);
void SDL_SetSurfaceColorMod (void) {}
void SDL_SetSurfacePalette (void);
void SDL_SetSurfacePalette (void) {}
void SDL_SetSurfaceRLE (void);
void SDL_SetSurfaceRLE (void) {}
void SDL_SetTextInputRect (void);
void SDL_SetTextInputRect (void) {}
void SDL_SetTextureAlphaMod (void);
void SDL_SetTextureAlphaMod (void) {}
void SDL_SetTextureBlendMode (void);
void SDL_SetTextureBlendMode (void) {}
void SDL_SetTextureColorMod (void);
void SDL_SetTextureColorMod (void) {}
void SDL_SetTextureScaleMode (void);
void SDL_SetTextureScaleMode (void) {}
void SDL_SetTextureUserData (void);
void SDL_SetTextureUserData (void) {}
void SDL_SetThreadPriority (void);
void SDL_SetThreadPriority (void) {}
void SDL_SetWindowAlwaysOnTop (void);
void SDL_SetWindowAlwaysOnTop (void) {}
void SDL_SetWindowBordered (void);
void SDL_SetWindowBordered (void) {}
void SDL_SetWindowBrightness (void);
void SDL_SetWindowBrightness (void) {}
void SDL_SetWindowData (void);
void SDL_SetWindowData (void) {}
void SDL_SetWindowDisplayMode (void);
void SDL_SetWindowDisplayMode (void) {}
void SDL_SetWindowFullscreen (void);
void SDL_SetWindowFullscreen (void) {}
void SDL_SetWindowGammaRamp (void);
void SDL_SetWindowGammaRamp (void) {}
void SDL_SetWindowGrab (void);
void SDL_SetWindowGrab (void) {}
void SDL_SetWindowHitTest (void);
void SDL_SetWindowHitTest (void) {}
void SDL_SetWindowIcon (void);
void SDL_SetWindowIcon (void) {}
void SDL_SetWindowInputFocus (void);
void SDL_SetWindowInputFocus (void) {}
void SDL_SetWindowKeyboardGrab (void);
void SDL_SetWindowKeyboardGrab (void) {}
void SDL_SetWindowMaximumSize (void);
void SDL_SetWindowMaximumSize (void) {}
void SDL_SetWindowMinimumSize (void);
void SDL_SetWindowMinimumSize (void) {}
void SDL_SetWindowModalFor (void);
void SDL_SetWindowModalFor (void) {}
void SDL_SetWindowMouseGrab (void);
void SDL_SetWindowMouseGrab (void) {}
void SDL_SetWindowMouseRect (void);
void SDL_SetWindowMouseRect (void) {}
void SDL_SetWindowOpacity (void);
void SDL_SetWindowOpacity (void) {}
void SDL_SetWindowPosition (void);
void SDL_SetWindowPosition (void) {}
void SDL_SetWindowResizable (void);
void SDL_SetWindowResizable (void) {}
void SDL_SetWindowShape (void);
void SDL_SetWindowShape (void) {}
void SDL_SetWindowSize (void);
void SDL_SetWindowSize (void) {}
void SDL_SetWindowTitle (void);
void SDL_SetWindowTitle (void) {}
void SDL_SetWindowsMessageHook (void);
void SDL_SetWindowsMessageHook (void) {}
void SDL_SetYUVConversionMode (void);
void SDL_SetYUVConversionMode (void) {}
void SDL_ShowCursor (void);
void SDL_ShowCursor (void) {}
void SDL_ShowMessageBox (void);
void SDL_ShowMessageBox (void) {}
void SDL_ShowSimpleMessageBox (void);
void SDL_ShowSimpleMessageBox (void) {}
void SDL_ShowWindow (void);
void SDL_ShowWindow (void) {}
void SDL_SoftStretch (void);
void SDL_SoftStretch (void) {}
void SDL_SoftStretchLinear (void);
void SDL_SoftStretchLinear (void) {}
void SDL_StartTextInput (void);
void SDL_StartTextInput (void) {}
void SDL_StopTextInput (void);
void SDL_StopTextInput (void) {}
void SDL_TLSCleanup (void);
void SDL_TLSCleanup (void) {}
void SDL_TLSCreate (void);
void SDL_TLSCreate (void) {}
void SDL_TLSGet (void);
void SDL_TLSGet (void) {}
void SDL_TLSSet (void);
void SDL_TLSSet (void) {}
void SDL_ThreadID (void);
void SDL_ThreadID (void) {}
void SDL_TryLockMutex (void);
void SDL_TryLockMutex (void) {}
void SDL_UIKitRunApp (void);
void SDL_UIKitRunApp (void) {}
void SDL_UnionFRect (void);
void SDL_UnionFRect (void) {}
void SDL_UnionRect (void);
void SDL_UnionRect (void) {}
void SDL_UnloadObject (void);
void SDL_UnloadObject (void) {}
void SDL_UnlockAudio (void);
void SDL_UnlockAudio (void) {}
void SDL_UnlockAudioDevice (void);
void SDL_UnlockAudioDevice (void) {}
void SDL_UnlockJoysticks (void);
void SDL_UnlockJoysticks (void) {}
void SDL_UnlockMutex (void);
void SDL_UnlockMutex (void) {}
void SDL_UnlockSensors (void);
void SDL_UnlockSensors (void) {}
void SDL_UnlockSurface (void);
void SDL_UnlockSurface (void) {}
void SDL_UnlockTexture (void);
void SDL_UnlockTexture (void) {}
void SDL_UnregisterApp (void);
void SDL_UnregisterApp (void) {}
void SDL_UpdateNVTexture (void);
void SDL_UpdateNVTexture (void) {}
void SDL_UpdateTexture (void);
void SDL_UpdateTexture (void) {}
void SDL_UpdateWindowSurface (void);
void SDL_UpdateWindowSurface (void) {}
void SDL_UpdateWindowSurfaceRects (void);
void SDL_UpdateWindowSurfaceRects (void) {}
void SDL_UpdateYUVTexture (void);
void SDL_UpdateYUVTexture (void) {}
void SDL_UpperBlit (void);
void SDL_UpperBlit (void) {}
void SDL_UpperBlitScaled (void);
void SDL_UpperBlitScaled (void) {}
void SDL_VideoInit (void);
void SDL_VideoInit (void) {}
void SDL_VideoQuit (void);
void SDL_VideoQuit (void) {}
void SDL_Vulkan_CreateSurface (void);
void SDL_Vulkan_CreateSurface (void) {}
void SDL_Vulkan_GetDrawableSize (void);
void SDL_Vulkan_GetDrawableSize (void) {}
void SDL_Vulkan_GetInstanceExtensions (void);
void SDL_Vulkan_GetInstanceExtensions (void) {}
void SDL_Vulkan_GetVkGetInstanceProcAddr (void);
void SDL_Vulkan_GetVkGetInstanceProcAddr (void) {}
void SDL_Vulkan_LoadLibrary (void);
void SDL_Vulkan_LoadLibrary (void) {}
void SDL_Vulkan_UnloadLibrary (void);
void SDL_Vulkan_UnloadLibrary (void) {}
void SDL_WaitEvent (void);
void SDL_WaitEvent (void) {}
void SDL_WaitEventTimeout (void);
void SDL_WaitEventTimeout (void) {}
void SDL_WaitThread (void);
void SDL_WaitThread (void) {}
void SDL_WarpMouseGlobal (void);
void SDL_WarpMouseGlobal (void) {}
void SDL_WarpMouseInWindow (void);
void SDL_WarpMouseInWindow (void) {}
void SDL_WasInit (void);
void SDL_WasInit (void) {}
void SDL_WinRTGetDeviceFamily (void);
void SDL_WinRTGetDeviceFamily (void) {}
void SDL_WinRTGetFSPathUNICODE (void);
void SDL_WinRTGetFSPathUNICODE (void) {}
void SDL_WinRTGetFSPathUTF8 (void);
void SDL_WinRTGetFSPathUTF8 (void) {}
void SDL_WinRTRunApp (void);
void SDL_WinRTRunApp (void) {}
void SDL_WriteBE16 (void);
void SDL_WriteBE16 (void) {}
void SDL_WriteBE32 (void);
void SDL_WriteBE32 (void) {}
void SDL_WriteBE64 (void);
void SDL_WriteBE64 (void) {}
void SDL_WriteLE16 (void);
void SDL_WriteLE16 (void) {}
void SDL_WriteLE32 (void);
void SDL_WriteLE32 (void) {}
void SDL_WriteLE64 (void);
void SDL_WriteLE64 (void) {}
void SDL_WriteU8 (void);
void SDL_WriteU8 (void) {}
void SDL_abs (void);
void SDL_abs (void) {}
void SDL_acos (void);
void SDL_acos (void) {}
void SDL_acosf (void);
void SDL_acosf (void) {}
void SDL_asin (void);
void SDL_asin (void) {}
void SDL_asinf (void);
void SDL_asinf (void) {}
void SDL_asprintf (void);
void SDL_asprintf (void) {}
void SDL_atan (void);
void SDL_atan (void) {}
void SDL_atan2 (void);
void SDL_atan2 (void) {}
void SDL_atan2f (void);
void SDL_atan2f (void) {}
void SDL_atanf (void);
void SDL_atanf (void) {}
void SDL_atof (void);
void SDL_atof (void) {}
void SDL_atoi (void);
void SDL_atoi (void) {}
void SDL_bsearch (void);
void SDL_bsearch (void) {}
void SDL_calloc (void);
void SDL_calloc (void) {}
void SDL_ceil (void);
void SDL_ceil (void) {}
void SDL_ceilf (void);
void SDL_ceilf (void) {}
void SDL_copysign (void);
void SDL_copysign (void) {}
void SDL_copysignf (void);
void SDL_copysignf (void) {}
void SDL_cos (void);
void SDL_cos (void) {}
void SDL_cosf (void);
void SDL_cosf (void) {}
void SDL_crc16 (void);
void SDL_crc16 (void) {}
void SDL_crc32 (void);
void SDL_crc32 (void) {}
void SDL_exp (void);
void SDL_exp (void) {}
void SDL_expf (void);
void SDL_expf (void) {}
void SDL_fabs (void);
void SDL_fabs (void) {}
void SDL_fabsf (void);
void SDL_fabsf (void) {}
void SDL_floor (void);
void SDL_floor (void) {}
void SDL_floorf (void);
void SDL_floorf (void) {}
void SDL_fmod (void);
void SDL_fmod (void) {}
void SDL_fmodf (void);
void SDL_fmodf (void) {}
void SDL_free (void);
void SDL_free (void) {}
void SDL_getenv (void);
void SDL_getenv (void) {}
void SDL_hid_ble_scan (void);
void SDL_hid_ble_scan (void) {}
void SDL_hid_close (void);
void SDL_hid_close (void) {}
void SDL_hid_device_change_count (void);
void SDL_hid_device_change_count (void) {}
void SDL_hid_enumerate (void);
void SDL_hid_enumerate (void) {}
void SDL_hid_exit (void);
void SDL_hid_exit (void) {}
void SDL_hid_free_enumeration (void);
void SDL_hid_free_enumeration (void) {}
void SDL_hid_get_feature_report (void);
void SDL_hid_get_feature_report (void) {}
void SDL_hid_get_indexed_string (void);
void SDL_hid_get_indexed_string (void) {}
void SDL_hid_get_manufacturer_string (void);
void SDL_hid_get_manufacturer_string (void) {}
void SDL_hid_get_product_string (void);
void SDL_hid_get_product_string (void) {}
void SDL_hid_get_serial_number_string (void);
void SDL_hid_get_serial_number_string (void) {}
void SDL_hid_init (void);
void SDL_hid_init (void) {}
void SDL_hid_open (void);
void SDL_hid_open (void) {}
void SDL_hid_open_path (void);
void SDL_hid_open_path (void) {}
void SDL_hid_read (void);
void SDL_hid_read (void) {}
void SDL_hid_read_timeout (void);
void SDL_hid_read_timeout (void) {}
void SDL_hid_send_feature_report (void);
void SDL_hid_send_feature_report (void) {}
void SDL_hid_set_nonblocking (void);
void SDL_hid_set_nonblocking (void) {}
void SDL_hid_write (void);
void SDL_hid_write (void) {}
void SDL_iPhoneSetAnimationCallback (void);
void SDL_iPhoneSetAnimationCallback (void) {}
void SDL_iPhoneSetEventPump (void);
void SDL_iPhoneSetEventPump (void) {}
void SDL_iconv (void);
void SDL_iconv (void) {}
void SDL_iconv_close (void);
void SDL_iconv_close (void) {}
void SDL_iconv_open (void);
void SDL_iconv_open (void) {}
void SDL_iconv_string (void);
void SDL_iconv_string (void) {}
void SDL_isalnum (void);
void SDL_isalnum (void) {}
void SDL_isalpha (void);
void SDL_isalpha (void) {}
void SDL_isblank (void);
void SDL_isblank (void) {}
void SDL_iscntrl (void);
void SDL_iscntrl (void) {}
void SDL_isdigit (void);
void SDL_isdigit (void) {}
void SDL_isgraph (void);
void SDL_isgraph (void) {}
void SDL_islower (void);
void SDL_islower (void) {}
void SDL_isprint (void);
void SDL_isprint (void) {}
void SDL_ispunct (void);
void SDL_ispunct (void) {}
void SDL_isspace (void);
void SDL_isspace (void) {}
void SDL_isupper (void);
void SDL_isupper (void) {}
void SDL_isxdigit (void);
void SDL_isxdigit (void) {}
void SDL_itoa (void);
void SDL_itoa (void) {}
void SDL_lltoa (void);
void SDL_lltoa (void) {}
void SDL_log (void);
void SDL_log (void) {}
void SDL_log10 (void);
void SDL_log10 (void) {}
void SDL_log10f (void);
void SDL_log10f (void) {}
void SDL_logf (void);
void SDL_logf (void) {}
void SDL_lround (void);
void SDL_lround (void) {}
void SDL_lroundf (void);
void SDL_lroundf (void) {}
void SDL_ltoa (void);
void SDL_ltoa (void) {}
void SDL_malloc (void);
void SDL_malloc (void) {}
void SDL_memcmp (void);
void SDL_memcmp (void) {}
void SDL_memcpy (void);
void SDL_memcpy (void) {}
void SDL_memmove (void);
void SDL_memmove (void) {}
void SDL_memset (void);
void SDL_memset (void) {}
void SDL_pow (void);
void SDL_pow (void) {}
void SDL_powf (void);
void SDL_powf (void) {}
void SDL_qsort (void);
void SDL_qsort (void) {}
void SDL_realloc (void);
void SDL_realloc (void) {}
void SDL_round (void);
void SDL_round (void) {}
void SDL_roundf (void);
void SDL_roundf (void) {}
void SDL_scalbn (void);
void SDL_scalbn (void) {}
void SDL_scalbnf (void);
void SDL_scalbnf (void) {}
void SDL_setenv (void);
void SDL_setenv (void) {}
void SDL_sin (void);
void SDL_sin (void) {}
void SDL_sinf (void);
void SDL_sinf (void) {}
void SDL_snprintf (void);
void SDL_snprintf (void) {}
void SDL_sqrt (void);
void SDL_sqrt (void) {}
void SDL_sqrtf (void);
void SDL_sqrtf (void) {}
void SDL_sscanf (void);
void SDL_sscanf (void) {}
void SDL_strcasecmp (void);
void SDL_strcasecmp (void) {}
void SDL_strcasestr (void);
void SDL_strcasestr (void) {}
void SDL_strchr (void);
void SDL_strchr (void) {}
void SDL_strcmp (void);
void SDL_strcmp (void) {}
void SDL_strdup (void);
void SDL_strdup (void) {}
void SDL_strlcat (void);
void SDL_strlcat (void) {}
void SDL_strlcpy (void);
void SDL_strlcpy (void) {}
void SDL_strlen (void);
void SDL_strlen (void) {}
void SDL_strlwr (void);
void SDL_strlwr (void) {}
void SDL_strncasecmp (void);
void SDL_strncasecmp (void) {}
void SDL_strncmp (void);
void SDL_strncmp (void) {}
void SDL_strrchr (void);
void SDL_strrchr (void) {}
void SDL_strrev (void);
void SDL_strrev (void) {}
void SDL_strstr (void);
void SDL_strstr (void) {}
void SDL_strtod (void);
void SDL_strtod (void) {}
void SDL_strtokr (void);
void SDL_strtokr (void) {}
void SDL_strtol (void);
void SDL_strtol (void) {}
void SDL_strtoll (void);
void SDL_strtoll (void) {}
void SDL_strtoul (void);
void SDL_strtoul (void) {}
void SDL_strtoull (void);
void SDL_strtoull (void) {}
void SDL_strupr (void);
void SDL_strupr (void) {}
void SDL_tan (void);
void SDL_tan (void) {}
void SDL_tanf (void);
void SDL_tanf (void) {}
void SDL_tolower (void);
void SDL_tolower (void) {}
void SDL_toupper (void);
void SDL_toupper (void) {}
void SDL_trunc (void);
void SDL_trunc (void) {}
void SDL_truncf (void);
void SDL_truncf (void) {}
void SDL_uitoa (void);
void SDL_uitoa (void) {}
void SDL_ulltoa (void);
void SDL_ulltoa (void) {}
void SDL_ultoa (void);
void SDL_ultoa (void) {}
void SDL_utf8strlcpy (void);
void SDL_utf8strlcpy (void) {}
void SDL_utf8strlen (void);
void SDL_utf8strlen (void) {}
void SDL_utf8strnlen (void);
void SDL_utf8strnlen (void) {}
void SDL_vasprintf (void);
void SDL_vasprintf (void) {}
void SDL_vsnprintf (void);
void SDL_vsnprintf (void) {}
void SDL_vsscanf (void);
void SDL_vsscanf (void) {}
void SDL_wcscasecmp (void);
void SDL_wcscasecmp (void) {}
void SDL_wcscmp (void);
void SDL_wcscmp (void) {}
void SDL_wcsdup (void);
void SDL_wcsdup (void) {}
void SDL_wcslcat (void);
void SDL_wcslcat (void) {}
void SDL_wcslcpy (void);
void SDL_wcslcpy (void) {}
void SDL_wcslen (void);
void SDL_wcslen (void) {}
void SDL_wcsncasecmp (void);
void SDL_wcsncasecmp (void) {}
void SDL_wcsncmp (void);
void SDL_wcsncmp (void) {}
void SDL_wcsstr (void);
void SDL_wcsstr (void) {}
