#version 450
layout(std140, set = 0, binding = 0) uniform UBO {
    mat4 MVP;
    vec4 OutputSize;
    vec4 OriginalSize;
    vec4 SourceSize;
    uint FrameCount;
} global;
#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 uv;
void main() { gl_Position = global.MVP * Position; uv = TexCoord; }
#pragma stage fragment
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
layout(push_constant) uniform Push { float GAIN; } params;
#pragma parameter GAIN "Gain" 1.0 0.1 2.0 0.1
layout(set = 0, binding = 3) uniform sampler2D LOOKUP;
layout(set = 0, binding = 4) uniform sampler2D PREPASS;
void main() {
    vec3 source = mix(textureLod(Source, uv, 1.0).rgb, texture(PREPASS, uv).rgb, 0.5);
    FragColor = vec4(source * texture(LOOKUP, vec2(0.5)).rgb * params.GAIN, 1.0);
}
