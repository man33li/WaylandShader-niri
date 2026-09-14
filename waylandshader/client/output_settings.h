#pragma once

namespace WaylandShader {

struct OutputSettings {
    static constexpr double MinimumGamma = 0.1;
    static constexpr double MaximumGamma = 5.0;
    static constexpr double MinimumSaturation = 0.0;
    static constexpr double MaximumSaturation = 2.0;

    bool shaderEnabled = true;
    bool colorEnabled = false;
    double gamma = 1.0;
    double saturation = 1.0;

    bool adjustsColor() const
    {
        return colorEnabled && (gamma != 1.0 || saturation != 1.0);
    }
};

} // namespace WaylandShader
