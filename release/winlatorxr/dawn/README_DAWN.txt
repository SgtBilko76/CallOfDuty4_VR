CALL OF DUTY 4 VR (KisakCOD VR) - WinlatorXR DAWN PACKAGE - BETA 0.2
====================================================================

For WinlatorXR "Dawn" (tested on dawn-33, Meta Quest 3). If you use the older
cats-27 fork, take KisakCOD-VR-WinlatorXR-v*.zip instead.

You need your own copy of Call of Duty 4: Modern Warfare (2007, Windows).
This package contains no Call of Duty game data.


WHAT IS IN HERE
---------------
  KisakCOD-sp.exe       the VR game executable
  mss32.dll, miles\,    audio and video runtime; these replace the game's
  binkw32.dll,          own copies
  steam_api.dll
  Launch-KisakCOD-VR-WinlatorXR.bat
                        starts the game with the WinlatorXR backend and the
                        2388x1080 side-by-side mode
  VR-Settings.bat       VR settings the launcher loads first
  CallOfDuty4-VR.desktop, Install-CoD4-Shortcut.bat
                        a ready-made shortcut and its installer
  CallOfDuty4-VR - proton-9.0-x86_64 - Quest 3.wxrprofile.json
                        the container settings, ready to import into Dawn
  WINLATORXR.txt        the full manual (setup, controls, settings, limits)


INSTALL
-------
1. Copy your COD4 folder (the one with iw3sp.exe) to the headset as
     /sdcard/Download/CallOfDuty4
   which is D:\CallOfDuty4 inside the container.

2. Extract everything from this zip INTO that folder, overwriting
   binkw32.dll, mss32.dll and the miles folder, so that you get
     CallOfDuty4\KisakCOD-sp.exe
     CallOfDuty4\Launch-KisakCOD-VR-WinlatorXR.bat

3. In WinlatorXR Dawn, create a container (Proton 9.0, x86_64) and make sure
   drive D: points to /sdcard/Download.

4. Create the shortcut. Either:
     a) Edit container_id in CallOfDuty4-VR.desktop to match your container,
        copy it to /sdcard/Download/Winlator, and run
        D:\CallOfDuty4\Install-CoD4-Shortcut.bat once inside the container;
   or
     b) Create a shortcut by hand:
          Exec:      wine C:\windows\system32\cmd.exe
          Arguments: /c D:\CallOfDuty4\Launch-KisakCOD-VR-WinlatorXR.bat

5. Import the profile: copy the .wxrprofile.json to
     /sdcard/Download/Winlator/WxrProfiles/
   then open the shortcut's settings in Dawn and import it. It sets the
   screen size (2388x1080), DXVK 1.10.1, the Turnip/wrapper driver and Box64
   (PERFORMANCE).

6. Put the headset ON, then launch CallOfDuty4-VR. The first start compiles
   shaders and takes a while. If the headset reads as taken off, the Quest
   suspends the app and it looks frozen.


SCREEN SIZE
-----------
The game draws both eyes side by side, so the screen is twice as wide as one
eye: 2388x1080 on a Quest 3. The profile, the shortcut's screenSize and
WINLATORXR_MODE in the launcher must all match. The game writes the value
that fits your headset to main\console.log at startup.


CONTROLS
--------
  Right trigger = fire              Left grip = support grip, magazines,
  A = reload / eject magazine                   grenades
  B = crouch / stance               Right grip = grenade launcher
  X = use / pick up                 Left stick click = sprint
  Y = next weapon                   Menu button = pause
  Right stick click = free (WinlatorXR's own menu)

  Menus: left stick moves the selection, A confirms, B goes back.
  Reloads and grenades are physical - see WINLATORXR.txt.


TROUBLESHOOTING
---------------
  Doubled flat image (both eyes side by side)
      WinlatorXR has not found the sync pixel. The game pins its window
      borderless at the top left on its own; relaunch, and check that the
      screen size matches everywhere.
  Squeezed or stretched image
      The screen size does not match the launcher's WINLATORXR_MODE.
  Frozen on a static frame
      The headset reads as off-face - put it on.

Source: https://github.com/SgtBilko76/CallOfDuty4_VR
Based on KisakCOD VR (https://github.com/jplakon/CallOfDuty4_VR) and
KisakCOD (https://github.com/SwagSoftware/KisakCOD).
Call of Duty 4: Modern Warfare is (C) Infinity Ward / Activision.
