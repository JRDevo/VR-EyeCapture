# License of the OBS Studio plugin (`obs/`)

SPDX-License-Identifier: GPL-2.0-or-later

Copyright (c) 2026 JRDevo

The OBS Studio source plugin in this folder (`obs/plugin.cpp`, `obs/live_consumer.cpp`, `obs/live_consumer.h`,
`obs/obs.def`; built as `obs-eyecapture.dll`) is free software: you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation, either version 2 of the License, or (at
your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

The full text of the GNU General Public License, version 2:
<https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt>

Why GPL: the plugin is built against OBS Studio's `libobs` headers and links `obs.dll`, and OBS Studio is licensed under
GPL-2.0-or-later.

The plugin also compiles `live_share_proto.h` (the shared-memory protocol with the recorder). That header is dual-licensed `PolyForm-Noncommercial-1.0.0 OR GPL-2.0-or-later`, so it is available to the
plugin under GPL-2.0-or-later.

Source code: the complete source of the plugin is in the `obs/` folder of <https://github.com/JRDevo/VR-EyeCapture>; build
steps in `obs/README.md`. The libobs headers are fetched from the OBS Studio repository at build time (OBS Studio 31.0.0,
<https://github.com/obsproject/obs-studio>); OBS Studio itself is not redistributed.

The rest of VR EyeCapture (the OpenXR layer, the SteamVR proxy and mirror, the tray app, the scripts and the documentation)
is NOT under this license: it is closed source under its own license (`LICENSE.md` in the repository root). Third-party parts: `THIRD_PARTY.md`.
