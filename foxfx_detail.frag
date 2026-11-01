#version 450
// foxfx_detail.frag  (PASS 1)  -  HD detail + gachpala (foliage) boost
// Order: fox_net_custom -> foxfx_detail (eta) -> foxfx_final
// Luminance-only, noise coring, soft clip (halo nai). Green foliage-e (gach, palm, ghas) aro beshi, aro mosrin detail.
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(push_constant) uniform PC {
    float amount;      // 1.0 natural, 1.4 crisp, 2+ over-sharp
    float coring;      // 0.004 noise gate
    float clipAmt;     // 0.07 halo control
    float foliage;     // 0.9 : gachpala-r extra boost (0 = off)
} pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float core(float d, float t) { return sign(d) * max(abs(d) - t, 0.0); }

void main() {
    vec2 uv = vUV;
    vec2 px = 1.0 / vec2(textureSize(uScene, 0));
    vec3 c  = texture(uScene, uv).rgb;
    float L = luma(c);

    // 3x3 gaussian (fine)
    float b1 = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            b1 += luma(texture(uScene, uv + vec2(x, y) * px).rgb) * w;
        }
    b1 /= 16.0;
    // ring 3px (medium)
    float b2 = b1 * 2.0, ws = 2.0;
    for (int i = 0; i < 8; ++i) {
        float a = float(i) * 0.7853982;
        b2 += luma(texture(uScene, uv + vec2(cos(a), sin(a)) * 3.0 * px).rgb);
        ws += 1.0;
    }
    b2 /= ws;

    // foliage detect: sobuj-er dike jhuka + moderate saturation, ar local texture beshi hole
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    float sat = (mx - mn) / (mx + 1e-4);
    float green = smoothstep(0.015, 0.08, c.g - max(c.r, c.b) * 0.97) * smoothstep(0.08, 0.20, sat);
    float f = green * pc.foliage;

    float d1 = L - b1, d2 = b1 - b2;
    float t  = pc.coring * (1.0 - 0.45 * f);                     // foliage-e noise gate kom = chhoto pata-o dhora porbe
    float d  = (0.90 + 0.55 * f) * core(d1, t) + (0.60 + 0.25 * f) * core(d2, t * 0.8);
    d *= pc.amount * (1.0 + 0.55 * f);
    float cl = pc.clipAmt * (1.0 + 0.4 * f);
    d = cl * tanh(d / cl);

    float w = 1.0 - 0.5 * pow(2.0 * L - 1.0, 2.0);
    outColor = vec4(clamp(c + d * w, 0.0, 1.0), 1.0);
}
