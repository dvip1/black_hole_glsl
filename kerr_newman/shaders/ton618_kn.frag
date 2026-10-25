#version 330 core
// TON 618 as a Kerr–Newman black hole (mass M, spin a, charge Q).
//
// Units: G = c = M = 1, so the event horizon sits at r+ = 1 + sqrt(1 - a^2 - Q^2).
//
// Geodesics are integrated in *outgoing* Kerr–Schild Cartesian coordinates:
//     g_{μν} = η_{μν} + f k_μ k_ν ,   g^{μν} = η^{μν} - f k^μ k^ν
//     f = r^2 (2Mr - Q^2) / (r^4 + a^2 z^2)
//     k = (1, -(r x - a y)/(r^2 + a^2), -(r y + a x)/(r^2 + a^2), -z/r)
// and r is the Boyer–Lindquist radius, the root of
//     r^4 - (x^2 + y^2 + z^2 - a^2) r^2 - a^2 z^2 = 0 .
// The inverse metric is exact and there is no singularity on the spin axis.
// Photons follow Hamilton's equations for
//     H = 1/2 g^{μν} p_μ p_ν
// with p_t conserved. Rays are traced backwards in time from the camera
// (time-reversed momentum, p_t = +1). The outgoing chart matters here: a ray in
// the shadow, followed back in time, heads for the *past* horizon, which is
// regular only in outgoing coordinates. In the usual ingoing chart such rays
// hover just outside r+ forever and numerical error flings them back out.
//
// The spin axis is +z and the accretion disk lies in the z = 0 plane.

in vec2 vUV;
out vec4 fragColor;

uniform vec2  uResolution;
uniform float uTime;
uniform vec3  uCam;   // yaw, pitch, distance (in M)
uniform vec4  uBH;    // a, Q, r_horizon, r_isco
uniform vec2  uFlags; // jets on/off, ergosphere on/off

const float DISK_OUT  = 30.0;
const float T_DISK    = 11000.0;  // peak temperature (K), artistic: a hot, blue-white quasar disk
const int   MAX_STEPS = 420;
const float PI        = 3.14159265359;

float A, Q2, RH, ISCO;

// ------------------------------------------------------------------ noise

float hash13(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float vnoise(vec3 p)
{
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash13(i + vec3(0,0,0)), hash13(i + vec3(1,0,0)), f.x),
                   mix(hash13(i + vec3(0,1,0)), hash13(i + vec3(1,1,0)), f.x), f.y),
               mix(mix(hash13(i + vec3(0,0,1)), hash13(i + vec3(1,0,1)), f.x),
                   mix(hash13(i + vec3(0,1,1)), hash13(i + vec3(1,1,1)), f.x), f.y), f.z);
}

float fbm(vec3 p)
{
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) {
        s += a * vnoise(p);
        p = p * 2.03 + vec3(1.7, 9.2, 3.1);
        a *= 0.5;
    }
    return s;
}

float fbm3(vec3 p)
{
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 3; i++) {
        s += a * vnoise(p);
        p = p * 2.1 + vec3(4.1, 1.3, 7.7);
        a *= 0.5;
    }
    return s;
}

// ------------------------------------------------------------------ colour

vec3 blackbody(float t)
{
    t = clamp(t, 1000.0, 40000.0) / 100.0;
    vec3 c;
    c.r = t <= 66.0 ? 1.0 : 1.29293618606 * pow(t - 60.0, -0.1332047592);
    c.g = t <= 66.0 ? 0.39008157876 * log(t) - 0.63184144378
                    : 1.12989086089 * pow(t - 60.0, -0.0755148492);
    c.b = t >= 66.0 ? 1.0 : (t <= 19.0 ? 0.0 : 0.54320678911 * log(t - 10.0) - 1.19625408914);
    return pow(clamp(c, 0.0, 1.0), vec3(2.2));
}

// ------------------------------------------------------------------ Kerr–Newman metric

float ksRadius(vec3 x)
{
    float a2 = A * A;
    float b = dot(x, x) - a2;
    float r2 = 0.5 * (b + sqrt(b * b + 4.0 * a2 * x.z * x.z));
    return sqrt(max(r2, 1e-8));
}

void ksField(vec3 x, out float r, out float f, out vec3 k)
{
    r = ksRadius(x);
    float r2 = r * r, a2 = A * A;
    f = r2 * (2.0 * r - Q2) / (r2 * r2 + a2 * x.z * x.z);
    // outgoing chart: k_out = -k_in(-a), same g_tφ (rotation sense) as the ingoing one
    k = -vec3((r * x.x - A * x.y) / (r2 + a2),
              (r * x.y + A * x.x) / (r2 + a2),
              x.z / r);
}

// f (k^μ p_μ)^2 for the time-reversed ray (p_t = +1, k^t = -1).
float ksPotential(vec3 x, vec3 p)
{
    float r, f; vec3 k;
    ksField(x, r, f, k);
    float kp = dot(k, p) - 1.0;
    return f * kp * kp;
}

// Hamilton's equations: dx/dλ = ∂H/∂p,  dp/dλ = -∂H/∂x = 1/2 ∇[f (k·p)^2].
void geodesic(vec3 x, vec3 p, out vec3 dx, out vec3 dp)
{
    float r, f; vec3 k;
    ksField(x, r, f, k);
    float kp = dot(k, p) - 1.0;
    dx = p - f * kp * k;

    float e = 5e-3 * max(r, 1.0);   // ~cbrt(float eps): balances truncation vs round-off
    dp.x = ksPotential(x + vec3(e, 0, 0), p) - ksPotential(x - vec3(e, 0, 0), p);
    dp.y = ksPotential(x + vec3(0, e, 0), p) - ksPotential(x - vec3(0, e, 0), p);
    dp.z = ksPotential(x + vec3(0, 0, e), p) - ksPotential(x - vec3(0, 0, e), p);
    dp *= 0.25 / e;
}

// ------------------------------------------------------------------ accretion disk

float diskTempProfile(float r)
{
    float x = ISCO / r;
    return pow(x, 0.75) * pow(max(1.0 - sqrt(x), 0.0), 0.25) / 0.488;
}

// Keplerian angular velocity of a neutral particle on a circular equatorial
// Kerr–Newman orbit: Ω = sqrt(Mr - Q²) / (r² + a sqrt(Mr - Q²)).
float keplerOmega(float r)
{
    float s = sqrt(max(r - Q2, 0.0));
    return s / (r * r + A * s);
}

float diskDensity(float r, float phi)
{
    const float P = 16.0;
    float omega = keplerOmega(r) * 9.0;      // sped up so the swirl is visible
    float density = 0.0;
    for (int k = 0; k < 2; k++) {
        float tl = mod(uTime + float(k) * P * 0.5, P);
        float w  = 1.0 - abs(2.0 * tl / P - 1.0);
        float a  = phi - omega * tl + float(k) * 2.1;   // pattern co-rotates (+φ)
        vec3 q = vec3(r * 0.7, cos(a) * 6.0, sin(a) * 6.0) + float(k) * 13.7;
        float n = fbm(q);
        // spiral arms: logarithmic spirals winding with the flow
        float arms = 0.5 + 0.5 * sin(2.0 * a + 3.5 * log(r) + n * 3.0);
        float rings = 1.0 - abs(2.0 * vnoise(vec3(r * 3.0, cos(a) * 1.5, sin(a) * 1.5) + float(k) * 5.3) - 1.0);
        density += w * (n * 0.9 + arms * 0.35 + rings * rings * 0.4);
    }
    return density;
}

// Shade a disk crossing. p is the time-reversed photon momentum (p_t = +1).
vec4 shadeDisk(vec3 hit, vec3 p)
{
    float rho = length(hit.xy);
    float r = sqrt(max(rho * rho - A * A, 0.0));   // BL radius on the equator
    float rIn = max(ISCO * 0.75, RH * 1.1);        // plunging gas fades out before the horizon
    if (r < rIn || r > DISK_OUT) return vec4(0.0);

    float edge = smoothstep(rIn, max(ISCO * 1.1, rIn + 0.2), r)
               * (1.0 - smoothstep(DISK_OUT * 0.5, DISK_OUT, r));

    // Emitter 4-velocity u = u^t (∂_t + Ω ∂_φ); ∂_φ = (-y, x, 0) in KS coordinates.
    float omega = keplerOmega(r);
    float rr, f; vec3 k;
    ksField(hit, rr, f, k);
    float kV = 1.0 + omega * (-k.x * hit.y + k.y * hit.x);
    float gVV = -1.0 + omega * omega * rho * rho + f * kV * kV;
    if (gVV >= 0.0) return vec4(0.0);              // superluminal: inside the plunge region
    float ut = inversesqrt(-gVV);

    // Redshift g = E_obs / E_emit with the physical momentum = -p.
    float L = hit.x * p.y - hit.y * p.x;
    float g = min(1.0 / max(ut * (1.0 + omega * L), 1e-3), 3.0);

    float phi  = atan(hit.y, hit.x);
    float prof = diskTempProfile(max(r, ISCO));
    float dens = diskDensity(r, phi);

    vec3 col = blackbody(T_DISK * prof * g);
    float intensity = pow(prof, 2.5) * pow(g, 3.0) * 0.9 * dens;
    float alpha = clamp(edge * (0.25 + dens * 0.7), 0.0, 1.0);
    return vec4(col * intensity * edge, alpha);
}

// ------------------------------------------------------------------ jets & ergosphere

// Relativistic bipolar jets along the spin axis (Blandford–Znajek-ish look).
vec3 jetEmission(vec3 x, vec3 dir)
{
    float az = abs(x.z);
    if (az < RH * 0.8 || az > 80.0) return vec3(0.0);
    float rho = length(x.xy);
    float w = 0.25 + 0.05 * az;
    float core = exp(-rho * rho / (w * w));
    if (core < 0.01) return vec3(0.0);

    float sgn = sign(x.z);
    float t = uTime * 6.0;
    float twist = atan(x.y, x.x) + az * 0.15;
    float knots = fbm3(vec3(cos(twist) * 1.5, sin(twist) * 1.5, az * 0.35 - t * 0.3 * sgn));
    float dens = core * (0.3 + knots * 1.4)
               * smoothstep(RH * 0.8, RH * 3.0, az) * exp(-az / 22.0);

    // Doppler boost: β = 0.6 along ±z; photon heads opposite to the traced direction.
    const float beta = 0.6, gamma = 1.25;
    float D = 1.0 / (gamma * (1.0 - beta * dot(vec3(0, 0, sgn), -dir)));
    vec3 tint = mix(vec3(0.25, 0.45, 1.0), vec3(0.75, 0.6, 1.0), knots);
    return tint * dens * D * D * D * 0.5;
}

// Faint shimmer filling the ergoregion, r+ < r < M + sqrt(M² - Q² - a² cos²θ).
vec3 ergoEmission(vec3 x, float r)
{
    float c = x.z / r;
    float rErgo = 1.0 + sqrt(max(1.0 - Q2 - A * A * c * c, 0.0));
    if (r > rErgo || r < RH) return vec3(0.0);
    float shell = smoothstep(rErgo - 0.25 * (rErgo - RH), rErgo, r) * (1.0 - smoothstep(rErgo * 0.97, rErgo, r));
    float n = fbm3(x * 4.0 + vec3(0.0, 0.0, uTime * 0.7));
    return vec3(0.5, 0.3, 1.0) * shell * (0.3 + n) * 1.5;
}

// ------------------------------------------------------------------ sky

vec3 starLayer(vec3 d, float scale, float threshold, float seed)
{
    vec3 p  = d * scale;
    vec3 id = floor(p);
    vec3 f  = fract(p) - 0.5;
    float h = hash13(id + seed);
    if (h < threshold) return vec3(0.0);
    vec3 off = vec3(hash13(id + seed + 1.0), hash13(id + seed + 2.0), hash13(id + seed + 3.0)) - 0.5;
    float dist = length(f - off * 0.6);
    float bright = pow(hash13(id + seed + 4.0), 8.0) * 6.0 + 0.15;
    vec3 tint = blackbody(mix(3000.0, 16000.0, hash13(id + seed + 5.0)));
    return tint * bright * exp(-dist * dist * 160.0);
}

vec3 sky(vec3 d)
{
    vec3 col = vec3(0.0);
    col += starLayer(d, 90.0,  0.965, 0.0);
    col += starLayer(d, 180.0, 0.975, 11.0);
    col += starLayer(d, 360.0, 0.985, 23.0) * 0.6;

    // deep teal/violet nebula, distinct from the warm Schwarzschild version
    float n1 = fbm(d * 5.0 + 5.0);
    float n2 = fbm(d * 9.0 - 2.0);
    vec3 neb = mix(vec3(0.02, 0.05, 0.08), vec3(0.09, 0.03, 0.12), smoothstep(0.3, 0.7, n2));
    col += neb * pow(n1, 3.0) * 0.9;
    return col;
}

// ------------------------------------------------------------------ main

vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    A = uBH.x; Q2 = uBH.y * uBH.y; RH = uBH.z; ISCO = uBH.w;

    vec2 frag = gl_FragCoord.xy;
    vec2 uv = (frag - 0.5 * uResolution) / uResolution.y;

    // camera orbiting the spin axis (+z is up)
    float yaw   = uCam.x + uTime * 0.02;
    float pitch = uCam.y;
    float dist  = uCam.z;
    vec3 ro = dist * vec3(cos(pitch) * cos(yaw), cos(pitch) * sin(yaw), sin(pitch));
    vec3 fw = normalize(-ro);
    vec3 rt = normalize(cross(fw, vec3(0.0, 0.0, 1.0)));
    vec3 up = cross(rt, fw);
    float roll = -0.08;
    vec3 rt2 = rt * cos(roll) + up * sin(roll);
    vec3 up2 = up * cos(roll) - rt * sin(roll);
    vec3 d = normalize(fw * 1.6 + uv.x * rt2 + uv.y * up2);

    // Scale the spatial momentum so the time-reversed p (p_t = +1) is null:
    //   s^2 (1 - f (k·d)^2) + 2 f (k·d) s - (1 + f) = 0
    vec3 x = ro;
    float r0, f0; vec3 k0;
    ksField(x, r0, f0, k0);
    float kd = dot(k0, d);
    float qa = 1.0 - f0 * kd * kd, qb = 2.0 * f0 * kd, qc = -(1.0 + f0);
    float s = (-qb + sqrt(max(qb * qb - 4.0 * qa * qc, 0.0))) / (2.0 * qa);
    vec3 p = d * s;

    vec3 col = vec3(0.0);
    vec3 glow = vec3(0.0);
    vec3 ergoGlow = vec3(0.0);  // only shown for rays that escape: every captured ray crosses the ergoregion
    float trans = 1.0;
    bool escaped = false;
    vec3 dir = d;
    float escapeR = max(dist * 1.6, 90.0);
    bool jets = uFlags.x > 0.5, ergo = uFlags.y > 0.5;

    float jitter = hash13(vec3(frag, fract(uTime * 7.0) * 61.0));
    for (int i = 0; i < MAX_STEPS; i++) {
        float r = ksRadius(x);
        if (r < RH * 1.01) break;
        if (r > escapeR && dot(x, dir) > 0.0) { escaped = true; break; }

        // affine step: finer near the hole, the disk plane, and the jet axis
        // shadow rays, traced backwards, creep toward the horizon asymptotically: keep steps fine there
        float h = clamp(0.07 * (r - RH * 0.95), 0.006, 3.0);
        float rhoXY = length(x.xy);
        if (rhoXY < DISK_OUT * 1.2) h = min(h, max(abs(x.z) * 0.5, 0.05 * r));
        if (jets && abs(x.z) < 80.0) h = min(h, max(rhoXY * 0.6, 0.25));
        if (i == 0) h *= 0.25 + jitter;   // per-pixel offset breaks up step banding

        // RK4
        vec3 k1x, k1p, k2x, k2p, k3x, k3p, k4x, k4p;
        geodesic(x, p, k1x, k1p);
        geodesic(x + 0.5 * h * k1x, p + 0.5 * h * k1p, k2x, k2p);
        geodesic(x + 0.5 * h * k2x, p + 0.5 * h * k2p, k3x, k3p);
        geodesic(x + h * k3x, p + h * k3p, k4x, k4p);
        vec3 prev = x;
        x += h / 6.0 * (k1x + 2.0 * k2x + 2.0 * k3x + k4x);
        p += h / 6.0 * (k1p + 2.0 * k2p + 2.0 * k3p + k4p);

        vec3 step = x - prev;
        float seg = length(step);
        dir = step / max(seg, 1e-6);

        // thin luminous haze over the disk
        float rc = length(x.xy);
        float thick = 0.12 + rc * 0.03;
        float haze = exp(-x.z * x.z / (thick * thick))
                   * smoothstep(ISCO * 0.8, ISCO * 1.6, rc)
                   * (1.0 - smoothstep(ISCO * 1.5, DISK_OUT * 1.1, rc));
        float hp = diskTempProfile(max(rc, ISCO));
        glow += trans * haze * seg * blackbody(T_DISK * hp * 0.8) * hp * hp * 0.03;

        if (jets) glow += trans * seg * jetEmission(x, dir);
        if (ergo) ergoGlow += trans * seg * ergoEmission(x, ksRadius(x));

        if (prev.z * x.z < 0.0) {
            float t = prev.z / (prev.z - x.z);
            vec4 dsk = shadeDisk(mix(prev, x, t), p);
            col += trans * dsk.rgb * dsk.a;
            trans *= 1.0 - dsk.a;
            if (trans < 0.01) break;
        }
    }

    col += glow;
    if (escaped) {   // rays that run out of steps are skimming the horizon: treat as captured
        col += ergoGlow;
        if (trans > 0.01) col += trans * sky(dir);
    }

    col *= 1.1;
    col = aces(col);
    col = pow(col, vec3(1.0 / 2.2));

    vec2 q = frag / uResolution;
    col *= 0.55 + 0.45 * pow(16.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y), 0.2);
    col += (hash13(vec3(frag, fract(uTime) * 100.0)) - 0.5) / 255.0;

    fragColor = vec4(col, 1.0);
}
