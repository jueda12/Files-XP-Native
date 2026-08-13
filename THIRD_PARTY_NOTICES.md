# Third-party notices

Binary packages include the official Windows x64 command-line components from 7-Zip 26.02:
`7z.exe` and `7z.dll`, Copyright (C) 1999-2026 Igor Pavlov.

The files are acquired from the pinned `7zip-bin-full` 26.2.1 registry archive and verified by
SHA-256 before packaging. The complete upstream `License.txt` is distributed beside the binaries.
It describes the GNU LGPL, BSD, and unRAR restriction that apply to the corresponding 7-Zip code.
The unRAR code must not be used to recreate the proprietary RAR compression algorithm.

7-Zip source and license information: <https://www.7-zip.org/>

The FTP/FTPS manager invokes the `curl.exe` supplied with supported Windows 10 installations.
That executable is an operating-system component and is not redistributed in Files XP Native
packages. curl is provided under the curl license; source and copyright information are available
from <https://curl.se/docs/copyright.html>.
