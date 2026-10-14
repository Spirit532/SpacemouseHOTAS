# Third-party notices

## vJoy

SpacemouseHOTAS feeds the vJoy virtual joystick driver directly. It does not bundle vJoy, but its feeder
(`src/platform/win32/vjoy_pad.cpp`) reproduces vJoy's device interface: the IOCTL codes, the
`JOYSTICK_POSITION_V3` structure and the HID report descriptor layout written by vJoyConf. These were derived
from the vJoy source code:

- Original project: vJoy by Shaul Eizikovich
- Maintained fork used here: https://github.com/BrunnerInnovation/vJoy

vJoy is distributed under the MIT License:

```
MIT License

Copyright (c) 2017 Shaul Eizikovich

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
```

## 3Dconnexion

3Dconnexion, SpaceMouse and 3DxWare are trademarks of 3Dconnexion. This project is not affiliated with or
endorsed by 3Dconnexion. Force capture talks to the installed 3DxWare driver through its SiApp interface
(`siappdll.dll`, loaded at runtime); no 3Dconnexion code or files are included.
