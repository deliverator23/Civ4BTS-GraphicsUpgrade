// Timing for the log: what GraphicsUpgrade costs, averaged over the frames between the log's frame lines.
//
// CPU: the time spent handling the game's draws, less the game's own plain draws within it, plus GraphicsUpgrade's
// work at each Present; and the frame time. GPU: GraphicsUpgrade's own whole passes (the hill shadow mask, the
// shadow blur), with timestamp queries read back a few frames later so nothing waits for the GPU. Where the driver
// has no timestamp queries, only the CPU side is reported.
#pragma once
#include "common.h"

#include <string>

class Perf
{
public:
    explicit Perf(IDirect3DDevice9* device);
    ~Perf() { ReleaseAll(); }

    static double Now();  // seconds (QueryPerformanceCounter)

    // CPU time, in seconds: handling one of the game's draws (all of it), the game's own plain draw within it, and
    // GraphicsUpgrade's work at a Present.
    void AddDraw(double seconds) { m_drawSeconds += seconds; }
    void AddGameDraw(double seconds) { m_gameDrawSeconds += seconds; }
    void AddPresent(double seconds) { m_presentSeconds += seconds; }

    // GPU passes, bracketed by Begin and End within one frame.
    enum Pass
    {
        kHillMask,
        kShadowBlur,
        kPasses
    };
    void Begin(Pass pass) { Stamp(pass, 0); }
    void End(Pass pass) { Stamp(pass, 1); }
    void Cancel(Pass pass);  // bracketed, but the pass didn't run after all
    void CountHillBuild() { ++m_hillBuilds; }

    void EndFrame();    // just before the game's Present
    void StartFrame();  // just after it
    void Flush();       // wait for the GPU results still out (before a Reset, and at the end)
    // "frame time ...; CPU ...; GPU ..." for the frames since the last call, which starts a new count.
    std::string Summary();

    void ReleaseAll();  // the queries (before a Reset, and at the end); made again on use
    ULONG InternalRefs() const { return m_internalRefs; }

private:
    static const int kRing = 4;  // frames in flight: a frame's queries are read kRing frames later
    struct Slot
    {
        IDirect3DQuery9 *disjoint = nullptr, *frequency = nullptr, *stamps[kPasses][2] = {};
        bool issued[kPasses][2] = {}, pending = false;
    };
    void Stamp(Pass pass, int end);
    bool EnsureQueries();
    void Collect(Slot& slot, bool wait);

    IDirect3DDevice9* m_dev;
    Slot m_slots[kRing];
    int m_slot = 0;
    bool m_tried = false, m_gpu = false, m_inFrame = false;
    bool m_noTimestamps = false;  // the driver has none: not tried again
    ULONG m_internalRefs = 0;
    double m_lastPresent = 0, m_frameSeconds = 0;
    unsigned m_frames = 0, m_hillBuilds = 0;
    double m_drawSeconds = 0, m_gameDrawSeconds = 0, m_presentSeconds = 0;
    double m_gpuMs[kPasses] = {};
    unsigned m_gpuCount[kPasses] = {};
};
