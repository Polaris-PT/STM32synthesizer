# play.ps1 - send MIDI to the synth from the command line (WinMM, no extra install)
#
#   .\tools\play.ps1 -List                          # list MIDI out devices (the synth is usually index 1)
#   .\tools\play.ps1 -Patch 20                      # channel 0 -> patch 20 (Piano, from the QSPI library)
#   .\tools\play.ps1 -Patch 23 -Notes 36            # channel 0 -> Perc, play kick
#   .\tools\play.ps1 -Notes 60,64,67 -Duration 800  # play a chord for 0.8 s
#   .\tools\play.ps1 -Patch 12 -Channel 1 -Notes 64 # channel 1 -> FM E.Piano
#   .\tools\play.ps1 -AllNotesOff                   # panic: all notes off on every channel
#
# Only ONE sample instrument (PC 20..23) can be live at a time: the last Program Change wins.
# The 15 built-in patches (PC 0..14) are per-channel and can coexist.

param(
    [int]$Device = 1,
    [int]$Channel = 0,
    [int]$Patch = -1,
    [string]$Notes = "",
    [int]$Velocity = 100,
    [int]$Duration = 700,
    [switch]$List,
    [switch]$AllNotesOff
)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class WinMM {
    [DllImport("winmm.dll")] public static extern int midiOutGetNumDevs();
    [DllImport("winmm.dll")] public static extern int midiOutGetDevCaps(IntPtr id, byte[] caps, int size);
    [DllImport("winmm.dll")] public static extern int midiOutOpen(out IntPtr h, int id, IntPtr cb, IntPtr inst, int flags);
    [DllImport("winmm.dll")] public static extern int midiOutShortMsg(IntPtr h, int msg);
    [DllImport("winmm.dll")] public static extern int midiOutClose(IntPtr h);
}
'@

function Get-Devices {
    $n = [WinMM]::midiOutGetNumDevs()
    for ($i = 0; $i -lt $n; $i++) {
        $caps = New-Object byte[] 96
        $rc = [WinMM]::midiOutGetDevCaps([IntPtr]$i, $caps, $caps.Length)
        $name = if ($rc -eq 0) { [Text.Encoding]::Default.GetString($caps, 8, 32).Split([char]0)[0] } else { '?' }
        "  index $i : $name"
    }
}

if ($List) {
    "MIDI out devices:"
    Get-Devices
    return
}

$h = [IntPtr]::Zero
$rc = [WinMM]::midiOutOpen([ref]$h, $Device, [IntPtr]::Zero, [IntPtr]::Zero, 0)
if ($rc -ne 0) {
    "cannot open MIDI out device $Device (rc=$rc). Available:"
    Get-Devices
    exit 1
}

$ch = $Channel -band 0x0F
try {
    if ($AllNotesOff) {
        foreach ($c in 0..15) {
            [WinMM]::midiOutShortMsg($h, (0xB0 -bor $c -bor (123 -shl 8))) | Out-Null   # all notes off
            [WinMM]::midiOutShortMsg($h, (0xB0 -bor $c -bor (64 -shl 8))) | Out-Null    # sustain off
        }
        "all notes off, all channels"
        return
    }
    if ($Patch -ge 0) {
        [WinMM]::midiOutShortMsg($h, (0xC0 -bor $ch -bor (($Patch -band 0x7F) -shl 8))) | Out-Null
        "channel $ch -> patch $Patch"
    }
    if ($Notes.Trim()) {
        $noteList = @($Notes -split '[,;\s]+' | Where-Object { $_ -ne '' } | ForEach-Object { [int]$_ })
        foreach ($n in $noteList) {
            [WinMM]::midiOutShortMsg($h, (0x90 -bor $ch -bor (($n -band 0x7F) -shl 8) -bor (($Velocity -band 0x7F) -shl 16))) | Out-Null
        }
        "notes [$($noteList -join ',')] on channel $ch, velocity $Velocity"
        Start-Sleep -Milliseconds $Duration
        foreach ($n in $noteList) {
            [WinMM]::midiOutShortMsg($h, (0x80 -bor $ch -bor (($n -band 0x7F) -shl 8))) | Out-Null
        }
        "off"
    }
}
finally {
    [WinMM]::midiOutClose($h) | Out-Null
}
