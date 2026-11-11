// nvt-game --gfx: the same check (and with mode 2, rewrite) as nvt-game.comp,
// done by fragments of a full-screen draw instead of a compute dispatch, so
// the work is graphics work on GR (what Chrome or a game submits). Graphics
// preemption is coarser than compute preemption, which matters when the
// driver stops a process to move its memory.
#version 450
layout(early_fragment_tests) in;
layout(location = 0) out vec4 color;

layout(std430, set = 0, binding = 0) buffer Data { uvec4 d[]; };
layout(std430, set = 0, binding = 1) buffer Result { uint errors; uint first_bad; uint got_x; uint want_x; };

layout(push_constant) uniform PC {
    uint seed;
    uint base;
    uint count;
    uint mode;     // 1 verify, 2 verify + write next generation
    uint gen_old;
    uint gen_new;
    uint width;    // render target size: fragment k handles elements k, k + w*h, ...
    uint height;
} pc;

uint hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void main()
{
    uint k = uint(gl_FragCoord.y) * pc.width + uint(gl_FragCoord.x);
    uint stride = pc.width * pc.height;
    uint acc = 0u;
    for (uint off = k; off < pc.count; off += stride) {
        uint i = pc.base + off;
        uint h = hash(i ^ pc.seed ^ (pc.gen_old * 0x9e3779b9u));
        uvec4 want = uvec4(h, h ^ 0x55555555u, i, pc.gen_old);
        uvec4 got = d[i];
        acc ^= got.x;
        if (got != want) {
            if (atomicAdd(errors, 1u) == 0u) {
                first_bad = i;
                got_x = got.w;
                want_x = want.w;
            }
        }
        if (pc.mode == 2u) {
            uint hn = hash(i ^ pc.seed ^ (pc.gen_new * 0x9e3779b9u));
            d[i] = uvec4(hn, hn ^ 0x55555555u, i, pc.gen_new);
        }
    }
    color = unpackUnorm4x8(acc);
}
