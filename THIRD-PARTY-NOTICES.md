# Third-party notices

## Linux hid-playstation

The bundled webOS 4.4 compatibility module is derived from the DualSense output
protocol and driver lifecycle in Linux `hid-playstation`, first released in the
Linux 5.12 series. The module source is distributed under GPL-2.0 in
`kernel/hid-playstation-compat/`.

Copyright (C) 2020 Sony Interactive Entertainment

Linux kernel source: <https://github.com/torvalds/linux>

The webOS 4.4 module is compiled against LG's GPL-2.0 `linux-4.4-lg115x`
source and `lg1k` configuration from the `webOS 5.0 JO 2.0` source release for
OLED55GXRLA. The build verifies every enclosing LG source archive by SHA-256
before using it.

LG product source catalog: <https://opensource.lge.com/product>

## punktfunk-webos

The webOS DualSense Bluetooth feedback implementation is adapted from
[punktfunk-webos](https://github.com/dyptan-io/punktfunk-webos), commit
`7bd261a9f51c89b68994bf619a19b48ef827948a`.

MIT License

Copyright (c) 2026 dyptan-io

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## SDL-webOS

The IPK bundles SDL 2.30.12 from
[webosbrew/SDL-webOS](https://github.com/webosbrew/SDL-webOS), distributed
under the zlib license:

Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
