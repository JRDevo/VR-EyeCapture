// st3d -- add the Spherical Video V2 'st3d' (stereoscopic 3D) box to the video sample entry of a finished MP4.
//
// Box: size 13, 'st3d', version 0, flags 0, stereo_mode (0 mono, 1 top-bottom, 2 left-right). It is appended as a
// child of the first video sample entry (moov/trak[vide]/mdia/minf/stbl/stsd/{hvc1,hev1,avc1,avc3,av01}); every ancestor
// box size grows by 13. If 'moov' sits after 'mdat' (Media Foundation's default) the file is patched in place
// (only the moov region and what follows it are rewritten). If 'moov' precedes 'mdat', a 'free' box right after moov
// is shrunk when possible; otherwise the file is rewritten through a temp file and every stco/co64 chunk offset is
// shifted by 13.
//
// Dome recordings (VR180 / VR360) also get the 'sv3d' box right after 'st3d' (Spherical Video V2 RFC):
//   sv3d { svhd (FullBox, metadata_source "OpenXR-EyeCapture\0"),
//          proj { prhd (FullBox, int32 16.16 yaw / pitch / roll = 0),
//                 equi (FullBox, uint32 0.32 projection_bounds_top / bottom / left / right) } }
// The bounds are the fraction of the full sphere NOT covered, measured from each edge: VR180 = 0 / 0 / 0.25 / 0.25
// (0x40000000), VR360 = all 0. The whole blob is inserted the same way (ancestors grow by its size).
#pragma once
#include <cstdint>
#include <string>

// Returns true when the box is present afterwards (also when it already was). `report` gets a one-line summary.
bool PatchSt3d(const std::wstring& path, unsigned char stereoMode, std::string& report);
// st3d (stereoMode: 1 top-bottom, 2 left-right) and, with sv3d = true, the spherical box with these equi bounds
// (top, bottom, left, right; nullptr = all 0).
bool PatchStereoSpherical(const std::wstring& path, unsigned char stereoMode, bool sv3d, const uint32_t equiBounds[4],
                          std::string& report);
