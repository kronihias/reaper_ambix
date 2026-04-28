Reaper ambiX read/write plug-in
==========

> Reaper plug-in that adds read/write support for .ambix files according the ambiX (Ambisonics eXchangeable) specifications [1].

[1] http://iem.kug.ac.at/fileadmin/media/iem/projects/2011/ambisonics11_nachbar_zotter_sontacchi_deleflie.pdf


> This software uses *libambix* by IOhannes m zmölnig which is licensed under LGPL.
> https://github.com/umlaeute/ambix


*reaper_ambix* is based on Xenakios reaper_libsndfilewrapper
https://code.google.com/p/reaperlibsndfilewrapper/



binaries:
----------
ready to use 32/64 bit binaries for Windows and MacOS can be found at:
http://www.matthiaskronlachner.com

**GitHub Releases:**
Signed installers for Windows (.exe) and macOS (.pkg) are automatically built and available in [GitHub Releases](https://github.com/kronihias/reaper_ambix/releases).

To create a new release:
1. Update the `VERSION` file with the new version number
2. Commit and push the change
3. Create a GitHub release with tag `vX.Y.Z` (matching VERSION)
4. Publish the release to trigger automated builds

See [.github/CODE_SIGNING.md](.github/CODE_SIGNING.md) for code signing setup details.


building yourself
--------------

- cmake, working build environment
- libambix


authors
-----------
2014-2026 Matthias Kronlachner

Contact:
m.kronlachner (ät) gmail.com
www.matthiaskronlachner.com
