HFVExplorer help
===============
HFVExplorer.chm is compiled HTML Help for the Windows 7-11 x64 target build. Keep it next to
HFVExplorer.exe. Help, F1 and Shift+F1 open the manual; the old WinHelp
placeholder is no longer used. Per-control context help is not implemented.

The default page describes changes in the unofficial x64 build. The original
1999 manual, screenshots, copyright and disclaimer are preserved unchanged.
The compiled manual uses 43 separate topic pages, so scrolling cannot cross
section boundaries. Contents and cross-references navigate between pages.
The preserved single-page source and historical homepage are not compiled.
Historical instructions and external links can be obsolete. See SOURCE.txt
for provenance and the downloaded archive's SHA256.

Rebuild with ..\build-help.ps1 (Microsoft HTML Help Workshop required).
build-x64.ps1 copies the existing CHM alongside the executable. HTML sources
can also be read offline by opening start.html in a web browser.

To regenerate the section pages/navigation from HFVExplorerHelp.html, install
Python packages beautifulsoup4 and html5lib, then run split-help.py. The script
checks local links, anchors, images and image maps. Recompile afterwards.
