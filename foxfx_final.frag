#version 450
// foxfx_final.frag  (PASS 2)  -  reflection + bloom
//  * Road: wet asphalt reflection (contact-point planar) - shudhu dark asphalt, patchy puddle
//  * Gloss objects (gari, plastic, glass): fresnel-based high reflection - uGlossMask-er bhitore shudhu
//  * Bloom: halka, kinaray/konay ektu beshi
// Order: fox_net_custom -> foxfx_detail -> foxfx_final.  UNTESTED on device.
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) uniform sampler2D uGlossMask;   // R8, screen-size: 1 = gari-r ongsho / plastic / glass, 0 = baki (stencil/material-ID pass theke)
layout(push_constant) uniform PC {
    float horizon;     // ~0.43
    float strength;    // road reflection (1.2 high, 0 = off)
    float satMax;      // 0.29
    float bloom;       // 0.07 halka
    vec4  hudRect;     // minimap/HUD rect (uv), 0 = off
    float glossRefl;   // gari/plastic reflection gain (1.5 = high, 1.0 = natural, 2.0 = super)
    float bloomThr;    // 0.65
    float bloomEdge;   // 0.9 : konay/kinaray extra bloom
    float pad;
} pc;                  // 64 bytes
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const int   MARCH_STEPS = 40;
const float MARCH_DS    = 0.010;

float hash21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), f.x), mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), f.x), f.y);
}
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

float rectMask(vec2 uv, vec4 r) {
    float e = 0.008;
    return smoothstep(r.x, r.x + e, uv.x) * (1.0 - smoothstep(r.z - e, r.z, uv.x))
         * smoothstep(r.y, r.y + e, uv.y) * (1.0 - smoothstep(r.w - e, r.w, uv.y));
}
// mask ektu fulaye - edge-er pixel-e road reflection jeno na pore
float glossDil(vec2 uv) {
    vec2 px = 1.0 / vec2(textureSize(uGlossMask, 0));
    float m = textureLod(uGlossMask, uv, 0.0).r;
    for (int i = 0; i < 8; ++i) {
        float a = float(i) * 0.7853982;
        m = max(m, textureLod(uGlossMask, clamp(uv + vec2(cos(a), sin(a)) * 4.0 * px, 0.0, 1.0), 0.0).r);
    }
    return m;
}
float roadMaskAt(vec2 uv) {
    vec3 c = textureLod(uScene, uv, 0.0).rgb;
    float mx = max(c.r, max(c.g, c.b));
    float sat = (mx - min(c.r, min(c.g, c.b))) / (mx + 1e-10);
    float below = smoothstep(pc.horizon + 0.02, pc.horizon + 0.06, uv.y);
    float m = below * (1.0 - smoothstep(pc.satMax - 0.05, pc.satMax + 0.05, sat))
                    * smoothstep(0.20, 0.28, mx) * (1.0 - smoothstep(0.85, 0.92, mx));
    return m * (1.0 - glossDil(uv)) * (1.0 - rectMask(uv, pc.hudRect));
}

// halka bloom, konay/kinaray beshi
vec3 applyBloom(vec3 col, vec2 uv) {
    if (pc.bloom <= 0.0) return col;
    vec2 px = 1.0 / vec2(textureSize(uScene, 0));
    vec2 q = (uv - 0.5) * 2.0;
    float edge = smoothstep(0.35, 1.25, length(q * vec2(1.0, 0.9)));
    float amt  = pc.bloom * (1.0 + pc.bloomEdge * edge);
    float rad  = 14.0 * (1.0 + 0.35 * edge);
    vec3 acc = vec3(0.0); float wsum = 0.0;
    for (int i = 0; i < 16; ++i) {
        float a = float(i) * 2.39996323;
        float r = sqrt((float(i) + 0.5) / 16.0);
        vec3 s = texture(uScene, clamp(uv + vec2(cos(a), sin(a)) * r * rad * px, 0.0, 1.0)).rgb;
        float b = smoothstep(pc.bloomThr, pc.bloomThr + 0.25, luma(s));
        float w = 1.0 - 0.6 * r;
        acc += s * b * w; wsum += w;
    }
    vec3 glow = acc / wsum * vec3(1.0, 0.96, 0.88);
    return col + glow * amt * 2.0 - col * glow * amt;
}

void main() {
    vec2 uv = vUV;
    vec3 base = texture(uScene, uv).rgb;
    vec2 px = 1.0 / vec2(textureSize(uScene, 0));
    vec3 col = base;
    float gloss = texture(uGlossMask, uv).r;
    float roadM = roadMaskAt(uv);

    // ---------- wet road ----------
    if (roadM >= 0.01 && pc.strength > 0.0) {
        float yb = pc.horizon;
        for (int i = 1; i <= MARCH_STEPS; ++i) {
            float p = uv.y - float(i) * MARCH_DS;
            if (p < pc.horizon) break;
            if (roadMaskAt(vec2(uv.x, p)) < 0.5) { yb = p + MARCH_DS * 0.5; break; }
        }
        float d  = max(uv.y - yb, 0.0);
        float ry = 2.0 * yb - uv.y;
        float k  = clamp((uv.y - pc.horizon) / (1.0 - pc.horizon), 0.0, 1.0);
        float H  = float(textureSize(uScene, 0).y);
        float rip = sin(uv.y * H * 0.3 + sin(uv.x * 40.0) * 3.0) * 0.002 * k;
        vec2 muv = vec2(uv.x + rip, ry);
        float bl = 1.2 + d * 14.0;
        vec3 mir = vec3(0.0); float wsum = 0.0;
        for (int i = -4; i <= 4; ++i) {
            float w = exp(-float(i * i) / 10.0);
            mir += texture(uScene, clamp(muv + vec2(0.0, float(i) * px.y * bl), 0.0, 1.0)).rgb * w;
            wsum += w;
        }
        mir = clamp((mir / wsum - 0.5) * 1.15 + 0.5, 0.0, 1.0) * 0.88;
        mir *= 1.0 + 0.35 * smoothstep(0.6, 1.0, luma(mir));

        float vis   = smoothstep(0.0, 0.10, ry) * (1.0 - smoothstep(0.98, 1.0, ry));
        float distA = 1.0 - 0.65 * smoothstep(0.10, 0.45, d);
        float selfR = 1.0 - 0.7 * roadMaskAt(clamp(vec2(muv.x, ry), 0.0, 1.0));
        float fres  = pow(1.0 - k, 0.7);
        float mxB   = max(base.r, max(base.g, base.b));
        float refl  = 1.0 - 0.85 * smoothstep(0.45, 0.70, mxB);        // dark asphalt-e beshi, light concrete-e kom
        vec2  pp    = vec2(uv.x * 6.0, (uv.y - pc.horizon) * 14.0);
        float wet   = mix(0.40, 1.0, smoothstep(0.30, 0.65, vnoise(pp) * 0.65 + vnoise(pp * 2.7) * 0.35));
        float hitG  = texture(uGlossMask, vec2(uv.x, clamp(yb - 0.012, 0.0, 1.0))).r;
        float ra    = clamp((0.30 + 0.45 * fres) * pc.strength, 0.0, 0.88) * roadM * vis * distA
                    * mix(1.0, selfR, 0.8) * refl * wet * (1.0 - 0.35 * hitG);
        col = base * (1.0 - 0.16 * roadM * refl * wet);
        col = mix(col, mir, ra);
    }

    // ---------- gloss objects: gari, plastic, glass (Fresnel, additive = detail thake) ----------
    if (gloss > 0.01 && pc.glossRefl > 0.0) {
        // object-er upor/nichu khuje relative height t (0 = top, 1 = bottom)
        float top = uv.y, bot = uv.y;
        for (int i = 1; i <= 28; ++i) { float p = uv.y - float(i) * 0.016; top = p; if (texture(uGlossMask, vec2(uv.x, clamp(p, 0.0, 1.0))).r < 0.5) break; }
        for (int i = 1; i <= 28; ++i) { float p = uv.y + float(i) * 0.016; bot = p; if (texture(uGlossMask, vec2(uv.x, clamp(p, 0.0, 1.0))).r < 0.5) break; }
        float t = clamp((uv.y - top) / max(bot - top, 0.02), 0.0, 1.0);

        // environment: upore-facing panel -> sky, side/nichu -> street
        float skyY = clamp(pc.horizon - 0.08 - 0.28 * (1.0 - t), 0.02, 0.98);
        vec2  skyUV = vec2(uv.x, skyY);
        vec2  stUV  = vec2(1.0 - uv.x, pc.horizon - 0.02);
        vec3 sky = vec3(0.0), st = vec3(0.0);
        for (int i = 0; i < 8; ++i) {
            float a = float(i) * 2.39996323, r = sqrt((float(i) + 0.5) / 8.0);
            vec2 o = vec2(cos(a), sin(a)) * r * 10.0 * px;
            sky += texture(uScene, clamp(skyUV + o, 0.0, 1.0)).rgb;
            st  += texture(uScene, clamp(stUV  + o, 0.0, 1.0)).rgb;
        }
        vec3 env = mix(sky, st, smoothstep(0.35, 0.85, t)) / 8.0;

        // silhouette edge (grazing) - fresnel
        float inner = 0.0;
        for (int i = 0; i < 4; ++i) {
            float a = float(i) * 1.5707963 + 0.7854;
            inner += texture(uGlossMask, clamp(uv + vec2(cos(a), sin(a)) * 7.0 * px, 0.0, 1.0)).r;
        }
        float edgeness = 1.0 - inner * 0.25;
        float cosT = mix(0.30, 0.85, smoothstep(0.35, 0.90, t)) * (0.30 + 0.70 * (1.0 - edgeness));
        float F    = 0.04 + 0.96 * pow(1.0 - cosT, 5.0);                      // Schlick

        // material: dark glass/plastic beshi glossy, paint moderate, tyre/very dark kom
        float mx    = max(base.r, max(base.g, base.b));
        float dark  = 1.0 - smoothstep(0.10, 0.35, mx);
        float mat   = mix(0.70, 1.0, dark) * smoothstep(0.03, 0.09, mx);
        float objH  = bot - top;
        float sizeW = mix(0.30, 1.0, smoothstep(0.03, 0.15, objH));              // chhoto plastic (signal housing)-e kom, jate shape na harae
        float k     = clamp(F * mat * pc.glossRefl, 0.0, 0.55) * gloss * sizeW;
        // reflection paint/plastic-er rong-e tinted + ektu dark (overcast sky safed kore na dey)
        vec3 tint   = base / max(mx, 0.05);
        env = env * mix(vec3(1.0), tint, 0.5) * 0.72;

        col = col * (1.0 - 0.30 * k) + env * k;                                  // base-er detail rekhe reflection jog
        col += vec3(1.0, 0.98, 0.92) * smoothstep(0.70, 0.95, luma(env)) * F * mat * pc.glossRefl * 0.25 * gloss * sizeW;  // sun/sky highlight
    }

    col = applyBloom(col, uv);
    outColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
