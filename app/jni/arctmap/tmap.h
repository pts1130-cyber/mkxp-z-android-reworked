// Tera body material map builder (port of build.py make_map). C++17, no dependencies.
// Output: (h*k) x (w*k) RGBA8. R = facet level, G,B = facet normal xy, A = blend weight.
#pragma once
#include <cstdint>
#include <string>

namespace tmap {

struct Params {
  // values mirror build.py P.get(...) defaults; NaN-free
  bool hash_mix = false;
  bool tri_check = false; float tri_lo = 0.5f, tri_hi = 2.0f;
  int hole_na = 12, hole_nb = 3;
  double shade_w = 0.9, tilt = 0.45, smooth_n = 0.0;
  bool broad_fast = false;        // float32 sums for the body-scale blur (not bit-exact, ~2x faster)
  double broad = 0.0, broad_gain = 0.8, broad_tilt = 0.15, facet_n = 0.0;
  double thick_blur = 2.0, lx = -0.6, ly = -0.7;
  double facet = 0.0, facet_pow = 2.5, lam = 0.25;
  double base = 0.0, thick = 0.0, thick_pow = 1.4, thick_facet = 0.7;
  double accent = 0.0, accent_v = 0.0;
  double white_frac = 0.0, white_lev = 1.0, white_comp = 0.0;
  double keep_small = 0.0, keep_w = 0.1;
  double dark_frac = 0.0, dark_lev = 0.12, dark_w = 1.0, thin = 0.0;   // thin: no dark facet within thin*k px of the outline
  double keep_upper = 0.0;         // > 0: keep_small spots only in the top keep_upper of the body rows
  double edge = 0.0, edge_frac = 0.4;
  double rim = 0.0, rim_w = 0.0, rim_min = 0.35;
  double lum_base = 0.0, lum_ratio = 0.0;
  double outline_comp = 0.0, outline_rim = 0.0, line_floor = -9.0;
  double inner_comp = 0.0, inner_lum = 0.22;
  double keep_sat = 0.0;
  int keep_main = 1;               // 1 = True, 0 = False, 2 = 'area'
  bool has_main_hf = false; double main_h = 0.0, main_frac = 0.0; bool main_none = false;
  double main_frac0 = 0.55, main_frac_w = 0.13;
  bool has_a_lo = false; double a_lo = 0.0, a_pow = 1.0;
};

// parse "key v1 v2 ..." lines (export_mesh.py .par format)
bool parse_params(const std::string& text, Params& P);

struct Timing { double fill = 0, normals = 0, holes = 0, stats = 0, broad = 0, thick = 0, colour = 0, rim = 0, out = 0; };

// rgba: one frame, w x h, 1x. p0: frame-0 vertices (nv x 2, k-scale px). tri: nt x 3.
// v: this frame's vertices (nv x 2). out: (h*k)*(w*k)*4 bytes. ids_out (optional): (h*k)*(w*k).
// tri_ok (optional): bit i = triangle i passes the area check for this frame (precomputed from float positions,
// so positions may be stored at 1/16 px). When null the check runs here on p0/v.
// returns 0 on success.
int build(const uint8_t* rgba, int w, int h, int k, int nv, const float* p0, int nt, const int32_t* tri,
          const float* v, uint32_t sid, const Params& P, uint8_t* out, int32_t* ids_out = nullptr, Timing* tm = nullptr,
          const uint8_t* tri_ok = nullptr);

// helpers exposed for checks
void resize_linear_f32(const float* src, int w, int h, int k, float* dst);
void gauss_f32(float* a, int w, int h, double sigma);
void gauss_f64(double* a, int w, int h, double sigma);
void edt(const uint8_t* m, int w, int h, double* out);
void fill_tris(const float* v, int nt, const int32_t* tri, const uint8_t* ok, int W, int H, int32_t* ids);
void rgb2hsv8(const uint8_t* rgb, int stride, int n, uint8_t* hsv);
double hmix(uint32_t a, uint32_t b, uint32_t c);

}  // namespace tmap

#ifdef _WIN32
#define TMAP_EXPORT __declspec(dllexport)
#else
#define TMAP_EXPORT __attribute__((visibility("default")))
#endif
#define TMAP_VERSION 3

// C ABI (MiniFFI: integers and pointers only, <= 10 args; returns are numbers)
// ip = int32[8] {buf_w, h, frame_w, k, sid, frame, src_x, flags}: rgba = buf_w x h pixels holding the frame at x = src_x
// (src_x < 0: frame * frame_w), flags bit0 = rgba is BGRA (Bitmap#raw_data order); tmv = .tmv with zlib part inflated;
// par = params text; out = (h*k)*(frame_w*k)*4 bytes. Errors are positive codes (11..19), 0 = ok.
extern "C" {
TMAP_EXPORT int tmap_version(void);  // TMAP_VERSION
TMAP_EXPORT int tmap_frame(const int32_t* ip, const uint8_t* rgba, const uint8_t* tmv, const char* par, uint8_t* out);
TMAP_EXPORT intptr_t tmap_job_new(const int32_t* ip, const uint8_t* rgba, const uint8_t* tmv, const char* par);  // 0 = bad input
TMAP_EXPORT int tmap_job_step(intptr_t job, int budget_us);   // 1 done, 0 more work, >= 11 error
TMAP_EXPORT int tmap_job_take(intptr_t job, uint8_t* out);    // after step returned 1
TMAP_EXPORT void tmap_job_free(intptr_t job);
// crc32 of the pixels in RGBA order with fully transparent pixels as 0 (picture key); ip = int32[2] {npx, flags}, io = uint32 seed in / crc out
TMAP_EXPORT int tmap_hash(const uint8_t* rgba, const int32_t* ip, uint32_t* io);
// v3: vertex tracks (.tmv) for a picture the pack does not have, worked out on background threads.
// ip = int32[8] {frame_w, frame_h, nf, k, flags (bit0 BGRA), threads (0 = cores-1, at most 3), 0, 0};
// rgba = the nf frames one after another (copied). Handles are small positive ints (0 = refused).
TMAP_EXPORT int tmap_batch_new(const int32_t* ip, const uint8_t* rgba);
TMAP_EXPORT int tmap_batch_poll(int batch, int32_t* io);   // 0 running, 1 done, >= 11 error; io int32[4] {done, nf, bytes, state}
TMAP_EXPORT int tmap_batch_take(int batch, uint8_t* out);  // .tmv, zlib part not applied (io[2] bytes)
TMAP_EXPORT int tmap_batch_free(int batch);                // cancels if still running
}
