#version 450
// foxfx_net.frag (PASS 0) - saturation / contrast / warm tone.
// Analytic approximation of net/fox_net_custom.h (the 3x3 conv net is NOT executed on GPU here).
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 c = texture(uScene, vUV).rgb;
    float L = dot(c, vec3(0.299, 0.587, 0.114));
    c += (c - L) * 0.16;                 // saturation
    c = (c - 0.5) * 1.06 + 0.5;          // contrast
    c *= vec3(1.012, 1.0, 0.988);        // warm
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
