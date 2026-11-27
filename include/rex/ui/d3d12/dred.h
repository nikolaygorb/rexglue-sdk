#pragma once

struct ID3D12Device;

namespace rex::ui::d3d12 {
void LogDredDeviceLoss(ID3D12Device* device);
}
