// TEMP(transformers): appended to a build-local raudio.c by audio_latency.cmake.
unsigned int ps2AudioDeviceLatencyUs(void)
{
    if (!AUDIO.System.device.sampleRate) return 0;
    unsigned long long frames = (unsigned long long)AUDIO.System.device.playback.internalPeriodSizeInFrames * AUDIO.System.device.playback.internalPeriods;
    unsigned long long us = frames * 1000000 / AUDIO.System.device.sampleRate;
    return (unsigned int)(us > 300000 ? 300000 : us);
}
