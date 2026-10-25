#version 330 core
// TON 618 — ~66 billion solar-mass black hole powering a hyperluminous quasar.
//
// Units: Schwarzschild radius r_s = 1 (for TON 618 that's ~1300 AU).
// Photon paths are integrated through Schwarzschild spacetime using the
// well-known Newtonian-form geodesic equation  a = -3/2 * h^2 * r / |r|^5,
// where h = |r x v| is conserved. This reproduces the photon sphere (1.5 r_s),
// the shadow (~2.6 r_s), and the lensed back side of the disk wrapping over
// and under the hole.

in vec2 vUV;
out vec4 fragColor;

uniform vec2  uResolution;
uniform float uTime;
uniform vec3  uCam;          // yaw, pitch, distance

const float DISK_IN   = 3.0;    // innermost stable circular orbit = 3 r_s
const float DISK_OUT  = 18.0;
const float T_DISK    = 5200.0; // peak disk temperature (K), artistic
const int   MAX_STEPS = 600;
const float PI        = 3.14159265359;

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

// ------------------------------------------------------------------ colour

// Blackbody colour (Tanner Helland fit), returned in linear RGB, max channel = 1.
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

// ------------------------------------------------------------------ disk

// Novikov–Thorne-like radial temperature profile, normalised so peak = 1.
float diskTempProfile(float r)
{
    float x = DISK_IN / r;
    return pow(x, 0.75) * pow(max(1.0 - sqrt(x), 0.0), 0.25) / 0.488;
}

// Turbulent density, sheared by Keplerian rotation. Two layers are
// cross-faded on a cycle so the differential winding never shreds the pattern.
float diskDensity(float r, float phi)
{
    const float P = 14.0;
    float omega = 3.0 * pow(r, -1.5);
    float density = 0.0;
    for (int k = 0; k < 2; k++) {
        float tl = mod(uTime + float(k) * P * 0.5, P);
        float w  = 1.0 - abs(2.0 * tl / P - 1.0);
        float a  = phi + omega * tl + float(k) * 2.1;
        vec3 q = vec3(r * 1.3, cos(a) * 5.0, sin(a) * 5.0) + float(k) * 13.7;
        float n = fbm(q);
        // thin bright filaments on top of the broad turbulence
        float streak = 1.0 - abs(2.0 * vnoise(vec3(r * 6.0, cos(a) * 2.0, sin(a) * 2.0) + float(k) * 5.3) - 1.0);
        density += w * (n * 1.1 + streak * streak * 0.5);
    }
    return density;
}

// Emitted colour + opacity where the ray pierces the disk plane at `p`.
vec4 shadeDisk(vec3 p, vec3 rayDir)
{
    float r = length(p.xz);
    if (r < DISK_IN * 0.8 || r > DISK_OUT) return vec4(0.0);

    float phi = atan(p.z, p.x);
    float edge = smoothstep(DISK_IN * 0.8, DISK_IN * 1.15, r)
               * (1.0 - smoothstep(DISK_OUT * 0.45, DISK_OUT, r));

    // Relativistic beaming: orbital speed of a circular orbit, β = sqrt(M/(r-2M)), M = 1/2.
    float beta  = clamp(sqrt(0.5 / max(r - 1.0, 0.01)), 0.0, 0.99);
    float gamma = inversesqrt(1.0 - beta * beta);
    vec3  vdir  = normalize(vec3(-p.z, 0.0, p.x));
    float cosT  = dot(vdir, -rayDir);                  // toward observer
    float doppler = 1.0 / (gamma * (1.0 - beta * cosT));
    float gravz   = sqrt(max(1.0 - 1.0 / r, 0.0));     // gravitational redshift
    float g = doppler * gravz;

    float prof = diskTempProfile(max(r, DISK_IN));
    float dens = diskDensity(r, phi);

    vec3 col = blackbody(T_DISK * prof * g);
    float intensity = pow(prof, 3.0) * pow(g, 4.0) * 0.9 * dens;
    float alpha = clamp(edge * (0.35 + dens * 0.75), 0.0, 1.0);
    return vec4(col * intensity * edge, alpha);
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
    vec3 tint = blackbody(mix(2800.0, 14000.0, hash13(id + seed + 5.0)));
    return tint * bright * exp(-dist * dist * 160.0);
}

vec3 sky(vec3 d)
{
    vec3 col = vec3(0.0);
    col += starLayer(d, 90.0,  0.965, 0.0);
    col += starLayer(d, 180.0, 0.975, 11.0);
    col += starLayer(d, 360.0, 0.985, 23.0) * 0.6;

    // faint dusty galactic band + distant nebulosity
    vec3 n = normalize(vec3(0.3, 1.0, 0.45));
    float band = exp(-pow(dot(d, n), 2.0) * 14.0);
    float neb = fbm(d * 4.0 + 3.0);
    float dust = smoothstep(0.35, 0.75, fbm(d * 9.0 + 7.0));
    col += band * neb * mix(vec3(0.10, 0.08, 0.13), vec3(0.03, 0.025, 0.03), dust) * 0.6;
    col += vec3(0.02, 0.012, 0.03) * pow(neb, 3.0) * 0.5;
    return col;
}

// ------------------------------------------------------------------ main

vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    vec2 frag = gl_FragCoord.xy;
    vec2 uv = (frag - 0.5 * uResolution) / uResolution.y;

    // camera: orbit around the hole, slowly drifting
    float yaw   = uCam.x + uTime * 0.025;
    float pitch = uCam.y;
    float dist  = uCam.z;
    vec3 ro = dist * vec3(cos(pitch) * sin(yaw), sin(pitch), cos(pitch) * cos(yaw));
    vec3 fw = normalize(-ro);
    vec3 rt = normalize(cross(fw, vec3(0.0, 1.0, 0.0)));
    vec3 up = cross(rt, fw);
    // slight roll for drama
    float roll = 0.12;
    vec3 rt2 = rt * cos(roll) + up * sin(roll);
    vec3 up2 = up * cos(roll) - rt * sin(roll);
    vec3 rd = normalize(fw * 1.5 + uv.x * rt2 + uv.y * up2);

    vec3 pos = ro;
    vec3 vel = rd;
    float h2 = dot(cross(pos, vel), cross(pos, vel));

    vec3 col = vec3(0.0);
    float trans = 1.0;          // remaining transmittance
    vec3 glow = vec3(0.0);
    bool captured = false;
    float escapeR = max(dist * 1.5, 60.0);

    for (int i = 0; i < MAX_STEPS; i++) {
        float r = length(pos);
        if (r < 1.0) { captured = true; break; }
        if (r > escapeR && dot(pos, vel) > 0.0) break;

        // adaptive step: smaller near the hole and near the disk plane
        float dt = clamp(0.06 * r, 0.015, 2.5);
        dt = min(dt, max(abs(pos.y) * 0.6, 0.04 * r));

        // leapfrog (kick-drift-kick) keeps orbits around the photon sphere stable
        vec3 acc = -1.5 * h2 * pos / pow(r, 5.0);
        vel += acc * dt * 0.5;
        vec3 prev = pos;
        pos += vel * dt;
        float r2 = length(pos);
        acc = -1.5 * h2 * pos / pow(r2, 5.0);
        vel += acc * dt * 0.5;

        // volumetric haze hugging the disk (lensed along with everything else)
        float rc = length(pos.xz);
        float thick = 0.08 + rc * 0.035;
        float haze = exp(-pos.y * pos.y / (thick * thick))
                   * smoothstep(DISK_IN * 0.7, DISK_IN * 1.6, rc)
                   * (1.0 - smoothstep(DISK_IN * 1.5, DISK_OUT * 1.1, rc));
        glow += trans * haze * dt * blackbody(T_DISK * diskTempProfile(max(rc, DISK_IN)) * 0.95)
              * pow(diskTempProfile(max(rc, DISK_IN)), 2.0) * 0.05;

        // disk plane crossing
        if (prev.y * pos.y < 0.0) {
            float t = prev.y / (prev.y - pos.y);
            vec3 hit = mix(prev, pos, t);
            vec4 d = shadeDisk(hit, normalize(vel));
            col += trans * d.rgb * d.a;
            trans *= 1.0 - d.a;
            if (trans < 0.01) break;
        }
    }

    col += glow;
    if (!captured && trans > 0.01)
        col += trans * sky(normalize(vel));

    // exposure, tonemap, grade
    col *= 1.15;
    col = aces(col);
    col = pow(col, vec3(1.0 / 2.2));

    // vignette + subtle grain
    vec2 q = frag / uResolution;
    col *= 0.55 + 0.45 * pow(16.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y), 0.2);
    col += (hash13(vec3(frag, fract(uTime) * 100.0)) - 0.5) / 255.0;

    fragColor = vec4(col, 1.0);
}
