# VideoProcessorNative
Video frame extraction library using ffmpeg

## Requirements

Requires, at minimum, an LGPL static build of ffmpeg v5.1.x. This is not provided here, though it is referenced by default in [Griffeye.VideoProcessor.Native.csproj](Griffeye.VideoProcessor.Native.csproj). There are many options for compiling ffmpeg, though the simplest is probably [using vcpkg](https://trac.ffmpeg.org/wiki/CompilationGuide/vcpkg).

## Build

Update [CMakeLists.txt](CMakeLists.txt) and change the variables FFMPEG_NUGET_NAME and FFMPEG_NUGET_VERSION to the FFMpeg LGPL package to use.
Also update [Griffeye.VideoProcessor.Native.csproj](Griffeye.VideoProcessor.Native.csproj) and set the FFMPeg LGPL package to use.

### Windows
Need to have Cmake installed and on the path.
```
dotnet restore Griffeye.VideoProcessor.Native.csproj
cmake -S . -B build_windows/
cmake --build build_windows/ --config [Release|Debug]
```

### Linux
Need to have dotnet CLI installed and some build utils
`RUN apt update && DEBIAN_FRONTEND=noninteractive apt install -y build-essential cmake zip`

```
dotnet restore Griffeye.VideoProcessor.Native.csproj --packages Packages
cmake -S . -B build_linux/
cmake --build build_linux/ --config [Release|Debug]

```

## Architecture

![Architecture overview](architecture.png "Architecture overview")