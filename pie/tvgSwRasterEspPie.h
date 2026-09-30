/*
 * ThorVG CPU raster functions accelerated with the Espressif PIE v2 SIMD extension.
 *
 * This is the PIE counterpart of thorvg/src/renderer/cpu_engine/tvgSwRasterNeon.h.
 * It lives outside the ThorVG tree and is included by tvgSwRaster.cpp when
 * THORVG_ESP_PIE_V2_SUPPORT is defined (see this component's CMakeLists.txt).
 * ESP-IDF's -march (..._xespv for PIE v2.2) reaches ThorVG's Meson build through
 * the @cxxflags response file, so the PIE instructions below assemble as-is.
 *
 * Accelerated so far (ARGB8888 only, everything else falls back to plain C):
 *   - rasterPixel32()        solid fill of a row        -> espPieFill32()
 *   - _rasterTranslucentRect translucent rectangle      -> espPieBlend32()
 *   - _rasterTranslucentRle  translucent anti-aliased shape spans -> espPieBlend32()
 *
 * Each row/span is split into three parts:
 *   head: single pixels until dst is 16-byte aligned (PIE 128-bit loads/stores
 *         need 16-byte aligned addresses unless CFG allows misaligned access,
 *         so we simply never give them a misaligned address)
 *   body: groups of 4 pixels (one 128-bit register), done with PIE inline asm
 *   tail: the 0..3 pixels left over, done in C
 *
 * ---------------------------------------------------------------------------
 * PIE quick reference (only what this file uses)
 * ---------------------------------------------------------------------------
 *   q0..q7            128-bit vector registers. One q register = 4 ARGB8888 pixels,
 *                     or 8 x 16-bit lanes, or 16 x 8-bit lanes.
 *   SAR               shift amount used by esp.vmul.*, esp.vsr.*, esp.vsl.*
 *   CFG[7:4]          rounding mode for those right shifts (0 = FLOOR)
 *
 *   esp.movi.32.q  qd, rs, n    qd lane n (32-bit, n = 0..3) = rs
 *   esp.vld.128.ip qd, rs, imm  qd = 16 bytes at rs, then rs += imm
 *   esp.vst.128.ip qs, rs, imm  16 bytes at rs = qs, then rs += imm
 *   esp.andq / esp.orq          128-bit bitwise AND / OR
 *   esp.vsr.u32    qd, qs       4 x 32-bit logical shift right by SAR
 *   esp.vsl.32     qd, qs       4 x 32-bit shift left by SAR
 *   esp.vmul.u16   qd, qa, qb   8 x 16-bit unsigned (qa * qb) >> SAR, keep low 16 bits
 *   esp.vadd.u8    qd, qa, qb   16 x 8-bit unsigned add (saturating)
 *   esp.movx.r.cfg / esp.movx.w.cfg / esp.movx.w.sar   read/write CFG, write SAR
 *
 * ---------------------------------------------------------------------------
 * Rules for the inline asm below
 * ---------------------------------------------------------------------------
 *   1. PIE instructions only accept a0-a5, s0-s1, s8-s11 and t3-t6 as scalar
 *      operands. A plain "r" constraint could give us any register (e.g. a6 or
 *      t0), so every operand is pinned to a fixed register with
 *      `register T name asm("aN")`. Scratch registers are listed as clobbers.
 *   2. GCC does not know the q registers (it rejects "q0" as a clobber), and it
 *      never generates PIE code on its own. So each kernel is ONE asm block that
 *      sets up every q register it reads, and never expects q values to survive
 *      from one asm block to the next.
 *   3. "memory" clobber: the asm reads and writes the pixel buffer behind GCC's back.
 *   4. PIE is a coprocessor with lazy context switching: the first PIE instruction
 *      in a task pins it to the current core. On ESP32-S31 only core 1 has PIE,
 *      so render from a task pinned to core 1. Never use it from an ISR or inside
 *      a critical section.
 */

#ifdef THORVG_ESP_PIE_V2_SUPPORT

/************************************************************************/
/* PIE kernels                                                          */
/************************************************************************/

/*
 * Write val to 4 * blocks pixels.
 * dst must be 16-byte aligned and blocks must be > 0.
 */
static void espPieFill32(uint32_t* dst, uint32_t val, uint32_t blocks)
{
    register uint32_t* a0 asm("a0") = dst;     //destination pointer, advanced by the asm
    register uint32_t a1 asm("a1") = val;      //fill colour
    register uint32_t a2 asm("a2") = blocks;   //loop counter, counts down to 0

    asm volatile(
        //q0 = val in all 4 lanes, i.e. 4 pixels of the fill colour
        "esp.movi.32.q   q0, %[val], 0              \n"
        "esp.movi.32.q   q0, %[val], 1              \n"
        "esp.movi.32.q   q0, %[val], 2              \n"
        "esp.movi.32.q   q0, %[val], 3              \n"

        "1:                                         \n"
        "esp.vst.128.ip  q0, %[dst], 16             \n"  //store 4 pixels, dst += 16 bytes
        "addi            %[blocks], %[blocks], -1   \n"  //one group of 4 pixels done
        "bnez            %[blocks], 1b              \n"  //loop until blocks == 0

        : [dst] "+r"(a0), [blocks] "+r"(a2)             //both are modified by the asm
        : [val] "r"(a1)
        : "memory"                                      //we wrote to *dst
    );
}

/*
 * For 4 * blocks pixels:  dst[i] = src + ALPHA_BLEND(dst[i], ialpha)
 * dst must be 16-byte aligned and blocks must be > 0.
 * src is a premultiplied ARGB colour, ialpha is 0..255.
 *
 * ThorVG's ALPHA_BLEND() (tvgSwCommon.h) computes, per 8-bit channel:
 *     (channel * (ialpha + 1)) >> 8
 * ialpha + 1 can be 256, which does not fit an 8-bit multiplier, so we cannot
 * just use esp.vmul.u8. Instead, like the C code, each pixel is split in two
 * with the 0x00ff00ff mask:
 *
 *     pixel                    = [ A  | R  | G  | B  ]   (bytes 3..0)
 *     low  = pixel & 0x00ff00ff        = [ 00 | R  | 00 | B  ]
 *     high = (pixel >> 8) & 0x00ff00ff = [ 00 | A  | 00 | G  ]
 *
 * Now each channel sits alone in a 16-bit lane with 8 zero bits above it, so
 * a 16-bit multiply by (ialpha + 1) cannot overflow (255 * 256 = 65280), and
 * ">> 8" brings the result back to 0..255 in the low byte of each lane.
 * Then:  result = low | (high << 8)
 *
 * Why the final byte add cannot overflow: src is premultiplied, so every src
 * channel is <= src alpha, and the blended dst channel is at most
 * 255 * (256 - alpha) >> 8 <= 255 - alpha. Their sum is <= 255. So the
 * saturating esp.vadd.u8 gives the same result as the plain 32-bit add in C.
 */
static void espPieBlend32(uint32_t* dst, uint32_t src, uint32_t ialpha, uint32_t blocks)
{
    register uint32_t* a0 asm("a0") = dst;                          //destination pointer, advanced by the asm
    register uint32_t a1 asm("a1") = src;                           //source colour
    register uint32_t a2 asm("a2") = (ialpha + 1) * 0x00010001u;    //(ialpha + 1) in both 16-bit halves
    register uint32_t a3 asm("a3") = blocks;                        //loop counter, counts down to 0
    register uint32_t a4 asm("a4") = 0x00ff00ffu;                   //channel mask

    asm volatile(
        //Rounding mode FLOOR (CFG bits 7:4 = 0) so every ">> 8" truncates exactly
        //like the C code. The caller's CFG is kept in a5 and restored at the end;
        //the other CFG bits (e.g. misaligned access enables) are left untouched.
        "esp.movx.r.cfg  a5                         \n"  //a5 = old CFG
        "andi            t3, a5, ~0xf0              \n"  //t3 = old CFG with rounding bits cleared
        "esp.movx.w.cfg  t3                         \n"  //CFG = t3

        //SAR = 8: the shift used by esp.vmul.u16 (>> 8), esp.vsr.u32 (>> 8) and esp.vsl.32 (<< 8)
        "li              t3, 8                      \n"
        "esp.movx.w.sar  t3                         \n"

        //q5 = src in all 4 lanes
        "esp.movi.32.q   q5, %[src], 0              \n"
        "esp.movi.32.q   q5, %[src], 1              \n"
        "esp.movi.32.q   q5, %[src], 2              \n"
        "esp.movi.32.q   q5, %[src], 3              \n"

        //q6 = (ialpha + 1) in all 8 16-bit lanes
        "esp.movi.32.q   q6, %[ia], 0               \n"
        "esp.movi.32.q   q6, %[ia], 1               \n"
        "esp.movi.32.q   q6, %[ia], 2               \n"
        "esp.movi.32.q   q6, %[ia], 3               \n"

        //q7 = 0x00ff00ff in all 4 lanes
        "esp.movi.32.q   q7, %[mask], 0             \n"
        "esp.movi.32.q   q7, %[mask], 1             \n"
        "esp.movi.32.q   q7, %[mask], 2             \n"
        "esp.movi.32.q   q7, %[mask], 3             \n"

        "1:                                         \n"
        "esp.vld.128.ip  q0, %[dst], 0              \n"  //q0 = 4 dst pixels (dst not advanced yet)
        "esp.andq        q1, q0, q7                 \n"  //q1 = [00|R|00|B] per pixel
        "esp.vsr.u32     q2, q0                     \n"  //q2 = pixel >> 8 = [00|A|R|G]
        "esp.andq        q2, q2, q7                 \n"  //q2 = [00|A|00|G]
        "esp.vmul.u16    q1, q1, q6                 \n"  //q1 = (R, B) * (ialpha + 1) >> 8
        "esp.vmul.u16    q2, q2, q6                 \n"  //q2 = (A, G) * (ialpha + 1) >> 8
        "esp.vsl.32      q2, q2                     \n"  //q2 = [A|00|G|00], back in place
        "esp.orq         q0, q1, q2                 \n"  //q0 = ALPHA_BLEND(dst, ialpha)
        "esp.vadd.u8     q0, q0, q5                 \n"  //q0 = src + ALPHA_BLEND(dst, ialpha)
        "esp.vst.128.ip  q0, %[dst], 16             \n"  //store 4 pixels, dst += 16 bytes
        "addi            %[blocks], %[blocks], -1   \n"  //one group of 4 pixels done
        "bnez            %[blocks], 1b              \n"  //loop until blocks == 0

        "esp.movx.w.cfg  a5                         \n"  //restore the caller's rounding mode

        : [dst] "+r"(a0), [blocks] "+r"(a3)             //both are modified by the asm
        : [src] "r"(a1), [ia] "r"(a2), [mask] "r"(a4)
        : "memory", "a5", "t3"                          //we wrote to *dst and used a5/t3 as scratch
    );
}

/************************************************************************/
/* Row helpers: head (C) + body (PIE) + tail (C)                        */
/************************************************************************/

//Number of single pixels to process before dst is 16-byte aligned.
//ThorVG pixel buffers are uint32_t, so dst is always at least 4-byte aligned.
static inline uint32_t espPieHeadLength(const uint32_t* dst, uint32_t len)
{
    auto head = ((16 - (reinterpret_cast<uintptr_t>(dst) & 15)) & 15) / sizeof(uint32_t);
    return (head < len) ? head : len;
}

//dst[i] = src + ALPHA_BLEND(dst[i], ialpha) for len pixels
static void espPieBlendSpan(uint32_t* dst, uint32_t src, uint32_t ialpha, uint32_t len)
{
    auto head = espPieHeadLength(dst, len);
    for (uint32_t i = 0; i < head; ++i, ++dst) *dst = src + ALPHA_BLEND(*dst, ialpha);
    len -= head;

    auto blocks = len / 4;
    if (blocks > 0) {
        espPieBlend32(dst, src, ialpha, blocks);
        dst += blocks * 4;
        len -= blocks * 4;
    }

    for (uint32_t i = 0; i < len; ++i, ++dst) *dst = src + ALPHA_BLEND(*dst, ialpha);
}

/************************************************************************/
/* Functions called from tvgSwRaster.cpp                                */
/************************************************************************/

static void espPieRasterPixel32(uint32_t* dst, uint32_t val, uint32_t offset, int32_t len)
{
    if (len <= 0) return;
    dst += offset;

    auto count = static_cast<uint32_t>(len);
    auto head = espPieHeadLength(dst, count);
    for (uint32_t i = 0; i < head; ++i) *dst++ = val;
    count -= head;

    auto blocks = count / 4;
    if (blocks > 0) {
        espPieFill32(dst, val, blocks);
        dst += blocks * 4;
        count -= blocks * 4;
    }

    while (count--) *dst++ = val;
}

static bool espPieRasterTranslucentRect(SwSurface* surface, const RenderRegion& bbox, const RenderColor& c)
{
    //8-bit grayscale surfaces (masks) stay on the C path
    if (surface->channelSize != sizeof(uint32_t)) return cRasterTranslucentRect(surface, bbox, c);

    auto color = surface->join(c.r, c.g, c.b, c.a);
    auto buffer = surface->buf32 + (bbox.min.y * surface->stride) + bbox.min.x;
    auto ialpha = 255 - c.a;

    for (uint32_t y = 0; y < bbox.h(); ++y) {
        espPieBlendSpan(&buffer[y * surface->stride], color, ialpha, bbox.w());
    }
    return true;
}

static bool espPieRasterTranslucentRle(SwSurface* surface, const SwRle* rle, const RenderRegion& bbox, const RenderColor& c)
{
    //8-bit grayscale surfaces (masks) stay on the C path
    if (surface->channelSize != sizeof(uint32_t)) return cRasterTranslucentRle(surface, rle, bbox, c);

    const SwSpan* end;
    int32_t x, len;
    auto color = surface->join(c.r, c.g, c.b, c.a);

    for (auto span = rle->fetch(bbox, &end); span < end; ++span) {
        if (!span->fetch(bbox, x, len)) continue;
        //anti-aliased edge: scale the colour by the span coverage first, same as the C version
        auto src = (span->coverage < 255) ? ALPHA_BLEND(color, span->coverage) : color;
        espPieBlendSpan(&surface->buf32[span->y * surface->stride + x], src, IA(src), len);
    }
    return true;
}

#endif
