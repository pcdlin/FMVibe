# MTS-ESP client

ODDSound's MTS-ESP client library, unchanged: `Client/libMTSClient.h` and `Client/libMTSClient.cpp` from
https://github.com/ODDSound/MTS-ESP at commit `f214739b8832e7f297cb9970d0c0efbf783f1462` (4 February 2026), under
the 0BSD license in `LICENSE`; and from the same commit its master library, `Master/libMTSMaster.h` and
`Master/libMTSMaster.cpp`, which only `fmvibe-mtsmaster` (`tools/mtsmaster.cpp`), a master for trying FMVibe by hand,
builds in.

It is built into the plugins only (`fmvibe-plugin`), not the engine, the browser demo or the tools. As the plugin
loads, it looks for libMTS, which an MTS-ESP master installs (`Common Files\MTS-ESP\LIBMTS.dll` on Windows,
`/usr/local/lib/libMTS.so` on Linux); without it, the plugin plays 12-TET or what MTS SysEx tunes it to.
`src/mtstuning.cpp` is FMVibe's side of it.

To update them, copy the four files from a newer commit and change the commit above.
