# Platform notes

Platform-specific findings from the tested configurations; these are not universal driver or operating-system claims.

## Windows background power throttling

Windows background power policy can materially affect hidden-window benchmark results.
The timing investigation compared an explicit process opt-out with the previous hidden-window configuration.
That change alters the measurement environment as well as the observed performance.
Use the same throttling state on both variants before attributing a difference to runtime code.
Report guest counters separately from host CPU-time and wall-time measurements.

## Windows AMD Vulkan compatibility

An integrated-GPU startup failure remained after the UMA allocation repair.
Disabling optional push descriptors made the tested Vulkan rendering path work.
Allocated descriptor sets retained the rendering features and shaders.
This is a qualified compatibility workaround, not proof of which component violated the Vulkan specification.
A simple descriptor-count overflow was not established by the investigation.

## UMA memory and renderer health

A small dedicated-memory carve-out can exhaust its budget even when shared system RAM is plentiful.
The investigated integrated-GPU logs showed tracked allocation beyond that local budget at boot.
The allocation repair and push-descriptor fallback addressed separate failures.
A fresh-card route could report exact guest counters even after the renderer lost its device.
Qualification must therefore check renderer errors and actual pictures as well as terminal counters.

## Controllers: selection and enumeration

The revised selector uses fresh button edges or rearmed stick crossings to switch devices.
Simultaneous competing activity keeps the current controller.
Enumeration uses increasing retry intervals, but the owning render thread still performs the call synchronously.
Rate limiting does not guarantee an individual scan is hitch-free.
Scripted tests avoid hardware-slot selection so attached controllers do not change test inputs.

## Host audio: replay qualification has limits

The host movie-audio controller was tested with 24 recorded or synthetic schedules.
Independent replay found no unexpected underruns, hard drops or unresolved repeat windows.
The requested low correction rate still failed for every recorded schedule in that report.
The source recorded producer-speed and pacing issues, rather than proving all movie timing solved.
Offline schedule results are not listening tests or whole-game audio qualification.
