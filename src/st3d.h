// st3d -- add the Spherical Video V2 'st3d' (stereoscopic 3D) box to the video sample entry of a finished MP4.
//
// Box: size 13, 'st3d', version 0, flags 0, stereo_mode (0 mono, 1 top-bottom, 2 left-right). It is appended as a
// child of the first video sample entry (moov/trak[vide]/mdia/minf/stbl/stsd/{hvc1,hev1,avc1,avc3}); every ancestor
// box size grows by 13. If 'moov' sits after 'mdat' (Media Foundation's default) the file is patched in place
// (only the moov region and what follows it are rewritten). If 'moov' precedes 'mdat', a 'free' box right after moov
// is shrunk when possible; otherwise the file is rewritten through a temp file and every stco/co64 chunk offset is
// shifted by 13.
#pragma once
#include <string>

// Returns true when the box is present afterwards (also when it already was). `report` gets a one-line summary.
bool PatchSt3d(const std::wstring& path, unsigned char stereoMode, std::string& report);
