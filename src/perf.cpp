#include "perf.h"

#include <cstdio>

Perf::Perf(IDirect3DDevice9* device) : m_dev(device)
{
    m_lastPresent = Now();
}

double Perf::Now()
{
    static LARGE_INTEGER frequency = {};
    if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

bool Perf::EnsureQueries()
{
    if (m_tried || m_noTimestamps)
        return m_gpu;
    m_tried = true;
    ULONG before = DeviceRefs(m_dev);
    m_gpu = SUCCEEDED(m_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, nullptr)) &&
            SUCCEEDED(m_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, nullptr)) &&
            SUCCEEDED(m_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, nullptr));
    for (Slot& s : m_slots)
    {
        m_gpu = m_gpu && SUCCEEDED(m_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &s.disjoint)) &&
                SUCCEEDED(m_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &s.frequency));
        for (auto& pass : s.stamps)
            for (IDirect3DQuery9*& q : pass)
                m_gpu = m_gpu && SUCCEEDED(m_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q));
    }
    if (!m_gpu)
    {
        ReleaseAll();
        m_noTimestamps = true;
        Log::Write("timing: the driver has no timestamp queries; GPU times not reported");
    }
    else
        m_internalRefs += DeviceRefs(m_dev) - before;
    return m_gpu;
}

void Perf::Stamp(Pass pass, int end)
{
    if (!m_inFrame || !EnsureQueries())
        return;
    Slot& s = m_slots[m_slot];
    s.stamps[pass][end]->Issue(D3DISSUE_END);
    s.issued[pass][end] = true;
}

void Perf::Cancel(Pass pass)
{
    Slot& s = m_slots[m_slot];
    s.issued[pass][0] = s.issued[pass][1] = false;
}

void Perf::StartFrame()
{
    const double now = Now();
    m_frameSeconds += now - m_lastPresent;
    m_lastPresent = now;
    ++m_frames;
    if (!EnsureQueries())
        return;
    m_slot = (m_slot + 1) % kRing;
    Slot& s = m_slots[m_slot];
    if (s.pending)
        Collect(s, false);  // issued kRing frames ago
    for (auto& pass : s.issued)
        pass[0] = pass[1] = false;
    s.disjoint->Issue(D3DISSUE_BEGIN);
    m_inFrame = true;
}

void Perf::EndFrame()
{
    if (!m_inFrame)
        return;
    Slot& s = m_slots[m_slot];
    s.disjoint->Issue(D3DISSUE_END);
    s.frequency->Issue(D3DISSUE_END);
    s.pending = true;
    m_inFrame = false;
}

void Perf::Flush()
{
    for (Slot& s : m_slots)
        if (s.pending)
            Collect(s, true);
}

void Perf::Collect(Slot& s, bool wait)
{
    s.pending = false;
    // In the frame: results not in yet are left out. Waiting (Flush): until they are, within reason.
    auto get = [wait](IDirect3DQuery9* q, void* data, DWORD size) {
        HRESULT hr = q->GetData(data, size, wait ? D3DGETDATA_FLUSH : 0);
        for (int spins = 0; wait && hr == S_FALSE && spins < 1000000; ++spins)
            hr = q->GetData(data, size, D3DGETDATA_FLUSH);
        return hr == S_OK;
    };
    BOOL disjoint = TRUE;
    UINT64 frequency = 0;
    if (!get(s.disjoint, &disjoint, sizeof(disjoint)) || disjoint || !get(s.frequency, &frequency, sizeof(frequency)) ||
        !frequency)
        return;
    for (int p = 0; p < kPasses; ++p)
    {
        UINT64 t0 = 0, t1 = 0;
        if (s.issued[p][0] && s.issued[p][1] && get(s.stamps[p][0], &t0, sizeof(t0)) &&
            get(s.stamps[p][1], &t1, sizeof(t1)) && t1 >= t0)
        {
            m_gpuMs[p] += 1000.0 * static_cast<double>(t1 - t0) / static_cast<double>(frequency);
            ++m_gpuCount[p];
        }
    }
}

std::string Perf::Summary()
{
    char buf[512];
    const double frames = m_frames ? m_frames : 1;
    const double ours = (m_drawSeconds - m_gameDrawSeconds + m_presentSeconds) * 1000.0 / frames;
    int n = snprintf(buf, sizeof(buf),
                     "timing over %u frames: frame %.2f ms; GraphicsUpgrade CPU %.3f ms a frame (draws %.3f, Present "
                     "%.3f); hill shadow mask made %u times",
                     m_frames, m_frameSeconds * 1000.0 / frames, ours,
                     (m_drawSeconds - m_gameDrawSeconds) * 1000.0 / frames, m_presentSeconds * 1000.0 / frames,
                     m_hillBuilds);
    static const char* const kNames[kPasses] = {"hill shadow mask", "shadow blur"};
    if (m_gpu)
        for (int p = 0; p < kPasses && n > 0 && n < static_cast<int>(sizeof(buf)); ++p)
            n += snprintf(buf + n, sizeof(buf) - n, m_gpuCount[p] ? "; GPU %s %.3f ms each" : "; GPU %s: no results",
                          kNames[p], m_gpuCount[p] ? m_gpuMs[p] / m_gpuCount[p] : 0.0);
    m_frames = m_hillBuilds = 0;
    m_frameSeconds = m_drawSeconds = m_gameDrawSeconds = m_presentSeconds = 0;
    for (int p = 0; p < kPasses; ++p)
        m_gpuMs[p] = 0, m_gpuCount[p] = 0;
    return buf;
}

void Perf::ReleaseAll()
{
    for (Slot& s : m_slots)
    {
        IDirect3DQuery9** all[2 + 2 * kPasses] = {&s.disjoint, &s.frequency};
        int k = 2;
        for (auto& pass : s.stamps)
            for (IDirect3DQuery9*& q : pass)
                all[k++] = &q;
        for (IDirect3DQuery9** q : all)
            if (*q)
                (*q)->Release(), *q = nullptr;
        s.pending = false;
    }
    m_internalRefs = 0;
    m_tried = m_gpu = m_inFrame = false;
}
