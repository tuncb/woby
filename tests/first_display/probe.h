#pragma once
// Research-only, single-render-thread D3D11 completion probe.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "mapping_probe.h"
#include <bgfx/platform.h>
#include <d3d11.h>
#include <fstream>
#include <cstdlib>
#include <thread>

namespace display_probe {
using namespace mapping_probe;
inline Clock::time_point loadStart, startupStart;
inline Json totals = Json::object();
inline unsigned frames = 0;
inline double finishGpu() {
    const auto begin = Clock::now();
    auto* device = static_cast<ID3D11Device*>(bgfx::getInternalData()->context);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    ID3D11Query* query = nullptr;
    const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
    if (FAILED(device->CreateQuery(&description, &query))) {
        context->Release();
        throw std::runtime_error("Cannot create completion query");
    }
    context->End(query);
    context->Flush();
    HRESULT status;
    while ((status = context->GetData(query, nullptr, 0, 0)) == S_FALSE) {
        if (elapsed(begin) > 60000) { break; }
        std::this_thread::yield();
    }
    query->Release();
    context->Release();
    if (status != S_OK) { throw std::runtime_error("GPU completion failed or timed out"); }
    return elapsed(begin);
}
inline std::string outputPath() {
    char path[32768];
    const DWORD count = GetEnvironmentVariableA("WOBY_FIRST_DISPLAY_OUTPUT", path, sizeof(path));
    if (count == 0 || count >= sizeof(path)) { throw std::runtime_error("WOBY_FIRST_DISPLAY_OUTPUT is required"); }
    return std::string(path, count);
}
inline void save() {
    totals["stages"] = stages;
    totals["map"] = stats;
    std::ofstream output(outputPath());
    output << totals.dump(2) << '\n';
    if (!output) { throw std::runtime_error("Cannot write probe result"); }
}
}
