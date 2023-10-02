# VideoProcessorNative
Video frame extraction library using ffmpeg

## Requirements

Requires, at minimum, an LGPL static build of ffmpeg v5.1.x. This is not provided here, though it is referenced by default in [packages.config](packages.config). There are many options for compiling ffmpeg, though the simplest is probably [using vcpkg](https://trac.ffmpeg.org/wiki/CompilationGuide/vcpkg).

## Build

```
msbuild VideoProcessorNative.sln -restore -target:"Clean;Rebuild" -property:"Platform=x64;Configuration=Release"
```

## Architecture

![Architecture overview](architecture.png "Architecture overview")