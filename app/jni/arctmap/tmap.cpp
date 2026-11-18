// Tera body material map builder (port of build.py make_map). C++17, no dependencies.
// Build with -ffp-contract=off so float32/float64 steps round like numpy (matters on arm64, which fuses by default).
#include "tmap.h"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <functional>
#include <sstream>
#include <vector>

namespace tmap {

static const double PI = 3.14159265358979323846;
// stage timers use this thread's CPU time where available (load from other processes does not count)
#if defined(__linux__) || defined(__ANDROID__)
#include <time.h>
struct clk {
  using time_point = double;
  static double now() { timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6; }
};
static inline double ms_since(double t) { return clk::now() - t; }
#else
using clk = std::chrono::steady_clock;
static inline double ms_since(clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); }
#endif

// ---------------------------------------------------------------- params
bool parse_params(const std::string& text, Params& P) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string key; ls >> key;
    if (key.empty()) continue;
    std::vector<double> v; double x;
    while (ls >> x) v.push_back(x);
    if (v.empty()) {  // "nan" is not read by >>; treat as main_hf None
      if (key == "main_hf") { P.has_main_hf = true; P.main_none = true; }
      continue;
    }
    double a = v[0];
#define F(name) else if (key == #name) P.name = a;
    if (key == "hash") P.hash_mix = a == 1.0;
    else if (key == "tri_check") { P.tri_check = true; P.tri_lo = (float)v[0]; P.tri_hi = (float)(v.size() > 1 ? v[1] : 2.0); }
    else if (key == "hole_poster") { P.hole_na = (int)v[0]; P.hole_nb = (int)v[1]; }
    else if (key == "keep_main") P.keep_main = (int)a;
    else if (key == "main_hf") { P.has_main_hf = true; P.main_h = v[0]; P.main_frac = v.size() > 1 ? v[1] : 0; P.main_none = std::isnan(v[0]); }
    else if (key == "a_lo") { P.has_a_lo = true; P.a_lo = a; }
    F(shade_w) F(tilt) F(smooth_n) F(broad) F(broad_gain) F(broad_tilt) F(facet_n) F(thick_blur) F(lx) F(ly)
    F(facet) F(facet_pow) F(lam) F(base) F(thick) F(thick_pow) F(thick_facet) F(accent) F(accent_v)
    F(white_frac) F(white_lev) F(white_comp) F(keep_small) F(keep_w) F(dark_frac) F(dark_lev) F(dark_w) F(thin) F(keep_upper)
    F(edge) F(edge_frac) F(rim) F(rim_w) F(rim_min) F(lum_base) F(lum_ratio) F(outline_comp) F(outline_rim)
    F(line_floor) F(inner_comp) F(inner_lum) F(keep_sat) F(main_frac0) F(main_frac_w) F(a_pow)
    else if (key == "broad_fast") P.broad_fast = a != 0;
#undef F
  }
  return true;
}

// ---------------------------------------------------------------- hashes (build.py hmix / hsh)
double hmix(uint32_t a, uint32_t b, uint32_t c) {
  uint64_t x = 0x9E3779B97F4A7C15ULL;
  uint32_t vs[3] = {a, b, c};
  for (uint32_t t : vs) {
    x ^= (uint64_t)t;
    x += 0x9E3779B97F4A7C15ULL;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    x = z ^ (z >> 31);
  }
  return (double)(x >> 11) / 9007199254740992.0;
}
static double hsh(uint32_t a, uint32_t b, uint32_t c) {
  uint64_t x = 1469598103934665603ULL;
  uint32_t vs[3] = {a, b, c};
  for (uint32_t t : vs) x = (x ^ (uint64_t)t) * 1099511628211ULL;
  return (double)(x >> 11) / 9007199254740992.0;
}

// ---------------------------------------------------------------- numpy helpers
// numpy pairwise summation (float32 / float64)
template <class T> static T pairwise(const T* a, size_t n) {
  if (n < 8) { T r = (T)-0.0; for (size_t i = 0; i < n; i++) r += a[i]; return r; }
  if (n <= 128) {
    T r[8]; for (int j = 0; j < 8; j++) r[j] = a[j];
    size_t i = 8;
    for (; i < n - (n % 8); i += 8) for (int j = 0; j < 8; j++) r[j] += a[i + j];
    T res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; i++) res += a[i];
    return res;
  }
  size_t n2 = n / 2; n2 -= n2 % 8;
  return pairwise(a, n2) + pairwise(a + n2, n - n2);
}
static inline float pymodf(float a, float b) {  // numpy float remainder
  float m = std::fmod(a, b);
  if (m != 0.0f) { if ((b < 0) != (m < 0)) m += b; } else m = std::copysign(0.0f, b);
  return m;
}
static inline double pymod(double a, double b) {
  double m = std::fmod(a, b);
  if (m != 0.0) { if ((b < 0) != (m < 0)) m += b; } else m = std::copysign(0.0, b);
  return m;
}
template <class T> static inline T clip(T x, T lo, T hi) { return x < lo ? lo : (x > hi ? hi : x); }

// np.gradient over a w x h image, returns gy (axis 0) and gx (axis 1)
template <class T> static void gradient(const T* f, int w, int h, T* gy, T* gx) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      size_t i = (size_t)y * w + x;
      if (h < 2) gy[i] = 0;
      else if (y == 0) gy[i] = (f[i + w] - f[i]) / (T)1.0;
      else if (y == h - 1) gy[i] = (f[i] - f[i - w]) / (T)1.0;
      else gy[i] = (f[i + w] - f[i - w]) / (T)2.0;
      if (w < 2) gx[i] = 0;
      else if (x == 0) gx[i] = (f[i + 1] - f[i]) / (T)1.0;
      else if (x == w - 1) gx[i] = (f[i] - f[i - 1]) / (T)1.0;
      else gx[i] = (f[i + 1] - f[i - 1]) / (T)2.0;
    }
}

// ---------------------------------------------------------------- cv2.resize INTER_LINEAR, float32, integer upscale
void resize_linear_f32(const float* src, int w, int h, int k, float* dst) {
  int W = w * k, H = h * k;
  double scale = 1.0 / (double)k;
  std::vector<int> xo(W), yo(H);
  std::vector<float> xa(W), ya(H);
  auto coef = [&](int d, int n, int& s, float& a) {
    float f = (float)((d + 0.5) * scale - 0.5);
    s = (int)std::floor(f); f -= s;
    if (s < 0) { f = 0; s = 0; }
    if (s >= n - 1) { f = 0; s = n - 1; }
    a = f;
  };
  for (int x = 0; x < W; x++) coef(x, w, xo[x], xa[x]);
  for (int y = 0; y < H; y++) coef(y, h, yo[y], ya[y]);
  std::vector<float> rows((size_t)h * W);
  for (int y = 0; y < h; y++) {
    const float* S = src + (size_t)y * w; float* D = &rows[(size_t)y * W];
    for (int x = 0; x < W; x++) {
      int s = xo[x]; float a1 = xa[x], a0 = 1.f - a1;
      D[x] = (s + 1 < w) ? S[s] * a0 + S[s + 1] * a1 : S[s] * a0;
    }
  }
  for (int y = 0; y < H; y++) {
    int s0 = yo[y], s1 = std::min(s0 + 1, h - 1); float b1 = ya[y], b0 = 1.f - b1;
    const float* R0 = &rows[(size_t)s0 * W]; const float* R1 = &rows[(size_t)s1 * W];
    float* D = dst + (size_t)y * W;
    for (int x = 0; x < W; x++) D[x] = R0[x] * b0 + R1[x] * b1;
  }
}

// ---------------------------------------------------------------- scipy.ndimage.gaussian_filter (mode reflect, truncate 4)
static std::vector<double> gkernel(double sigma, int& r) {
  r = (int)(4.0 * sigma + 0.5);
  std::vector<double> phi(2 * r + 1);
  double s2 = sigma * sigma, c = -0.5 / s2;
  for (int i = -r; i <= r; i++) phi[i + r] = std::exp(c * (double)((long long)i * i));
  double s = pairwise(phi.data(), phi.size());
  for (auto& p : phi) p = p / s;
  return phi;
}
static inline int reflect(int i, int n) {
  if (n == 1) return 0;
  int p = 2 * n;
  i %= p; if (i < 0) i += p;
  return i < n ? i : p - 1 - i;
}
// scipy gaussian over a window [x0,x1) x [y0,y1), computable in row chunks. Exact: each pixel's sum order is scipy's
// (centre, then the pair at distance r .. 1); the loops run across pixels so they vectorise. Writes only the window of dst.
template <class T, class A = double> struct GaussJob {
  int w = 0, h = 0, r = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0, c0 = 0, c1 = 0, cw = 0;
  bool active = false;
  std::vector<A> k, src, acc, line, out;
  T* dst = nullptr;
  void init(T* a, int w_, int h_, double sigma, int wx0, int wx1, int wy0, int wy1) {
    w = w_; h = h_; dst = a;
    x0 = std::max(wx0, 0); y0 = std::max(wy0, 0); x1 = std::min(wx1, w); y1 = std::min(wy1, h);
    active = sigma > 1e-15 && x0 < x1 && y0 < y1;
    if (!active) return;
    std::vector<double> kd = gkernel(sigma, r);
    k.assign(kd.begin(), kd.end());
    c0 = x0 - r; c1 = x1 + r;
    if (c0 < 0 || c1 > w) { c0 = 0; c1 = w; }
    cw = c1 - c0;
    src.resize((size_t)cw * h);
    for (int y = 0; y < h; y++) for (int x = 0; x < cw; x++) src[(size_t)y * cw + x] = (A)a[(size_t)y * w + c0 + x];
    acc.resize(cw); line.resize(w + 2 * r); out.resize(w);
  }
  int chunk_rows() const {  // about 2e5 multiply-adds per chunk
    if (!active) return 1;
    long per_row = (long)(cw + (x1 - x0)) * (r + 1);
    return (int)std::max(1L, 200000L / std::max(per_row, 1L));
  }
  void rows(int ya, int yb) {
    if (!active) return;
    const A* fw = k.data() + r;
    ya = std::max(ya, y0); yb = std::min(yb, y1);
    for (int y = ya; y < yb; y++) {
      const A* c = &src[(size_t)y * cw];
      for (int x = 0; x < cw; x++) acc[x] = c[x] * fw[0];
      for (int j = -r; j < 0; j++) {
        const A* u = &src[(size_t)reflect(y + j, h) * cw];
        const A* d = &src[(size_t)reflect(y - j, h) * cw];
        const A f = fw[j];
        for (int x = 0; x < cw; x++) acc[x] += (u[x] + d[x]) * f;
      }
      A* L = line.data() + r;
      for (int x = c0; x < c1; x++) L[x] = (A)(T)acc[x - c0];  // scipy stores each axis in the array dtype
      if (c0 == 0 && c1 == w)
        for (int i = 1; i <= r; i++) { L[-i] = L[reflect(-i, w)]; L[w - 1 + i] = L[reflect(w - 1 + i, w)]; }
      for (int x = x0; x < x1; x++) out[x] = L[x] * fw[0];
      for (int j = -r; j < 0; j++) {
        const A f = fw[j];
        const A* u = L + j; const A* d = L - j;
        for (int x = x0; x < x1; x++) out[x] += (u[x] + d[x]) * f;
      }
      T* row = dst + (size_t)y * w;
      for (int x = x0; x < x1; x++) row[x] = (T)out[x];
    }
  }
  void release() { std::vector<A>().swap(src); std::vector<A>().swap(acc); std::vector<A>().swap(line); std::vector<A>().swap(out); }
};
template <class T, class A = double> static void gauss_win(T* a, int w, int h, double sigma, int x0, int x1, int y0, int y1) {
  GaussJob<T, A> g; g.init(a, w, h, sigma, x0, x1, y0, y1); g.rows(0, h);
}
template <class T> static void gauss_impl(T* a, int w, int h, double sigma) { gauss_win(a, w, h, sigma, 0, w, 0, h); }
void gauss_f32(float* a, int w, int h, double s) { gauss_impl(a, w, h, s); }
void gauss_f64(double* a, int w, int h, double s) { gauss_impl(a, w, h, s); }

// ---------------------------------------------------------------- exact Euclidean distance to nearest 0 pixel
static void edt1d(const double* f, int n, double* d, int* v, double* z) {
  int k = 0; v[0] = 0; z[0] = -1e300; z[1] = 1e300;
  for (int q = 1; q < n; q++) {
    if (f[q] >= 1e299) continue;
    if (f[v[0]] >= 1e299) { v[0] = q; continue; }
    double s;
    while (true) {
      s = ((f[q] + (double)q * q) - (f[v[k]] + (double)v[k] * v[k])) / (2.0 * q - 2.0 * v[k]);
      if (s <= z[k] && k > 0) k--; else break;
    }
    if (s <= z[k]) { v[k] = q; z[k + 1] = 1e300; continue; }  // k == 0 case
    k++; v[k] = q; z[k] = s; z[k + 1] = 1e300;
  }
  if (f[v[0]] >= 1e299) { for (int q = 0; q < n; q++) d[q] = 1e300; return; }
  k = 0;
  for (int q = 0; q < n; q++) {
    while (z[k + 1] < q) k++;
    double dq = (double)(q - v[k]);
    d[q] = dq * dq + f[v[k]];
  }
}
// column pass and row pass can run in chunks (EdtJob); edt() runs both at once
struct EdtJob {
  const uint8_t* m = nullptr; int w = 0, h = 0; double* out = nullptr;
  std::vector<double> g, f, d, z; std::vector<int> v;
  void init(const uint8_t* m_, int w_, int h_, double* out_) {
    m = m_; w = w_; h = h_; out = out_;
    int n = std::max(w, h);
    g.assign((size_t)w * h, 0.0); f.resize(n); d.resize(n); z.resize(n + 1); v.resize(n);
  }
  void cols(int x0, int x1) {
    for (int x = std::max(x0, 0); x < std::min(x1, w); x++) {
      for (int y = 0; y < h; y++) f[y] = m[(size_t)y * w + x] ? 1e300 : 0.0;
      edt1d(f.data(), h, d.data(), v.data(), z.data());
      for (int y = 0; y < h; y++) g[(size_t)y * w + x] = d[y];
    }
  }
  void rows(int y0, int y1) {
    for (int y = std::max(y0, 0); y < std::min(y1, h); y++) {
      edt1d(&g[(size_t)y * w], w, d.data(), v.data(), z.data());
      for (int x = 0; x < w; x++) out[(size_t)y * w + x] = m[(size_t)y * w + x] ? std::sqrt(d[x]) : 0.0;
    }
  }
  void release() { std::vector<double>().swap(g); }
};
void edt(const uint8_t* m, int w, int h, double* out) {
  EdtJob e; e.init(m, w, h, out); e.cols(0, w); e.rows(0, h);
}

// ---------------------------------------------------------------- cv2.fillPoly (LINE_8, shift 4), one triangle at a time
namespace cvr {
static const int XY_SHIFT = 16; static const int64_t XY_ONE = 1LL << XY_SHIFT;
struct P2 { int64_t x, y; };
static bool clipLine(int64_t W, int64_t H, P2& p1, P2& p2) {
  int64_t right = W - 1, bottom = H - 1;
  int64_t &x1 = p1.x, &y1 = p1.y, &x2 = p2.x, &y2 = p2.y;
  int c1 = (x1 < 0) + (x1 > right) * 2 + (y1 < 0) * 4 + (y1 > bottom) * 8;
  int c2 = (x2 < 0) + (x2 > right) * 2 + (y2 < 0) * 4 + (y2 > bottom) * 8;
  if ((c1 & c2) == 0 && (c1 | c2) != 0) {
    int64_t a;
    if (c1 & 12) {
      a = c1 < 8 ? 0 : bottom;
      x1 += (int64_t)((double)(a - y1) * (x2 - x1) / (y2 - y1));
      y1 = a; c1 = (x1 < 0) + (x1 > right) * 2;
    }
    if (c2 & 12) {
      a = c2 < 8 ? 0 : bottom;
      x2 += (int64_t)((double)(a - y2) * (x2 - x1) / (y2 - y1));
      y2 = a; c2 = (x2 < 0) + (x2 > right) * 2;
    }
    if ((c1 & c2) == 0 && (c1 | c2) != 0) {
      if (c1) {
        a = c1 == 1 ? 0 : right;
        y1 += (int64_t)((double)(a - x1) * (y2 - y1) / (x2 - x1));
        x1 = a; c1 = 0;
      }
      if (c2) {
        a = c2 == 1 ? 0 : right;
        y2 += (int64_t)((double)(a - x2) * (y2 - y1) / (x2 - x1));
        x2 = a; c2 = 0;
      }
    }
  }
  return (c1 | c2) == 0;
}
static void line8(int32_t* img, int W, int H, P2 a, P2 b, int32_t col) {
  if ((uint64_t)a.x >= (uint64_t)W || (uint64_t)b.x >= (uint64_t)W || (uint64_t)a.y >= (uint64_t)H || (uint64_t)b.y >= (uint64_t)H)
    if (!clipLine(W, H, a, b)) return;
  int64_t dx = b.x - a.x, dy = b.y - a.y;
  if (dx < 0) { dx = -dx; dy = -dy; std::swap(a, b); }
  int64_t sx = 1, sy = 1;
  if (dy < 0) { dy = -dy; sy = -1; }
  bool vert = dy > dx;
  if (vert) { std::swap(dx, dy); std::swap(sx, sy); }
  int64_t err = dx - (dy + dy), plusDelta = dx + dx, minusDelta = -(dy + dy);
  // non-vert: always step x by sx(minus), step y by sy when err < 0 (plus); vert: roles swapped
  int64_t mx = vert ? 0 : sx, my = vert ? sx : 0, px = vert ? sy : 0, py = vert ? 0 : sy;
  int64_t x = a.x, y = a.y, count = dx + 1;
  for (int64_t i = 0; i < count; i++) {
    img[(size_t)y * W + x] = col;
    int64_t mask = err < 0 ? -1 : 0;
    err += minusDelta + (plusDelta & mask);
    x += mx + (px & mask); y += my + (py & mask);
  }
}
struct Edge { int64_t x, dx; int y0, y1; };
static void fill_poly(int32_t* img, int W, int H, const int64_t* px, const int64_t* py, int n, int32_t col) {
  const int shift = 4; int64_t delta = (1 << shift) >> 1;
  Edge E[8]; int ne = 0;
  P2 p0{px[n - 1] << (XY_SHIFT - shift), (py[n - 1] + delta) >> shift};
  for (int i = 0; i < n; i++) {
    P2 p1{px[i] << (XY_SHIFT - shift), (py[i] + delta) >> shift};
    P2 p0c = p0, p1c = p1;
    P2 t0{(p0.x + (XY_ONE >> 1)) >> XY_SHIFT, p0.y}, t1{(p1.x + (XY_ONE >> 1)) >> XY_SHIFT, p1.y};
    line8(img, W, H, t0, t1, col);
    if ((uint64_t)t0.x >= (uint64_t)W || (uint64_t)t1.x >= (uint64_t)W || (uint64_t)t0.y >= (uint64_t)H || (uint64_t)t1.y >= (uint64_t)H) {
      clipLine(W, H, t0, t1);  // cv2 5.x: x always from the clipped segment, y only when it is not flat
      if (t0.y != t1.y) { p0c.y = t0.y; p1c.y = t1.y; }
      p0c.x = t0.x << XY_SHIFT; p1c.x = t1.x << XY_SHIFT;
    }
    if (p0.y != p1.y) {
      Edge e; e.dx = (p1c.x - p0c.x) / (p1c.y - p0c.y);
      if (p0.y < p1.y) { e.y0 = (int)p0.y; e.y1 = (int)p1.y; e.x = p0c.x + (p0.y - p0c.y) * e.dx; }
      else { e.y0 = (int)p1.y; e.y1 = (int)p0.y; e.x = p1c.x + (p1.y - p1c.y) * e.dx; }
      E[ne++] = e;
    }
    p0 = p1;
  }
  if (ne < 2) return;
  int ymin = INT32_MAX, ymax = INT32_MIN; int64_t xmin = INT64_MAX, xmax = INT64_MIN;
  for (int i = 0; i < ne; i++) {
    int64_t x1 = E[i].x + (E[i].y1 - E[i].y0) * E[i].dx;
    ymin = std::min(ymin, E[i].y0); ymax = std::max(ymax, E[i].y1);
    xmin = std::min({xmin, E[i].x, x1}); xmax = std::max({xmax, E[i].x, x1});
  }
  if (ymax < 0 || ymin >= H || xmax < 0 || xmin >= ((int64_t)W << XY_SHIFT)) return;
  // convex triangle: at each scanline exactly the active edges span [left, right]
  int yend = std::min(ymax, H);
  for (int y = ymin; y < yend; y++) {
    int64_t xs[8]; int na = 0;
    for (int i = 0; i < ne; i++)
      if (E[i].y0 <= y && y < E[i].y1) xs[na++] = E[i].x + (int64_t)(y - E[i].y0) * E[i].dx;
    if (y < 0 || na < 2) continue;
    for (int a = 1; a < na; a++) for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) std::swap(xs[b - 1], xs[b]);
    for (int j = 0; j + 1 < na; j += 2) {
      int x1 = (int)((xs[j] + XY_ONE - 1) >> XY_SHIFT), x2 = (int)(xs[j + 1] >> XY_SHIFT);
      if (x1 < W && x2 >= 0) {
        if (x1 < 0) x1 = 0;
        if (x2 >= W) x2 = W - 1;
        int32_t* row = img + (size_t)y * W;
        for (int x = x1; x <= x2; x++) row[x] = col;
      }
    }
  }
}
}  // namespace cvr

void fill_tris(const float* v, int nt, const int32_t* tri, const uint8_t* ok, int W, int H, int32_t* ids) {
  for (int i = 0; i < nt; i++) {
    if (ok && !ok[i]) continue;
    int64_t px[3], py[3];
    for (int j = 0; j < 3; j++) {
      int t = tri[i * 3 + j];
      px[j] = (int64_t)std::nearbyint(v[t * 2] * 16.0f);
      py[j] = (int64_t)std::nearbyint(v[t * 2 + 1] * 16.0f);
    }
    cvr::fill_poly(ids, W, H, px, py, 3, i);
  }
}

// ---------------------------------------------------------------- cv2 RGB2HSV (8U, H in 0..180)
void rgb2hsv8(const uint8_t* rgb, int stride, int n, uint8_t* hsv) {
  const int sh = 12;
  struct Tab { int s[256], h[256]; Tab() { s[0] = h[0] = 0; for (int i = 1; i < 256; i++) { s[i] = (int)std::lround((255 << 12) / (1. * i)); h[i] = (int)std::lround((180 << 12) / (6. * i)); } } };
  static const Tab tab;  // thread-safe one-time init
  const int* sdiv = tab.s; const int* hdiv = tab.h;
  for (int i = 0; i < n; i++) {
    const uint8_t* p = rgb + (size_t)i * stride;
    int r = p[0], g = p[1], b = p[2];
    int v = std::max({r, g, b}), vmin = std::min({r, g, b});
    int diff = v - vmin;
    int vr = v == r ? -1 : 0, vg = v == g ? -1 : 0;
    int s = (diff * sdiv[v] + (1 << (sh - 1))) >> sh;
    int hh = (vr & (g - b)) + (~vr & ((vg & (b - r + 2 * diff)) + ((~vg) & (r - g + 4 * diff))));
    hh = (hh * hdiv[diff] + (1 << (sh - 1))) >> sh;
    hh += hh < 0 ? 180 : 0;
    hsv[i * 3] = (uint8_t)hh; hsv[i * 3 + 1] = (uint8_t)s; hsv[i * 3 + 2] = (uint8_t)v;
  }
}

// ---------------------------------------------------------------- connected components (4-conn), raster order of first pixel
// lab[i] = component index (or -1). key[i] = class value; pixels join only with equal key. returns count.
static int label4(const int32_t* key, int w, int h, int32_t* lab, std::vector<int>* first = nullptr) {
  size_t N = (size_t)w * h;
  std::fill(lab, lab + N, -1);
  std::vector<int> stack; int n = 0;
  for (size_t s = 0; s < N; s++) {
    if (key[s] < 0 || lab[s] >= 0) continue;
    int kv = key[s]; lab[s] = n; stack.assign(1, (int)s);
    if (first) first->push_back((int)s);
    while (!stack.empty()) {
      int p = stack.back(); stack.pop_back();
      int x = p % w, y = p / w;
      int nb[4] = {x > 0 ? p - 1 : -1, x < w - 1 ? p + 1 : -1, y > 0 ? p - w : -1, y < h - 1 ? p + w : -1};
      for (int q : nb) if (q >= 0 && key[q] == kv && lab[q] < 0) { lab[q] = n; stack.push_back(q); }
    }
    n++;
  }
  return n;
}

// binary erosion / dilation with 3x3 ones, border 0 (scipy defaults)
static void morph3(const uint8_t* in, int w, int h, uint8_t* out, bool dilate) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      bool r = !dilate;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          int yy = y + dy, xx = x + dx;
          bool v = (yy >= 0 && yy < h && xx >= 0 && xx < w) ? in[(size_t)yy * w + xx] != 0 : false;
          if (dilate) r = r || v; else r = r && v;
        }
      out[(size_t)y * w + x] = r;
    }
}

static inline float luma_f(const uint8_t* p) {
  return ((float)p[0] * 0.299f + (float)p[1] * 0.587f + (float)p[2] * 0.114f) / 255.0f;
}

// float32 percentile (numpy 'linear')
static float percentile_f(std::vector<float> a, double q) {
  size_t n = a.size(); if (!n) return 0;
  double vi = (double)n * q + (1.0 + q * (1.0 - 1.0 - 1.0)) - 1.0;
  if (vi < 0) vi = 0;
  if (vi > (double)(n - 1)) vi = (double)(n - 1);
  size_t lo = (size_t)std::floor(vi), hi = std::min(lo + 1, n - 1);
  float g = (float)(vi - (double)lo);
  std::nth_element(a.begin(), a.begin() + lo, a.end()); float A = a[lo];
  float B = A;
  if (hi != lo) { B = *std::min_element(a.begin() + lo + 1, a.end()); }
  float d = B - A;
  return g >= 0.5f ? B - d * (1.0f - g) : A + d * g;
}

// ---------------------------------------------------------------- make_map
// ---------------------------------------------------------------- make_map as a resumable job
// The work is a list of small units (heavy blurs split by rows) so a caller can spend a fixed time per game frame.
// build() runs the same units back to back, so both paths give identical maps.
struct Job {
  // inputs (copied)
  int w, h, k, nv, nt0; uint32_t sid; Params P;
  std::vector<uint8_t> rgba; std::vector<float> p0, v; std::vector<int32_t> tri; std::vector<uint8_t> tri_ok;
  // state
  int W, H; size_t N, n1;
  int bx0 = 0, bx1 = -1, by0 = 0, by1 = -1;
  bool empty = false;
  std::vector<uint8_t> a1, body, m, wmask, dark, acc1, keep1, hsv;
  std::vector<float> L1, af, up_, Lu, NX, NY, hb, bf, cnt;
  std::vector<int> bp;
  std::vector<int32_t> ids;
  std::vector<double> dt, dome, n0, n1v, edg, lev, rim, sx, sy, fx, fy, r0, r1, r2;
  int nt = 0; float pc = 1.f; std::vector<float> bgx, bgy;
  GaussJob<float> g_mask, g_bf; GaussJob<double> g_dome, g_thick; GaussJob<float> g_hb; GaussJob<float, float> g_hbf;
  EdtJob e_m, e_b;
  std::vector<uint8_t> out;
  std::vector<int32_t> ids_keep;
  // facet attributes taken from frame 0 (same facet ids): a facet keeps its normal and mean thickness while the sprite moves
  const std::vector<double>* ref_fx = nullptr; const std::vector<double>* ref_fy = nullptr; const std::vector<double>* ref_tm = nullptr; const std::vector<float>* ref_cnt = nullptr;
  bool stable_holes = false;
  std::vector<std::function<void()>> units; size_t next = 0;
  Timing T;
  std::function<double(uint32_t, uint32_t, uint32_t)> Hs;

  size_t UP(size_t I) const { return (size_t)((I / W) / k) * w + (I % W) / k; }
  void add(std::function<void()> f) { units.push_back(std::move(f)); }
  // a unit that, when it runs, queues `parts` right after itself (sizes are known only then)
  void add_lazy(std::function<std::vector<std::function<void()>>()> parts) {
    add([this, parts]() {
      std::vector<std::function<void()>> more = parts();
      units.insert(units.begin() + next, more.begin(), more.end());
    });
  }
  static std::function<void()> timed(std::function<void()> f, double* slot) {
    return [f, slot]() { auto t = clk::now(); f(); *slot += ms_since(t); };
  }
  template <class G> void add_rows(G& g, double* slot) {  // blur rows in chunks, after the blur's init unit
    add_lazy([&g, slot]() {
      std::vector<std::function<void()>> more;
      int ch = g.chunk_rows();
      for (int ya = g.y0; ya < g.y1; ya += ch) { int yb = std::min(ya + ch, g.y1); more.push_back(timed([&g, ya, yb]() { g.rows(ya, yb); }, slot)); }
      return more;
    });
  }
  void add_edt(EdtJob& e, double* slot) {  // about 40k pixels per chunk
    add_lazy([this, &e, slot]() {
      std::vector<std::function<void()>> more;
      int cc = std::max(1, 40000 / std::max(H, 1)), rc = std::max(1, 40000 / std::max(W, 1));
      for (int x = 0; x < W; x += cc) more.push_back(timed([&e, x, cc]() { e.cols(x, x + cc); }, slot));
      for (int y = 0; y < H; y += rc) more.push_back(timed([&e, y, rc]() { e.rows(y, y + rc); }, slot));
      return more;
    });
  }
  // body pixels in raster order, in chunks (order-dependent sums stay in order)
  void add_px(std::function<void(size_t, size_t)> f, double* slot, size_t per = 15000) {
    add_lazy([this, f, slot, per]() {
      std::vector<std::function<void()>> more;
      for (size_t a = 0; a < bp.size(); a += per) { size_t b = std::min(a + per, bp.size()); more.push_back(timed([f, a, b]() { f(a, b); }, slot)); }
      return more;
    });
  }

  Job(const uint8_t* rgba_, int w_, int h_, int k_, int nv_, const float* p0_, int nt_, const int32_t* tri_, const float* v_,
      uint32_t sid_, const Params& P_, const uint8_t* tri_ok_)
      : w(w_), h(h_), k(k_), nv(nv_), nt0(nt_), sid(sid_), P(P_) {
    W = w * k; H = h * k; N = (size_t)W * H; n1 = (size_t)w * h;
    rgba.assign(rgba_, rgba_ + n1 * 4); p0.assign(p0_, p0_ + nv * 2); v.assign(v_, v_ + nv * 2); tri.assign(tri_, tri_ + nt0 * 3);
    if (tri_ok_) tri_ok.assign(tri_ok_, tri_ok_ + (nt0 + 7) / 8);
    Hs = P.hash_mix ? hmix : hsh;
    out.assign(N * 4, 0);
    for (size_t I = 0; I < N; I++) { out[I * 4 + 1] = 128; out[I * 4 + 2] = 128; }  // the shader skips a = 0 pixels
    plan();
  }
  bool done() const { return empty || next >= units.size(); }
  // run units until budget_ms of this thread's CPU time is used; returns true when finished
  bool step(double budget_ms) {
    double t0 = clk_ms();
    while (!done()) {
      std::function<void()> f = std::move(units[next++]);  // a unit may insert more units after itself
      f();
      if (empty) { next = units.size(); break; }
      if (clk_ms() - t0 >= budget_ms) break;
    }
    return done();
  }
  static double clk_ms() {
#if defined(__linux__) || defined(__ANDROID__)
    return clk::now();
#else
    return std::chrono::duration<double, std::milli>(clk::now().time_since_epoch()).count();
#endif
  }

  void plan() {
    add([this]() { prep(); });
    add([this]() { fill(); });
    add(timed([this]() { normals_mask_init(); }, &T.normals));
    add_rows(g_mask, &T.normals);
    add(timed([this]() { normals_mask_done(); }, &T.normals));
    add_edt(e_m, &T.normals);
    add(timed([this]() { normals_dome_init(); }, &T.normals));
    add_rows(g_dome, &T.normals);
    add(timed([this]() { n0.assign(N, 0.0); n1v.assign(N, 0.0); g_dome.release(); }, &T.normals));
    add_px([this](size_t a, size_t b) { normals_px(a, b); }, &T.normals);
    add(timed([this]() { std::vector<double>().swap(dome); std::vector<float>().swap(Lu); }, &T.normals));
    add(timed([this]() { holes(); }, &T.holes));
    add(timed([this]() { stats(); }, &T.stats));
    add(timed([this]() { edg.assign(N, 0.0); e_b.init(body.data(), W, H, edg.data()); }, &T.broad));
    add_edt(e_b, &T.broad);
    add(timed([this]() { e_b.release(); broad_init(); }, &T.broad));
    if (P.broad_fast) add_rows(g_hbf, &T.broad); else add_rows(g_hb, &T.broad);
    add(timed([this]() { broad_grad(); }, &T.broad));
    add_px([this](size_t a, size_t b) { broad_px(a, b); }, &T.broad);
    add(timed([this]() { std::vector<float>().swap(hb); std::vector<float>().swap(bgx); std::vector<float>().swap(bgy); }, &T.broad));
    add(timed([this]() { g_thick.init(dt.data(), W, H, k * P.thick_blur, bx0, bx1 + 1, by0, by1 + 1); }, &T.thick));
    add_rows(g_thick, &T.thick);
    add(timed([this]() { thick_prep(); }, &T.thick));
    add_px([this](size_t a, size_t b) { thick_tp(a, b); }, &T.thick, 8000);
    add(timed([this]() { thick_facets(); }, &T.thick));
    add_px([this](size_t a, size_t b) { thick_lev(a, b); }, &T.thick);
    add(timed([this]() { std::vector<double>().swap(dt); std::vector<double>().swap(tp); }, &T.thick));
    add(timed([this]() { colour_prep(); }, &T.colour));
    add_px([this](size_t a, size_t b) { colour_px(a, b); }, &T.colour);
    add(timed([this]() { rim_init(); }, &T.rim));
    add_rows(g_bf, &T.rim);
    add(timed([this]() { g_bf.release(); rim.assign(N, 0.0); }, &T.rim));
    add_px([this](size_t a, size_t b) { rim_px(a, b); }, &T.rim);
    add(timed([this]() { std::vector<float>().swap(bf); finish_prep(); }, &T.out));
    add_px([this](size_t a, size_t b) { finish_px(a, b); }, &T.out);
  }

  void prep() {
    auto t = clk::now();
    a1.assign(n1, 0); L1.assign(n1, 0.f);
    int ax0 = w, ax1 = -1, ay0 = h, ay1 = -1;
    for (size_t i = 0; i < n1; i++) {
      a1[i] = rgba[i * 4 + 3] > 0; L1[i] = luma_f(&rgba[i * 4]);
      if (a1[i]) { int x = (int)(i % w), y = (int)(i / w); ax0 = std::min(ax0, x); ax1 = std::max(ax1, x); ay0 = std::min(ay0, y); ay1 = std::max(ay1, y); }
    }
    if (ax1 < 0) { empty = true; return; }
    bx0 = ax0 * k; bx1 = ax1 * k + k - 1; by0 = ay0 * k; by1 = ay1 * k + k - 1;
    body.assign(N, 0);
    for (int y = by0; y <= by1; y++)
      for (int x = bx0; x <= bx1; x++) {
        size_t I = (size_t)y * W + x;
        if (a1[(size_t)(y / k) * w + x / k]) { body[I] = 1; bp.push_back((int)I); }
      }
    T.fill += ms_since(t);
  }
  void fill() {
    auto t = clk::now();
    ids.assign(N, -1);
    std::vector<uint8_t> ok(nt0, 1);
    if (P.tri_check && !tri_ok.empty()) {
      for (int i = 0; i < nt0; i++) ok[i] = (tri_ok[i >> 3] >> (i & 7)) & 1;
    } else if (P.tri_check) {
      auto area = [&](const float* p, int i) -> float {
        const float *A = p + tri[i * 3] * 2, *B = p + tri[i * 3 + 1] * 2, *C = p + tri[i * 3 + 2] * 2;
        return 0.5f * ((B[0] - A[0]) * (C[1] - A[1]) - (C[0] - A[0]) * (B[1] - A[1]));
      };
      for (int i = 0; i < nt0; i++) {
        float a0 = area(p0.data(), i), b1 = area(v.data(), i);
        float r = b1 / (std::fabs(a0) < 1e-6f ? 1e-6f : a0);
        ok[i] = (r > P.tri_lo) && (r < P.tri_hi);
      }
    }
    fill_tris(v.data(), nt0, tri.data(), ok.data(), W, H, ids.data());
    T.fill += ms_since(t);
  }
  // ---- normals(fr, k, shade_w)
  void normals_mask_init() {
    af.assign(n1, 0.f); up_.assign(N, 0.f);
    for (size_t i = 0; i < n1; i++) af[i] = a1[i] ? 1.f : 0.f;
    resize_linear_f32(af.data(), w, h, k, up_.data());
    int rm = (int)(4.0 * (k * 0.45) + 0.5) + k + 1;  // beyond this the blurred mask is exactly 0
    g_mask.init(up_.data(), W, H, k * 0.45, bx0 - rm, bx1 + 1 + rm, by0 - rm, by1 + 1 + rm);
  }
  void normals_mask_done() {
    g_mask.release();
    m.assign(N, 0);
    for (size_t I = 0; I < N; I++) m[I] = up_[I] > 0.5f;
    std::vector<float>().swap(up_);
    dt.assign(N, 0.0);
    e_m.init(m.data(), W, H, dt.data());
  }
  void normals_dome_init() {
    e_m.release();
    for (auto& d : dt) d = d / (double)k;
    dome.assign(N, 0.0);
    for (size_t I = 0; I < N; I++) dome[I] = std::sqrt(dt[I]);
    std::vector<float> La(n1), G2(n1), Lm(n1);
    for (size_t i = 0; i < n1; i++) { La[i] = L1[i] * (a1[i] ? 1.f : 0.f); G2[i] = af[i]; }
    gauss_f32(La.data(), w, h, 0.8); gauss_f32(G2.data(), w, h, 0.8);
    for (size_t i = 0; i < n1; i++) Lm[i] = La[i] / std::max(G2[i], 1e-3f);
    Lu.assign(N, 0.f);
    resize_linear_f32(Lm.data(), w, h, k, Lu.data());
    g_dome.init(dome.data(), W, H, k * 0.5, bx0 - 1, bx1 + 2, by0 - 1, by1 + 2);
  }
  void normals_px(size_t a, size_t b) {  // np.gradient of hgt*k at body pixels
    const float swf = (float)P.shade_w;
    auto HK = [&](size_t I) { return (dome[I] + (double)(swf * Lu[I])) * (double)k; };
    const double c55 = 0.55 * 0.55;
    for (size_t q = a; q < b; q++) {
      int I = bp[q], x = I % W, y = I / W;
      double gy = (H < 2) ? 0.0 : (y == 0 ? (HK(I + W) - HK(I)) / 1.0 : (y == H - 1 ? (HK(I) - HK(I - W)) / 1.0 : (HK(I + W) - HK(I - W)) / 2.0));
      double gx = (W < 2) ? 0.0 : (x == 0 ? (HK(I + 1) - HK(I)) / 1.0 : (x == W - 1 ? (HK(I) - HK(I - 1)) / 1.0 : (HK(I + 1) - HK(I - 1)) / 2.0));
      double aa = -gx, bb = -gy;
      double l = std::sqrt((aa * aa + bb * bb) + c55);
      n0[I] = aa / l; n1v[I] = bb / l;
    }
  }
  // ---- holes left by rejected triangles: cells of quantised normal direction
  void holes() {
    nt = nt0;
    if (P.tri_check) {
      std::vector<int32_t> key(N, -1);
      bool any = false;
      const int na = P.hole_na, nb = P.hole_nb;
      for (int I : bp) {
        if (ids[I] >= 0) continue;
        double ang = (std::atan2(n1v[I], n0[I]) / (2 * PI) + 0.5) * na;
        double mag = std::sqrt(n0[I] * n0[I] + n1v[I] * n1v[I]);
        long long qa = ((long long)std::floor(ang + 0.5)) % na; if (qa < 0) qa += na;
        long long qm = std::min((long long)(mag * nb * 1.15), (long long)nb - 1);
        key[I] = (int32_t)(qa * nb + qm); any = true;
      }
      if (any) {
        std::vector<int32_t> lab(N); std::vector<int> first;
        int nc = label4(key.data(), W, H, lab.data(), &first);
        std::vector<int> order(nc);
        for (int c = 0; c < nc; c++) order[c] = c;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return key[first[a]] < key[first[b]]; });
        std::vector<int> rank(nc);
        for (int r = 0; r < nc; r++) rank[order[r]] = r;
        if (stable_holes) {  // hole facets numbered by their direction cell, the same in every frame
          for (int I : bp) if (lab[I] >= 0) ids[I] = nt0 + key[I];
          nt = nt0 + na * nb;
        } else {
          for (int I : bp) if (lab[I] >= 0) ids[I] = nt0 + rank[lab[I]];
          nt = nt0 + nc;
        }
      }
    }
    {
      std::vector<int32_t> cp(N, -1);
      for (int I : bp) cp[I] = ids[I];
      ids.swap(cp);
    }
    std::vector<int> miss, src;  // pixels not covered by any triangle take the nearest one (only when tri_check is off)
    for (int I : bp) (ids[I] < 0 ? miss : src).push_back(I);
    if (!miss.empty() && !src.empty()) {
      std::vector<int32_t> cp = ids;
      for (int I : miss) {
        int x = I % W, y = I / W; long best = LONG_MAX; int bi = src[0];
        for (int S : src) { long dx = S % W - x, dy = S / W - y, d = dx * dx + dy * dy; if (d < best) { best = d; bi = S; } }
        cp[I] = ids[bi];
      }
      ids.swap(cp);
    }
    if (!miss.empty() && src.empty()) for (int I : miss) ids[I] = 0;
  }
  // ---- per-facet stats
  void stats() {
    if (nt < 1) nt = 1;
    cnt.assign(nt, 0.f); sx.assign(nt, 0.0); sy.assign(nt, 0.0);
    for (int I : bp) { int i = ids[I]; cnt[i] += 1.f; sx[i] += n0[I]; sy[i] += n1v[I]; }
    fx.resize(nt); fy.resize(nt); r0.resize(nt); r1.resize(nt); r2.resize(nt);
    const double tilt = P.tilt;
    for (int i = 0; i < nt; i++) {
      r0[i] = Hs(sid, i, 1); r1[i] = Hs(sid, i, 2); r2[i] = Hs(sid, i, 3);
      double c = std::max(cnt[i], 1.f);
      double mx = sx[i] / c, my = sy[i] / c;
      double a = mx + (r0[i] - .5) * 2 * tilt, b = my + (r1[i] - .5) * 2 * tilt;
      double r = std::sqrt(a * a + b * b), sc = std::min(1.0, 0.97 / std::max(r, 1e-6));
      fx[i] = a * sc; fy[i] = b * sc;
    }
    if (ref_fx) {
      const int m = std::min<int>(nt0, (int)std::min(ref_fx->size(), ref_cnt->size()));
      for (int i = 0; i < m; i++) if ((*ref_cnt)[i] > 0.f) { fx[i] = (*ref_fx)[i]; fy[i] = (*ref_fy)[i]; }
    }
    NX.assign(N, 0.f); NY.assign(N, 0.f);
    const double sw = P.smooth_n;
    for (int I : bp) {
      int i = ids[I];
      NX[I] = (float)(fx[i] * (1 - sw) + n0[I] * sw); NY[I] = (float)(fy[i] * (1 - sw) + n1v[I] * sw);
    }
    std::vector<double>().swap(n0); std::vector<double>().swap(n1v);
  }
  // ---- broad body-scale normal (edg = distance to the silhouette, also used by the rim)
  void broad_init() {
    if (!(P.broad > 0)) return;
    int bh = std::max({by1 - by0, bx1 - bx0, 1});
    hb.assign(N, 0.f);
    for (size_t I = 0; I < N; I++) hb[I] = (float)edg[I];
    if (P.broad_fast) g_hbf.init(hb.data(), W, H, bh * P.broad, bx0 - 1, bx1 + 2, by0 - 1, by1 + 2);
    else g_hb.init(hb.data(), W, H, bh * P.broad, bx0 - 1, bx1 + 2, by0 - 1, by1 + 2);
  }
  void broad_grad() {
    g_hb.release(); g_hbf.release();
    if (!(P.broad > 0)) return;
    bgx.assign(N, 0.f); bgy.assign(N, 0.f);
    std::vector<float> mg; mg.reserve(bp.size());
    for (int I : bp) {
      int x = I % W, y = I / W;
      bgy[I] = (H < 2) ? 0.f : (y == 0 ? (hb[I + W] - hb[I]) / 1.0f : (y == H - 1 ? (hb[I] - hb[I - W]) / 1.0f : (hb[I + W] - hb[I - W]) / 2.0f));
      bgx[I] = (W < 2) ? 0.f : (x == 0 ? (hb[I + 1] - hb[I]) / 1.0f : (x == W - 1 ? (hb[I] - hb[I - 1]) / 1.0f : (hb[I + 1] - hb[I - 1]) / 2.0f));
      float a = -bgx[I], b = -bgy[I]; mg.push_back(std::sqrt(a * a + b * b));
    }
    pc = percentile_f(mg, 0.95);
    if (pc < 1e-6f) pc = 1e-6f;
  }
  void broad_px(size_t a, size_t b) {
    if (!(P.broad > 0)) return;
    const float gain = (float)P.broad_gain; const double fw = P.facet_n, bt = P.broad_tilt;
    for (size_t q = a; q < b; q++) {
      int I = bp[q], i = ids[I];
      float g0 = -bgx[I] / pc * gain, g1 = -bgy[I] / pc * gain;
      double ft0 = (r0[i] - .5) * 2 * bt, ft1 = (r1[i] - .5) * 2 * bt;
      NX[I] = (float)clip(((double)g0 + ft0) * (1 - fw) + fx[i] * fw, -0.97, 0.97);
      NY[I] = (float)clip(((double)g1 + ft1) * (1 - fw) + fy[i] * fw, -0.97, 0.97);
    }
  }
  // ---- thickness and facet level
  double dn = 1.0; std::vector<double> tp, tm_, flev;
  void thick_prep() {
    g_thick.release();
    lev.assign(N, 0.0);
    double mx = -1e300;
    for (int I : bp) mx = std::max(mx, dt[I]);
    dn = std::max(mx, 1e-3);
    tp.assign(N, 0.0); tm_.assign(nt, 0.0);
  }
  void thick_tp(size_t a, size_t b) {
    for (size_t q = a; q < b; q++) { int I = bp[q]; tp[I] = std::pow(dt[I] / dn, P.thick_pow); tm_[ids[I]] += tp[I]; }
  }
  void thick_facets() {
    for (int i = 0; i < nt; i++) tm_[i] = tm_[i] / (double)std::max(cnt[i], 1.f);
    if (ref_tm) {
      const int m = std::min<int>(nt0, (int)std::min(ref_tm->size(), ref_cnt->size()));
      for (int i = 0; i < m; i++) if ((*ref_cnt)[i] > 0.f) tm_[i] = (*ref_tm)[i];
    }
    flev.resize(nt);
    for (int i = 0; i < nt; i++) {
      double lam = clip(-(fx[i] * P.lx + fy[i] * P.ly), -1.0, 1.0);
      flev[i] = clip(P.facet * std::pow(r2[i], P.facet_pow) + P.lam * lam, -0.3, 1.0);
    }
  }
  void thick_lev(size_t a, size_t b) {
    const double q = P.thick_facet;
    for (size_t j = a; j < b; j++) {
      int I = bp[j];
      double tq = tm_[ids[I]] * q + tp[I] * (1 - q);
      lev[I] = P.base + P.thick * tq + flev[ids[I]];
    }
  }
  // ---- colour-based masks (1x HSV)
  std::vector<uint8_t> wf, df;
  float HH(size_t i) const { return (float)hsv[i * 3] * 2.f; }
  float SS(size_t i) const { return (float)hsv[i * 3 + 1] / 255.f; }
  float VV(size_t i) const { return (float)hsv[i * 3 + 2] / 255.f; }
  static float dhue(float a, float b) { return std::fabs(pymodf(a - b + 180.f, 360.f) - 180.f); }
  void colour_prep() {
    hsv.assign(n1 * 3, 0);
    rgb2hsv8(rgba.data(), 4, (int)n1, hsv.data());
    bool need_mh = P.accent > 0 || P.keep_small > 0;
    float mh = 0; bool has_mh = false;
    if (need_mh) {  // unweighted circular mean hue of saturated pixels (float32 like numpy)
      std::vector<float> s, c;
      for (size_t i = 0; i < n1; i++) if (a1[i] && SS(i) > 0.25f) {
        float an = HH(i) * ((float)PI / 180.0f);
        s.push_back(std::sin(an)); c.push_back(std::cos(an));
      }
      if (!s.empty()) {
        float ms = pairwise(s.data(), s.size()) / (float)s.size(), mc = pairwise(c.data(), c.size()) / (float)c.size();
        mh = pymodf(std::atan2(ms, mc) * (180.0f / (float)PI), 360.f); has_mh = true;
      }
    }
    acc1.assign(n1, 0); keep1.assign(n1, 0);
    if (P.accent > 0 && has_mh)
      for (size_t i = 0; i < n1; i++)
        acc1[i] = a1[i] && dhue(HH(i), mh) > 32.f && SS(i) >= 0.4f && VV(i) >= (float)P.accent_v;
    if (P.keep_small > 0 && has_mh) {
      std::vector<int32_t> key(n1), lab(n1);
      for (size_t i = 0; i < n1; i++)
        key[i] = (a1[i] && dhue(HH(i), mh) > 32.f && SS(i) >= 0.35f && VV(i) >= 0.55f) ? 0 : -1;
      int nc = label4(key.data(), w, h, lab.data());
      std::vector<int> sz(nc, 0);
      for (size_t i = 0; i < n1; i++) if (lab[i] >= 0) sz[lab[i]]++;
      for (size_t i = 0; i < n1; i++) keep1[i] = lab[i] >= 0 && sz[lab[i]] <= P.keep_small;
      if (P.keep_upper > 0) {
        int ry0 = h, ry1 = -1;
        for (size_t i = 0; i < n1; i++) if (a1[i]) { int y = (int)(i / w); ry0 = std::min(ry0, y); ry1 = std::max(ry1, y); }
        const double lim = ry0 + P.keep_upper * (ry1 - ry0 + 1);
        for (size_t i = 0; i < n1; i++) if (keep1[i] && !((double)(i / w) < lim)) keep1[i] = 0;
      }
    }
    wmask.assign(N, 0); dark.assign(N, 0);
    wf.assign(nt, 0); df.assign(nt, 0);
    for (int i = 0; i < nt; i++) {
      wf[i] = P.white_frac > 0 && Hs(sid, i, 5) < P.white_frac;
      df[i] = P.dark_frac > 0 && Hs(sid, i, 7) < P.dark_frac;
    }
  }
  void colour_px(size_t a, size_t b) {  // white / dark facets, accents, then glinting facet edges (per pixel, same order)
    for (size_t q = a; q < b; q++) {
      int I = bp[q];
      size_t i1 = UP(I); int id = ids[I];
      if (P.white_frac > 0 && wf[id] && !acc1[i1]) { wmask[I] = 1; lev[I] = std::max(lev[I], P.white_lev); }
      if (P.dark_frac > 0 && df[id] && !(P.thin > 0 && !(edg[I] > P.thin * k))) { dark[I] = 1; lev[I] = std::min(lev[I], P.dark_lev); }
      if (acc1[i1]) lev[I] = std::min(lev[I], P.accent);
      if (P.edge > 0) {
        int x = I % W, y = I / W;
        int32_t up = ids[(size_t)((y - 1 + H) % H) * W + x], lf = ids[(size_t)y * W + (x - 1 + W) % W];
        bool bnd = (up != id && up >= 0) || (lf != id && lf >= 0);
        if (bnd && r0[id] < P.edge_frac) lev[I] = std::max(lev[I], P.edge);
      }
    }
  }
  // ---- rim
  void rim_init() {
    bf.assign(N, 0.f);
    for (size_t I = 0; I < N; I++) bf[I] = body[I] ? 1.f : 0.f;
    g_bf.init(bf.data(), W, H, k * 0.8, bx0 - 1, bx1 + 2, by0 - 1, by1 + 2);
  }
  void rim_px(size_t a, size_t b) {
    const float lxf = (float)P.lx, lyf = (float)P.ly;
    for (size_t q = a; q < b; q++) {
      int I = bp[q], x = I % W, y = I / W;
      float gy = (H < 2) ? 0.f : (y == 0 ? (bf[I + W] - bf[I]) / 1.0f : (y == H - 1 ? (bf[I] - bf[I - W]) / 1.0f : (bf[I + W] - bf[I - W]) / 2.0f));
      float gx = (W < 2) ? 0.f : (x == 0 ? (bf[I + 1] - bf[I]) / 1.0f : (x == W - 1 ? (bf[I] - bf[I - 1]) / 1.0f : (bf[I + 1] - bf[I - 1]) / 2.0f));
      float gl = std::sqrt(gx * gx + gy * gy) + 1e-6f;
      float face = clip(((gx / gl) * lxf + (gy / gl) * lyf) * 0.5f + 0.5f, 0.f, 1.f);
      double rr = clip((P.rim_w + 0.5 - edg[I]) / 1.0, 0.0, 1.0) * P.rim;
      double s = (double)(0.6f * face) + 0.4 * r1[ids[I]];
      rr = rr * clip(P.rim_min + (1 - P.rim_min) * s, 0.0, 1.0);
      rim[I] = rr;
      lev[I] = std::max(lev[I], rr);
    }
  }
  // ---- line / reflection compensation and output (per pixel: white_comp, outline, inner lines, write)
  std::vector<uint8_t> il1; float main_mh = 0; bool main_ok = false; float rep = 1.0f;
  void finish_prep() {
    il1.assign(n1, 0);
    if (P.outline_comp > 0 && P.inner_comp > 0) {
      std::vector<uint8_t> d1(n1), e1(n1), o1(n1), wide(n1);
      for (size_t i = 0; i < n1; i++) d1[i] = a1[i] && L1[i] < (float)P.inner_lum;
      morph3(d1.data(), w, h, e1.data(), false); morph3(e1.data(), w, h, o1.data(), true);
      morph3(o1.data(), w, h, wide.data(), true);
      for (size_t i = 0; i < n1; i++) il1[i] = d1[i] && !wide[i];
    }
    double main_fr = 0;
    if (P.keep_sat > 0 && P.keep_main != 1) {
      if (P.has_main_hf) { main_ok = !P.main_none; main_mh = (float)P.main_h; main_fr = P.main_frac; }
      else {  // main_colour(fr): saturation-weighted mean hue
        double ss = 0, cc = 0; bool any = false;
        for (size_t i = 0; i < n1; i++) if (a1[i] && SS(i) > 0.25f) {
          double an = HH(i) * PI / 180.0, wgt = SS(i); ss += std::sin(an) * wgt; cc += std::cos(an) * wgt; any = true;
        }
        if (any) {
          double m_ = pymod(std::atan2(ss, cc) * 180.0 / PI, 360.0); int cnt_m = 0, ca = 0;
          for (size_t i = 0; i < n1; i++) if (a1[i]) { ca++; if (SS(i) > 0.35f && VV(i) > 0.35f && dhue(HH(i), (float)m_) <= 32.f) cnt_m++; }
          main_ok = true; main_mh = (float)m_; main_fr = (double)cnt_m / std::max(ca, 1);
        }
      }
    }
    if (P.keep_main == 2) rep = (float)clip((main_fr - P.main_frac0) / P.main_frac_w, 0.0, 1.0);
  }
  void finish_px(size_t a, size_t b) {
    const float lb = (float)P.lum_base, lr = (float)P.lum_ratio;
    const bool wcomp = P.white_comp > 0 && P.white_frac > 0; const float wc = (float)P.white_comp;
    const double fl = P.line_floor; const float oc = (float)P.outline_comp, orim = (float)P.outline_rim, ic = (float)P.inner_comp;
    const float dw = (float)std::nearbyint(P.dark_w * 255), kw = (float)std::nearbyint(P.keep_w * 255);
    const float ksat = (float)P.keep_sat;
    for (size_t q = a; q < b; q++) {
      int I = bp[q];
      size_t i1 = UP(I); float L = L1[i1];
      if (wcomp && (wmask[I] || rim[I] > 0.5)) lev[I] = lev[I] + (double)(clip(lb - L, 0.f, 1.f) * lr * wc);
      if (P.outline_comp > 0) {
        if (edg[I] <= k + 0.5) {
          if (P.outline_rim > 0) lev[I] = std::max(lev[I], (double)(orim + clip(lb - L, 0.f, 1.f) * lr));
          lev[I] = std::max(lev[I], fl) + (double)(clip(lb - L, 0.f, 1.f) * lr * oc);
        }
        if (P.inner_comp > 0 && edg[I] > k + 0.5 && il1[i1] && !acc1[i1])
          lev[I] = std::max(lev[I], fl) + (double)(clip(lb - L, 0.f, 1.f) * lr * ic);
      }
      out[I * 4] = (uint8_t)clip(std::nearbyint(lev[I] * 255), 0.0, 255.0);
      out[I * 4 + 1] = (uint8_t)clip(std::nearbyint((NX[I] * 0.5f + 0.5f) * 255.f), 0.f, 255.f);
      out[I * 4 + 2] = (uint8_t)clip(std::nearbyint((NY[I] * 0.5f + 0.5f) * 255.f), 0.f, 255.f);
      float av = dark[I] ? dw : 255.f;
      if (P.keep_sat > 0) {
        float s2 = SS(i1), v2 = VV(i1);
        float ks = clip((s2 - 0.35f) / 0.3f, 0.f, 1.f) * (v2 > 0.35f ? 1.f : 0.f);
        if (P.keep_main != 1 && main_ok) {
          float dh = dhue(HH(i1), main_mh);
          ks = ks * (1.f - rep * (1.f - clip((dh - 24.f) / 16.f, 0.f, 1.f)));
        }
        av = av * (1.f - ksat * ks);
      }
      if (P.has_a_lo) {
        float al = (float)P.a_lo;
        av = av * clip(al + (float)(1 - P.a_lo) * std::pow(1.f - L, (float)P.a_pow), 0.f, 1.f);
      }
      if (keep1[i1]) av = kw;
      out[I * 4 + 3] = (uint8_t)clip(std::nearbyint(av), 1.f, 255.f);
    }
  }
};

int build(const uint8_t* rgba, int w, int h, int k, int nv, const float* p0, int nt0, const int32_t* tri,
          const float* v, uint32_t sid, const Params& P, uint8_t* out, int32_t* ids_out, Timing* tm, const uint8_t* tri_ok) {
  if (!rgba || !out || !v || !p0 || (nt0 > 0 && !tri) || w <= 0 || h <= 0 || k <= 0 || nv < 0 || nt0 < 0) return 1;
  for (int i = 0; i < nt0 * 3; i++) if (tri[i] < 0 || tri[i] >= nv) return 2;
  Job j(rgba, w, h, k, nv, p0, nt0, tri, v, sid, P, tri_ok);
  while (!j.step(1e300)) {}
  std::memcpy(out, j.out.data(), j.out.size());
  if (ids_out) {
    if (j.empty) std::fill(ids_out, ids_out + j.N, -1);
    else std::memcpy(ids_out, j.ids.data(), j.N * sizeof(int32_t));
  }
  if (tm) *tm = j.T;
  return 0;
}

// ================================================================ v3: mesh tracks (.tmv) from the sprite frames
// Port of tmv.track (build.Mesh + cv2 DIS PRESET_MEDIUM, frame 0 -> frame i, flow blurred by k*1.2, read at the vertex).
// Mesh points use numpy's PCG64(seed 5) stream, so they are the same float32 values as the Python precompute.
namespace trk {

// ---------------------------------------------------------------- numpy Generator(PCG64), seed 5
struct U128 { uint64_t hi, lo; };
static inline U128 mul64(uint64_t a, uint64_t b) {
  const uint64_t a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
  const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  const uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
  U128 r; r.lo = (p00 & 0xffffffffu) | (mid << 32); r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
  return r;
}
struct Pcg64 {
  uint64_t sh = 0xe500c1d129ce9183ULL, sl = 0xd973b921e554efeeULL;  // default_rng(5).bit_generator.state
  uint64_t ih = 0xaf6f7b8c936ab4a4ULL, il = 0x4f020b7b2d10234dULL;
  bool has32 = false; uint32_t u32 = 0;
  uint64_t next64() {
    const uint64_t mh = 0x2360ED051FC65DA4ULL, ml = 0x4385DF649FCCF645ULL;
    U128 p = mul64(sl, ml);
    uint64_t hi = p.hi + sl * mh + sh * ml, lo = p.lo + il;
    hi += ih + (lo < il ? 1 : 0);
    sh = hi; sl = lo;
    const uint64_t x = sh ^ sl; const unsigned rot = (unsigned)(sh >> 58);
    return (x >> rot) | (x << ((64 - rot) & 63));
  }
  uint32_t next32() {
    if (has32) { has32 = false; return u32; }
    const uint64_t n = next64(); has32 = true; u32 = (uint32_t)(n >> 32); return (uint32_t)n;
  }
  double next_double() { return (double)(next64() >> 11) * (1.0 / 9007199254740992.0); }
  double uniform(double lo, double hi) { return lo + (hi - lo) * next_double(); }
  uint64_t bounded(uint64_t rng) {  // random_bounded_uint64(off 0, rng, use_masked 0), rng < 2^32
    if (rng == 0) return 0;
    if (rng == 0xFFFFFFFFULL) return next32();
    const uint32_t rexcl = (uint32_t)rng + 1;
    uint64_t m = (uint64_t)next32() * rexcl; uint32_t left = (uint32_t)m;
    if (left < rexcl) {
      const uint32_t thr = (uint32_t)((0xFFFFFFFFu - (uint32_t)rng) % rexcl);
      while (left < thr) { m = (uint64_t)next32() * rexcl; left = (uint32_t)m; }
    }
    return m >> 32;
  }
  void shuffle_int(int64_t n, int64_t first, int64_t* d) {
    for (int64_t i = n - 1; i >= first; i--) { const int64_t j = (int64_t)bounded((uint64_t)i); std::swap(d[i], d[j]); }
  }
  // Generator.choice(pop, size, replace=False)
  std::vector<int64_t> choice(int64_t pop, int64_t size) {
    std::vector<int64_t> idx;
    if (pop > 10000 && size > pop / 50) {
      idx.resize(pop);
      for (int64_t i = 0; i < pop; i++) idx[i] = i;
      shuffle_int(pop, std::max<int64_t>(pop - size, 1), idx.data());
      return std::vector<int64_t>(idx.end() - size, idx.end());
    }
    idx.resize(size);
    uint64_t mask = (uint64_t)(1.2 * (double)size);
    for (int s = 1; s < 64; s <<= 1) mask |= mask >> s;
    std::vector<uint64_t> hs(mask + 1, ~0ULL);
    for (int64_t j = pop - size; j < pop; j++) {
      const uint64_t val = bounded((uint64_t)j);
      uint64_t loc = val & mask;
      while (hs[loc] != ~0ULL && hs[loc] != val) loc = (loc + 1) & mask;
      if (hs[loc] == ~0ULL) { hs[loc] = val; idx[j - pop + size] = (int64_t)val; }
      else {
        loc = (uint64_t)j & mask;
        while (hs[loc] != ~0ULL) loc = (loc + 1) & mask;
        hs[loc] = (uint64_t)j; idx[j - pop + size] = j;
      }
    }
    shuffle_int(size, 1, idx.data());
    return idx;
  }
};

// ---------------------------------------------------------------- exact orientation / in-circle (fixed point 2^-24 px)
struct Big {  // sign-magnitude, 256-bit magnitude
  int s = 0; uint32_t m[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  static Big of(int64_t v) {
    Big b; if (v == 0) return b;
    b.s = v < 0 ? -1 : 1; uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    b.m[0] = (uint32_t)u; b.m[1] = (uint32_t)(u >> 32); return b;
  }
  static int cmpmag(const Big& a, const Big& b) {
    for (int i = 7; i >= 0; i--) if (a.m[i] != b.m[i]) return a.m[i] < b.m[i] ? -1 : 1;
    return 0;
  }
  static Big addmag(const Big& a, const Big& b, int s) {
    Big r; uint64_t c = 0;
    for (int i = 0; i < 8; i++) { c += (uint64_t)a.m[i] + b.m[i]; r.m[i] = (uint32_t)c; c >>= 32; }
    r.s = s; return r;
  }
  static Big submag(const Big& a, const Big& b, int s) {  // |a| >= |b|
    Big r; int64_t c = 0;
    for (int i = 0; i < 8; i++) { int64_t t = (int64_t)a.m[i] - b.m[i] + c; c = t < 0 ? -1 : 0; r.m[i] = (uint32_t)(t + (c ? (1LL << 32) : 0)); }
    r.s = s; bool z = true;
    for (int i = 0; i < 8; i++) if (r.m[i]) z = false;
    if (z) r.s = 0;
    return r;
  }
  Big operator+(const Big& b) const {
    if (!s) return b;
    if (!b.s) return *this;
    if (s == b.s) return addmag(*this, b, s);
    const int c = cmpmag(*this, b);
    if (c == 0) return Big();
    return c > 0 ? submag(*this, b, s) : submag(b, *this, b.s);
  }
  Big neg() const { Big r = *this; r.s = -s; return r; }
  Big operator-(const Big& b) const { return *this + b.neg(); }
  Big operator*(const Big& b) const {
    Big r; if (!s || !b.s) return r;
    uint64_t t[16] = {0};
    for (int i = 0; i < 8; i++) {
      if (!m[i]) continue;
      uint64_t c = 0;
      for (int j = 0; j + i < 8; j++) { uint64_t p = (uint64_t)m[i] * b.m[j] + t[i + j] + c; t[i + j] = p & 0xffffffffu; c = p >> 32; }
    }
    for (int i = 0; i < 8; i++) r.m[i] = (uint32_t)t[i];
    r.s = s * b.s; return r;
  }
};
struct FP { int64_t x, y; double dx, dy; };
static const double EPSD = 1.1102230246251565e-16;  // 2^-53
static int orient_sign(const FP& a, const FP& b, const FP& c) {  // sign of (b-a) x (c-a)
  const double l = (b.dx - a.dx) * (c.dy - a.dy), r = (b.dy - a.dy) * (c.dx - a.dx), d = l - r;
  const double bound = (3.0 + 16.0 * EPSD) * EPSD * (std::fabs(l) + std::fabs(r));
  if (d > bound) return 1;
  if (-d > bound) return -1;
  Big e = Big::of(b.x - a.x) * Big::of(c.y - a.y) - Big::of(b.y - a.y) * Big::of(c.x - a.x);
  return e.s;
}
static int incircle_sign(const FP& a, const FP& b, const FP& c, const FP& p) {  // > 0: p inside circle of ccw abc
  const double adx = a.dx - p.dx, ady = a.dy - p.dy, bdx = b.dx - p.dx, bdy = b.dy - p.dy, cdx = c.dx - p.dx, cdy = c.dy - p.dy;
  const double bc1 = bdx * cdy, bc2 = cdx * bdy, ca1 = cdx * ady, ca2 = adx * cdy, ab1 = adx * bdy, ab2 = bdx * ady;
  const double al = adx * adx + ady * ady, bl = bdx * bdx + bdy * bdy, cl = cdx * cdx + cdy * cdy;
  const double det = al * (bc1 - bc2) + bl * (ca1 - ca2) + cl * (ab1 - ab2);
  const double perm = (std::fabs(bc1) + std::fabs(bc2)) * al + (std::fabs(ca1) + std::fabs(ca2)) * bl + (std::fabs(ab1) + std::fabs(ab2)) * cl;
  const double bound = (10.0 + 96.0 * EPSD) * EPSD * perm;
  if (det > bound) return 1;
  if (-det > bound) return -1;
  const Big ax = Big::of(a.x - p.x), ay = Big::of(a.y - p.y), bx = Big::of(b.x - p.x), by = Big::of(b.y - p.y);
  const Big cx = Big::of(c.x - p.x), cy = Big::of(c.y - p.y);
  Big e = (ax * ax + ay * ay) * (bx * cy - cx * by) + (bx * bx + by * by) * (cx * ay - ax * cy) + (cx * cx + cy * cy) * (ax * by - bx * ay);
  return e.s;
}

// ---------------------------------------------------------------- Delaunay (sorted sweep + flips, delaunator layout)
struct Delaunay {
  std::vector<FP> P;
  std::vector<int32_t> tri, half;
  std::vector<int> hnext, hprev, htri, stack;
  int hstart = 0;
  // hull edge q->r is visible from new point p (hull kept clockwise, as in delaunator)
  bool vis(int p, int q, int r) const { return orient_sign(P[p], P[q], P[r]) > 0; }
  void link(int a, int b) { half[a] = b; if (b != -1) half[b] = a; }
  int add(int i0, int i1, int i2, int a, int b, int c) {
    const int t = (int)tri.size();
    tri.push_back(i0); tri.push_back(i1); tri.push_back(i2);
    half.push_back(-1); half.push_back(-1); half.push_back(-1);
    link(t, a); link(t + 1, b); link(t + 2, c);
    return t;
  }
  int legalize(int a) {
    int ar = 0; stack.clear();
    for (;;) {
      const int b = half[a];
      const int a0 = a - a % 3; ar = a0 + (a + 2) % 3;
      if (b == -1) { if (stack.empty()) break; a = stack.back(); stack.pop_back(); continue; }
      const int b0 = b - b % 3, al = a0 + (a + 1) % 3, bl = b0 + (b + 2) % 3;
      const int p0 = tri[ar], pr = tri[a], pl = tri[al], p1 = tri[bl];
      // triangles are clockwise in y-up terms: p1 inside the circle of (p0, pr, pl) <=> in-circle determinant < 0
      if (incircle_sign(P[p0], P[pr], P[pl], P[p1]) < 0) {
        tri[a] = p1; tri[b] = p0;
        const int hbl = half[bl];
        if (hbl == -1) {
          int e = hstart;
          do { if (htri[e] == bl) { htri[e] = a; break; } e = hprev[e]; } while (e != hstart);
        }
        link(a, hbl); link(b, half[ar]); link(ar, bl);
        stack.push_back(b0 + (b + 1) % 3);
      } else {
        if (stack.empty()) break;
        a = stack.back(); stack.pop_back();
      }
    }
    return ar;
  }
  // returns 0 ok, 1 all points collinear / too few
  int run(const float* xy, int n) {
    P.resize(n);
    for (int i = 0; i < n; i++) {
      const double x = (double)xy[2 * i], y = (double)xy[2 * i + 1];
      FP f; f.x = (int64_t)std::llround(x * 16777216.0); f.y = (int64_t)std::llround(y * 16777216.0);
      f.dx = (double)f.x; f.dy = (double)f.y; P[i] = f;
    }
    if (n < 3) return 1;
    std::vector<int> ord(n);
    for (int i = 0; i < n; i++) ord[i] = i;
    std::sort(ord.begin(), ord.end(), [&](int a, int b) {
      if (P[a].x != P[b].x) return P[a].x < P[b].x;
      if (P[a].y != P[b].y) return P[a].y < P[b].y;
      return a < b;
    });
    int i0 = ord[0], i1 = -1, k1 = 1;
    while (k1 < n && P[ord[k1]].x == P[i0].x && P[ord[k1]].y == P[i0].y) k1++;
    if (k1 >= n) return 1;
    i1 = ord[k1];
    int k2 = k1 + 1;
    while (k2 < n && orient_sign(P[i0], P[i1], P[ord[k2]]) == 0) k2++;
    if (k2 >= n) return 1;
    int i2 = ord[k2];
    if (orient_sign(P[i0], P[i1], P[i2]) > 0) std::swap(i1, i2);  // clockwise seed (delaunator)
    hnext.assign(n, -1); hprev.assign(n, -1); htri.assign(n, -1);
    tri.clear(); half.clear(); tri.reserve((size_t)n * 6); half.reserve((size_t)n * 6);
    hstart = i0;
    hnext[i0] = hprev[i2] = i1; hnext[i1] = hprev[i0] = i2; hnext[i2] = hprev[i1] = i0;
    htri[i0] = 0; htri[i1] = 1; htri[i2] = 2;
    add(i0, i1, i2, -1, -1, -1);
    int last = ord[k2] == i2 ? i2 : i1;  // the seed vertex greatest in sort order
    std::vector<int> rest; rest.reserve(n);
    for (int k = k1 + 1; k < k2; k++) rest.push_back(ord[k]);
    for (int k = k2 + 1; k < n; k++) rest.push_back(ord[k]);
    int hsize = 3;
    for (int i : rest) {
      int e = last, q;
      if (!vis(i, e, hnext[e])) e = hprev[last];
      if (!vis(i, e, hnext[e])) {
        bool found = false; e = last;
        for (int guard = 0; guard <= hsize; guard++) {
          if (vis(i, e, hnext[e])) { found = true; break; }
          e = hnext[e];
        }
        if (!found) continue;  // duplicate point
      }
      for (int guard = 0; guard <= hsize && vis(i, hprev[e], e); guard++) e = hprev[e];
      int t = add(e, i, hnext[e], -1, -1, htri[e]);
      htri[i] = legalize(t + 2); htri[e] = t; hsize++;
      int nn = hnext[e];
      while (q = hnext[nn], vis(i, nn, q)) {
        t = add(nn, i, q, htri[i], -1, htri[nn]);
        htri[i] = legalize(t + 2); hnext[nn] = nn; hsize--; nn = q;
      }
      hstart = hprev[i] = e; hnext[e] = hprev[nn] = i; hnext[i] = nn;
      last = i;
    }
    return 0;
  }
};

// ---------------------------------------------------------------- image helpers (cv2 equivalents)
static inline int refl101(int i, int n) {
  if (n == 1) return 0;
  while (i < 0 || i >= n) { if (i < 0) i = -i; if (i >= n) i = 2 * n - 2 - i; }
  return i;
}
// cv2.resize INTER_AREA, uint8, downscale (dsize given)
static void resize_area_u8(const uint8_t* s, int sw, int sh, uint8_t* d, int dw, int dh) {
  const double sx = 1.0 / ((double)dw / sw), sy = 1.0 / ((double)dh / sh);
  const int ix = (int)std::lround(sx), iy = (int)std::lround(sy);
  if (std::fabs(sx - ix) < 2.220446049250313e-16 && std::fabs(sy - iy) < 2.220446049250313e-16 && ix == 2 && iy == 2) {
    for (int y = 0; y < dh; y++) {
      const uint8_t* a = s + (size_t)(2 * y) * sw; const uint8_t* b = a + sw; uint8_t* o = d + (size_t)y * dw;
      for (int x = 0; x < dw; x++) o[x] = (uint8_t)((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
    }
    return;
  }
  if (std::fabs(sx - ix) < 2.220446049250313e-16 && std::fabs(sy - iy) < 2.220446049250313e-16 && ix >= 1 && iy >= 1) {
    const int area = ix * iy; const float sc = 1.f / area;
    for (int y = 0; y < dh; y++)
      for (int x = 0; x < dw; x++) {
        int sum = 0;
        for (int j = 0; j < iy; j++) for (int i = 0; i < ix; i++) sum += s[(size_t)(y * iy + j) * sw + x * ix + i];
        d[(size_t)y * dw + x] = (uint8_t)std::min(255L, std::max(0L, std::lrint(sum * sc)));
      }
    return;
  }
  struct T { int di, si; float a; };
  auto tab = [](int ss, int ds, double scale, std::vector<T>& t) {
    for (int dx = 0; dx < ds; dx++) {
      const double f1 = dx * scale, f2 = f1 + scale, cw = std::min(scale, ss - f1);
      int s1 = (int)std::ceil(f1), s2 = (int)std::floor(f2);
      s2 = std::min(s2, ss - 1); s1 = std::min(s1, s2);
      if (s1 - f1 > 1e-3) t.push_back({dx, s1 - 1, (float)((s1 - f1) / cw)});
      for (int q = s1; q < s2; q++) t.push_back({dx, q, (float)(1.0 / cw)});
      if (f2 - s2 > 1e-3) t.push_back({dx, s2, (float)(std::min(std::min(f2 - s2, 1.), cw) / cw)});
    }
  };
  std::vector<T> xt, yt; tab(sw, dw, sx, xt); tab(sh, dh, sy, yt);
  std::vector<float> buf(dw), sum(dw, 0.f);
  int prev = yt.empty() ? 0 : yt[0].di;
  auto store = [&](int row) { for (int x = 0; x < dw; x++) d[(size_t)row * dw + x] = (uint8_t)std::min(255L, std::max(0L, std::lrint(sum[x]))); };
  for (size_t j = 0; j < yt.size(); j++) {
    const float beta = yt[j].a; const int dy = yt[j].di; const uint8_t* S = s + (size_t)yt[j].si * sw;
    std::fill(buf.begin(), buf.end(), 0.f);
    for (const T& t : xt) buf[t.di] += S[t.si] * t.a;
    if (dy != prev) { store(prev); for (int x = 0; x < dw; x++) sum[x] = buf[x] * beta; prev = dy; }
    else for (int x = 0; x < dw; x++) sum[x] += buf[x] * beta;
  }
  store(prev);
}
// cv2.resize INTER_LINEAR, float32, any size (dsize given)
static void resize_linear_any(const float* s, int sw, int sh, float* d, int dw, int dh) {
  const double scx = 1.0 / ((double)dw / sw), scy = 1.0 / ((double)dh / sh);
  std::vector<int> xo(dw), yo(dh); std::vector<float> xa(dw), ya(dh), xb(dw), yb(dh);
  auto coef = [](int dd, int n, double sc, int& so, float& a0, float& a1) {  // weights from the double position (closest to cv2 on x86)
    double f = (dd + 0.5) * sc - 0.5;
    int q = (int)std::floor(f); f -= q;
    if (q < 0) { f = 0; q = 0; }
    if (q >= n - 1) { f = 0; q = n - 1; }
    so = q; a0 = (float)(1.0 - f); a1 = (float)f;
  };
  for (int x = 0; x < dw; x++) coef(x, sw, scx, xo[x], xb[x], xa[x]);
  for (int y = 0; y < dh; y++) coef(y, sh, scy, yo[y], yb[y], ya[y]);
  std::vector<float> rows((size_t)sh * dw);
  for (int y = 0; y < sh; y++) {
    const float* S = s + (size_t)y * sw; float* D = &rows[(size_t)y * dw];
    for (int x = 0; x < dw; x++) { const int q = xo[x]; const float a1 = xa[x], a0 = xb[x]; D[x] = q + 1 < sw ? S[q] * a0 + S[q + 1] * a1 : S[q] * a0; }
  }
  for (int y = 0; y < dh; y++) {
    const int s0 = yo[y], s1 = std::min(s0 + 1, sh - 1); const float b1 = ya[y], b0 = yb[y];
    const float* R0 = &rows[(size_t)s0 * dw]; const float* R1 = &rows[(size_t)s1 * dw]; float* D = d + (size_t)y * dw;
    for (int x = 0; x < dw; x++) D[x] = R0[x] * b0 + R1[x] * b1;
  }
}
// cv2.spatialGradient (Sobel 3x3, BORDER_REFLECT_101), int16
static void spatial_gradient(const uint8_t* I, int w, int h, int16_t* gx, int16_t* gy) {
  for (int y = 0; y < h; y++) {
    const uint8_t* U = I + (size_t)refl101(y - 1, h) * w; const uint8_t* C = I + (size_t)y * w; const uint8_t* D = I + (size_t)refl101(y + 1, h) * w;
    for (int x = 0; x < w; x++) {
      const int l = refl101(x - 1, w), r = refl101(x + 1, w);
      gx[(size_t)y * w + x] = (int16_t)((U[r] - U[l]) + 2 * (C[r] - C[l]) + (D[r] - D[l]));
      gy[(size_t)y * w + x] = (int16_t)((D[l] - U[l]) + 2 * (D[x] - U[x]) + (D[r] - U[r]));
    }
  }
}

// flow image of one frame (build.flow_img): uint8 (h*k) x (w*k)
static void flow_img(const uint8_t* rgba, int w, int h, int k, bool bgra, std::vector<uint8_t>& out) {
  const int W = w * k, H = h * k;
  std::vector<float> g((size_t)w * h), G((size_t)W * H);
  for (size_t i = 0; i < g.size(); i++) {
    const uint8_t* p = rgba + i * 4;
    const double r = bgra ? p[2] : p[0], gg = p[1], b = bgra ? p[0] : p[2];
    const double L = (r * .299 + gg * .587 + b * .114) / 255.0, a = p[3] / 255.0;
    g[i] = (float)((L * 0.6 + 0.4) * a);
  }
  tmap::resize_linear_f32(g.data(), w, h, k, G.data());
  tmap::gauss_f32(G.data(), W, H, k * 0.6);
  out.resize((size_t)W * H);
  for (size_t i = 0; i < G.size(); i++) {
    float t = G[i] * 255.0f;
    t = t < 0.f ? 0.f : (t > 255.f ? 255.f : t);
    out[i] = (uint8_t)t;
  }
}

// ---------------------------------------------------------------- DIS optical flow (OpenCV PRESET_MEDIUM, CPU path)
static const float DEPS = 0.001f, DINF = 1e10f;
struct DisCfg { int finest = 1, coarsest = 0, ps = 8, stride = 3, gd = 25, vr = 5; };
// call n (0 = first) of one DISOpticalFlow object on W x H images (calc() may move finest_scale for good)
static DisCfg dis_cfg(int W, int H, int call) {
  DisCfg c; int finest_state = 1;
  for (int n = 0; n <= call; n++) {
    c.ps = 8;
    const int mx = std::max(W, H), mn = std::min(W, H);
    const int a = (int)(std::log(mx / (4.0 * 8)) / std::log(2.0) + 0.5), b = (int)(std::log((double)(mn / 8)) / std::log(2.0));
    c.coarsest = std::min(a, b);
    c.finest = finest_state;
    if (c.coarsest < 0) { c.coarsest = -1; return c; }
    if (c.coarsest < c.finest) {
      const float r = (2.0f * (float)W) / ((float)5 * (float)8);
      c.coarsest = std::max(0, (int)std::floor(std::log2(r)));
      c.finest = std::max(c.coarsest - 2, 0);
      finest_state = c.finest;
    }
  }
  return c;
}
struct DisLevel {  // frame-0 side of one pyramid level (shared by all frames of a sprite)
  int w = 0, h = 0, ws = 0, hs = 0;
  std::vector<uint8_t> I0; std::vector<int16_t> Ix, Iy;
  std::vector<float> xx, yy, xy, sx, sy;
};
static void structure_tensor(DisLevel& L, int ps, int st) {
  const int w = L.w, h = L.h, ws = L.ws;
  std::vector<float> axx((size_t)h * ws), ayy(axx.size()), axy(axx.size()), ax(axx.size()), ay(axx.size());
  for (int i = 0; i < h; i++) {
    float sxx = 0, syy = 0, sxy = 0, sx = 0, sy = 0;
    const int16_t* xr = &L.Ix[(size_t)i * w]; const int16_t* yr = &L.Iy[(size_t)i * w];
    for (int j = 0; j < ps; j++) { sxx += xr[j] * xr[j]; syy += yr[j] * yr[j]; sxy += xr[j] * yr[j]; sx += xr[j]; sy += yr[j]; }
    axx[(size_t)i * ws] = sxx; ayy[(size_t)i * ws] = syy; axy[(size_t)i * ws] = sxy; ax[(size_t)i * ws] = sx; ay[(size_t)i * ws] = sy;
    int js = 1;
    for (int j = ps; j < w; j++) {
      sxx += (xr[j] * xr[j] - xr[j - ps] * xr[j - ps]); syy += (yr[j] * yr[j] - yr[j - ps] * yr[j - ps]);
      sxy += (xr[j] * yr[j] - xr[j - ps] * yr[j - ps]); sx += (xr[j] - xr[j - ps]); sy += (yr[j] - yr[j - ps]);
      if ((j - ps + 1) % st == 0 && js < ws) {
        const size_t o = (size_t)i * ws + js; axx[o] = sxx; ayy[o] = syy; axy[o] = sxy; ax[o] = sx; ay[o] = sy; js++;
      }
    }
  }
  const size_t n = (size_t)L.hs * ws;
  L.xx.assign(n, 0); L.yy.assign(n, 0); L.xy.assign(n, 0); L.sx.assign(n, 0); L.sy.assign(n, 0);
  std::vector<float> s1(ws, 0), s2(ws, 0), s3(ws, 0), s4(ws, 0), s5(ws, 0);
  for (int i = 0; i < ps; i++)
    for (int j = 0; j < ws; j++) { const size_t o = (size_t)i * ws + j; s1[j] += axx[o]; s2[j] += ayy[o]; s3[j] += axy[o]; s4[j] += ax[o]; s5[j] += ay[o]; }
  for (int j = 0; j < ws; j++) { L.xx[j] = s1[j]; L.yy[j] = s2[j]; L.xy[j] = s3[j]; L.sx[j] = s4[j]; L.sy[j] = s5[j]; }
  int is = 1;
  for (int i = ps; i < h; i++) {
    for (int j = 0; j < ws; j++) {
      const size_t o = (size_t)i * ws + j, q = (size_t)(i - ps) * ws + j;
      s1[j] += (axx[o] - axx[q]); s2[j] += (ayy[o] - ayy[q]); s3[j] += (axy[o] - axy[q]); s4[j] += (ax[o] - ax[q]); s5[j] += (ay[o] - ay[q]);
    }
    if ((i - ps + 1) % st == 0 && is < L.hs) {
      for (int j = 0; j < ws; j++) { const size_t o = (size_t)is * ws + j; L.xx[o] = s1[j]; L.yy[o] = s2[j]; L.xy[o] = s3[j]; L.sx[o] = s4[j]; L.sy[o] = s5[j]; }
      is++;
    }
  }
}
struct DisRef {  // frame-0 pyramid for one DisCfg
  DisCfg c; int W = 0, H = 0; std::vector<DisLevel> lv;  // index = scale
  bool ok() const { return c.coarsest >= 0 && !lv.empty(); }
};
static void level_sizes(const DisCfg& c, int W, int H, std::vector<std::pair<int, int>>& sz) {
  sz.assign(c.coarsest + 1, {0, 0});
  int f = 1 << c.finest;
  sz[c.finest] = {W / f, H / f};
  for (int i = c.finest + 1; i <= c.coarsest; i++) sz[i] = {sz[i - 1].first / 2, sz[i - 1].second / 2};
}
static void pyramid(const uint8_t* I, int W, int H, const DisCfg& c, std::vector<std::vector<uint8_t>>& out) {
  std::vector<std::pair<int, int>> sz; level_sizes(c, W, H, sz);
  out.assign(c.coarsest + 1, {});
  for (int i = c.finest; i <= c.coarsest; i++) {
    out[i].resize((size_t)sz[i].first * sz[i].second);
    if (i == c.finest) resize_area_u8(I, W, H, out[i].data(), sz[i].first, sz[i].second);
    else resize_area_u8(out[i - 1].data(), sz[i - 1].first, sz[i - 1].second, out[i].data(), sz[i].first, sz[i].second);
  }
}
static void dis_ref(const uint8_t* I0, int W, int H, const DisCfg& c, DisRef& R) {
  R.c = c; R.W = W; R.H = H; R.lv.clear();
  if (c.coarsest < 0) return;
  std::vector<std::vector<uint8_t>> p; pyramid(I0, W, H, c, p);
  std::vector<std::pair<int, int>> sz; level_sizes(c, W, H, sz);
  R.lv.resize(c.coarsest + 1);
  for (int i = c.finest; i <= c.coarsest; i++) {
    DisLevel& L = R.lv[i];
    L.w = sz[i].first; L.h = sz[i].second; L.I0.swap(p[i]);
    L.ws = 1 + (L.w - c.ps) / c.stride; L.hs = 1 + (L.h - c.ps) / c.stride;
    L.Ix.resize(L.I0.size()); L.Iy.resize(L.I0.size());
    spatial_gradient(L.I0.data(), L.w, L.h, L.Ix.data(), L.Iy.data());
    structure_tensor(L, c.ps, c.stride);
  }
}

// variational refinement (cv::VariationalRefinement as configured by DIS), plain grid form of the red-black solver
struct Varref {
  int w = 0, h = 0;
  std::vector<float> Ix, Iy, Iz, Ixx, Ixy, Iyy, Ixz, Iyz, A11, A12, A22, b1, b2, wt, dU, dV, tU, tV, Wu, Wv, avg, warp;
  std::vector<float> rbW[2], rbU[2], rbV[2], rbA11[2], rbA12[2], rbA22[2], rbB1[2], rbB2[2];
  void sobel_x(const float* s, float* d) const {
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { const float* r = s + (size_t)y * w; d[(size_t)y * w + x] = r[std::min(x + 1, w - 1)] - r[std::max(x - 1, 0)]; }
  }
  void sobel_y(const float* s, float* d) const {
    for (int y = 0; y < h; y++) {
      const float* u = s + (size_t)std::max(y - 1, 0) * w; const float* dn = s + (size_t)std::min(y + 1, h - 1) * w;
      for (int x = 0; x < w; x++) d[(size_t)y * w + x] = dn[x] - u[x];
    }
  }
  void run(const uint8_t* I0, const uint8_t* I1, int w_, int h_, float* U, float* V, int fp_iter) {
    w = w_; h = h_; const size_t N = (size_t)w * h;
    for (auto* v : {&Ix, &Iy, &Iz, &Ixx, &Ixy, &Iyy, &Ixz, &Iyz, &A11, &A12, &A22, &b1, &b2, &wt, &dU, &dV, &tU, &tV, &Wu, &Wv, &avg, &warp}) v->assign(N, 0.f);
    // warp I1 by the flow (cv2.remap INTER_LINEAR, BORDER_REPLICATE; OpenCV 5 interpolates float coordinates)
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        const size_t o = (size_t)y * w + x;
        const float fx = x + U[o], fy = y + V[o];
        const float flx = std::floor(fx), fly = std::floor(fy);
        const int sx = (int)flx, sy = (int)fly;
        const float ax = fx - flx, ay = fy - fly;
        const int x0 = std::min(std::max(sx, 0), w - 1), x1 = std::min(std::max(sx + 1, 0), w - 1);
        const int y0 = std::min(std::max(sy, 0), h - 1), y1 = std::min(std::max(sy + 1, 0), h - 1);
        const float a00 = I1[(size_t)y0 * w + x0], a01 = I1[(size_t)y0 * w + x1], a10 = I1[(size_t)y1 * w + x0], a11 = I1[(size_t)y1 * w + x1];
        const float p = a00 + (a01 - a00) * ax, q = a10 + (a11 - a10) * ax;
        const float v = p + (q - p) * ay;
        warp[o] = v;
        avg[o] = (float)I0[o] * 0.5f + v * 0.5f;
        Iz[o] = v - (float)I0[o];
      }
    sobel_x(avg.data(), Ix.data()); sobel_y(avg.data(), Iy.data());
    sobel_x(Iz.data(), Ixz.data()); sobel_y(Iz.data(), Iyz.data());
    sobel_x(Ix.data(), Ixx.data()); sobel_y(Ix.data(), Ixy.data()); sobel_y(Iy.data(), Iyy.data());
    std::memcpy(Wu.data(), U, N * 4); std::memcpy(Wv.data(), V, N * 4);
    tU = Wu; tV = Wv;
    const float alpha = 20.f, gamma = 10.f, delta = 5.f, eps = 0.01f, omega = 1.6f, zeta = 0.1f;
    const float z2 = zeta * zeta, e2 = eps * eps, g2 = gamma / 2, d2 = delta / 2, a2 = alpha / 2;
    // red-black SOR on split rows: colour c of row y holds x = ((y + c) & 1) + 2m at column m + 1 (columns 0 and last stay 0)
    const int cwp = (w + 1) / 2 + 2;
    for (int c = 0; c < 2; c++) for (auto* v : {&rbW[c], &rbU[c], &rbV[c], &rbA11[c], &rbA12[c], &rbA22[c], &rbB1[c], &rbB2[c]}) v->assign((size_t)(h + 2) * cwp, 0.f);
    auto rb = [&](int, int y, int m) { return (size_t)(y + 1) * cwp + m + 1; };
    auto split = [&](const std::vector<float>& src, std::vector<float>* dst) {
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) { const int c = (x + y) & 1; dst[c][rb(c, y, x >> 1)] = src[(size_t)y * w + x]; }
    };
    for (int it = 0; it < fp_iter; it++) {
      for (size_t o = 0; o < N; o++) {
        const float ix = Ix[o], iy = Iy[o], iz = Iz[o], du = dU[o], dv = dV[o];
        float dn = ix * ix + iy * iy + z2;
        const float ik = iz + ix * du + iy * dv;
        float wgt = (d2 / std::sqrt(ik * ik / dn + e2)) / dn;
        float a11 = wgt * (ix * ix) + z2, a12 = wgt * (ix * iy), a22 = wgt * (iy * iy) + z2;
        float c1 = -wgt * (iz * ix), c2 = -wgt * (iz * iy);
        const float ixx = Ixx[o], ixy = Ixy[o], iyy = Iyy[o], ixz = Ixz[o], iyz = Iyz[o];
        dn = ixx * ixx + ixy * ixy + z2;
        const float dn2 = iyy * iyy + ixy * ixy + z2;
        const float kx = ixz + ixx * du + ixy * dv, ky = iyz + ixy * du + iyy * dv;
        wgt = g2 / std::sqrt(kx * kx / dn + ky * ky / dn2 + e2);
        a11 += wgt * (ixx * ixx / dn + ixy * ixy / dn2);
        a12 += wgt * (ixx * ixy / dn + ixy * iyy / dn2);
        a22 += wgt * (ixy * ixy / dn + iyy * iyy / dn2);
        c1 += -wgt * (ixx * ixz / dn + ixy * iyz / dn2);
        c2 += -wgt * (ixy * ixz / dn + iyy * iyz / dn2);
        A11[o] = a11; A12[o] = a12; A22[o] = a22; b1[o] = c1; b2[o] = c2;
      }
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
          const size_t o = (size_t)y * w + x, r = x + 1 < w ? o + 1 : o, d = y + 1 < h ? o + w : o;
          const float ux = tU[r] - tU[o], vx = tV[r] - tV[o], uy = tU[d] - tU[o], vy = tV[d] - tV[o];
          wt[o] = a2 / std::sqrt(ux * ux + vx * vx + uy * uy + vy * vy + e2);
        }
      // smoothness terms in the order the red pass / black pass add them
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
          const size_t o = (size_t)y * w + x;
          const bool red = ((x + y) & 1) == 0, hr = x + 1 < w, hl = x > 0, hd = y + 1 < h, hu = y > 0;
          float a11 = A11[o], a22 = A22[o], c1 = b1[o], c2 = b2[o];
          const float wo = wt[o];
          auto own_r = [&]() { if (hr) { c1 += wo * (Wu[o + 1] - Wu[o]); a11 += wo; c2 += wo * (Wv[o + 1] - Wv[o]); a22 += wo; } };
          auto from_l = [&]() { if (hl) { const float wl = wt[o - 1]; c1 -= wl * (Wu[o] - Wu[o - 1]); a11 += wl; c2 -= wl * (Wv[o] - Wv[o - 1]); a22 += wl; } };
          auto own_d = [&]() { if (hd) { c1 += wo * (Wu[o + w] - Wu[o]); a11 += wo; c2 += wo * (Wv[o + w] - Wv[o]); a22 += wo; } };
          auto from_u = [&]() { if (hu) { const float wu = wt[o - w]; c1 -= wu * (Wu[o] - Wu[o - w]); a11 += wu; c2 -= wu * (Wv[o] - Wv[o - w]); a22 += wu; } };
          if (red) { own_r(); from_l(); own_d(); from_u(); } else { from_l(); own_r(); from_u(); own_d(); }
          A11[o] = a11; A22[o] = a22; b1[o] = c1; b2[o] = c2;
        }
      split(wt, rbW); split(dU, rbU); split(dV, rbV); split(A11, rbA11); split(A12, rbA12); split(A22, rbA22); split(b1, rbB1); split(b2, rbB2);
      for (int s = 0; s < 5; s++)
        for (int c = 0; c < 2; c++) {
          const int oc = 1 - c;
          for (int y = 0; y < h; y++) {
            const int x0 = (y + c) & 1, cnt = (w - x0 + 1) / 2, sh = x0;  // odd x: left/right neighbours sit one column further
            float* pu = &rbU[c][rb(c, y, 0)]; float* pv = &rbV[c][rb(c, y, 0)];
            const float* pw = &rbW[c][rb(c, y, 0)];
            const float* a11 = &rbA11[c][rb(c, y, 0)]; const float* a12 = &rbA12[c][rb(c, y, 0)]; const float* a22 = &rbA22[c][rb(c, y, 0)];
            const float* q1 = &rbB1[c][rb(c, y, 0)]; const float* q2 = &rbB2[c][rb(c, y, 0)];
            const float* nu = &rbU[oc][rb(oc, y, 0)] - 1 + sh; const float* nv = &rbV[oc][rb(oc, y, 0)] - 1 + sh;
            const float* nw = &rbW[oc][rb(oc, y, 0)] - 1 + sh;
            const float* uu = &rbU[oc][rb(oc, y - 1, 0)]; const float* uv = &rbV[oc][rb(oc, y - 1, 0)]; const float* uw = &rbW[oc][rb(oc, y - 1, 0)];
            const float* du = &rbU[oc][rb(oc, y + 1, 0)]; const float* dv = &rbV[oc][rb(oc, y + 1, 0)];
            for (int m = 0; m < cnt; m++) {
              const float wc = pw[m];
              const float su = nw[m] * nu[m] + wc * nu[m + 1] + uw[m] * uu[m] + wc * du[m];
              const float sv = nw[m] * nv[m] + wc * nv[m + 1] + uw[m] * uv[m] + wc * dv[m];
              const float nu_ = pu[m] + omega * ((su + q1[m] - pv[m] * a12[m]) / a11[m] - pu[m]);
              pu[m] = nu_;
              pv[m] = pv[m] + omega * ((sv + q2[m] - nu_ * a12[m]) / a22[m] - pv[m]);
            }
          }
        }
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) { const int c = (x + y) & 1; const size_t o = (size_t)y * w + x; dU[o] = rbU[c][rb(c, y, x >> 1)]; dV[o] = rbV[c][rb(c, y, x >> 1)]; }
      for (size_t o = 0; o < N; o++) { tU[o] = Wu[o] + dU[o]; tV[o] = Wv[o] + dV[o]; }
    }
    std::memcpy(U, tU.data(), N * 4); std::memcpy(V, tV.data(), N * 4);
  }
};

// per-thread buffers for one frame's flow
struct DisWork {
  std::vector<std::vector<uint8_t>> I1;
  std::vector<uint8_t> ext;
  std::vector<float> Ux, Uy, Nx, Ny, Sx, Sy;
  Varref vr;
};
// 8x8 patch sums in the lane order of OpenCV's SIMD128 path (4 lanes, left/right half rows, (l0+l1)+(l2+l3)).
// GCC/clang vector extension: each lane is the same IEEE float operation as scalar code.
typedef float v4f __attribute__((vector_size(16)));
typedef int32_t v4i __attribute__((vector_size(16)));
static inline v4f ld8(const uint8_t* p) { v4i t = {p[0], p[1], p[2], p[3]}; return __builtin_convertvector(t, v4f); }
static inline v4f ld16(const int16_t* p) { v4i t = {p[0], p[1], p[2], p[3]}; return __builtin_convertvector(t, v4f); }
static inline float red4(v4f v) { return (v[0] + v[1]) + (v[2] + v[3]); }
static inline float ssd_mn(const uint8_t* I0, const uint8_t* I1, int s0, int s1, float w00, float w01, float w10, float w11) {
  const v4f W00 = {w00, w00, w00, w00}, W01 = {w01, w01, w01, w01}, W10 = {w10, w10, w10, w10}, W11 = {w11, w11, w11, w11};
  v4f sd = {0, 0, 0, 0}, sq = {0, 0, 0, 0};
  for (int i = 0; i < 8; i++) {
    const uint8_t* a = I1 + i * s1; const uint8_t* b = a + s1; const uint8_t* z = I0 + i * s0;
    const v4f dl = W00 * ld8(a) + W01 * ld8(a + 1) + W10 * ld8(b) + W11 * ld8(b + 1) - ld8(z);
    const v4f dr = W00 * ld8(a + 4) + W01 * ld8(a + 5) + W10 * ld8(b + 4) + W11 * ld8(b + 5) - ld8(z + 4);
    sq = sq + (dl * dl + dr * dr); sd = sd + (dl + dr);
  }
  const float SD = red4(sd), SQ = red4(sq);
  return SQ - SD * SD / 64.0f;
}
static inline float patch_mn(float& dux, float& duy, const uint8_t* I0, const uint8_t* I1, const int16_t* gx, const int16_t* gy, int s0, int s1,
                             float w00, float w01, float w10, float w11, float xs, float ys) {
  const v4f W00 = {w00, w00, w00, w00}, W01 = {w01, w01, w01, w01}, W10 = {w10, w10, w10, w10}, W11 = {w11, w11, w11, w11};
  v4f sd = {0, 0, 0, 0}, sq = {0, 0, 0, 0}, mx = {0, 0, 0, 0}, my = {0, 0, 0, 0};
  for (int i = 0; i < 8; i++) {
    const uint8_t* a = I1 + i * s1; const uint8_t* b = a + s1; const uint8_t* z = I0 + i * s0;
    const int16_t* X = gx + i * s0; const int16_t* Y = gy + i * s0;
    const v4f dl = W00 * ld8(a) + W01 * ld8(a + 1) + W10 * ld8(b) + W11 * ld8(b + 1) - ld8(z);
    const v4f dr = W00 * ld8(a + 4) + W01 * ld8(a + 5) + W10 * ld8(b + 4) + W11 * ld8(b + 5) - ld8(z + 4);
    mx = mx + (dl * ld16(X) + dr * ld16(X + 4));
    my = my + (dl * ld16(Y) + dr * ld16(Y + 4));
    sq = sq + (dl * dl + dr * dr); sd = sd + (dl + dr);
  }
  const float SD = red4(sd), SQ = red4(sq), MX = red4(mx), MY = red4(my);
  dux = MX - SD * xs / 64.0f; duy = MY - SD * ys / 64.0f;
  return SQ - SD * SD / 64.0f;
}
static void inverse_search(const DisCfg& c, const DisLevel& L, const uint8_t* I1e, const float* Ux, const float* Uy, float* Sx, float* Sy) {
  const int psz = c.ps, psz2 = psz / 2, bsz = 16, w = L.w, h = L.h, ws = L.ws, hs = L.hs, wext = w + 2 * bsz;
  const float il = std::max(bsz - psz + 1.0f, 0.0f), iu = std::min(bsz + h - 1.0f, h + 2.0f * bsz - 1.0f - psz);
  const float jl = std::max(bsz - psz + 1.0f, 0.0f), ju = std::min(bsz + w - 1.0f, w + 2.0f * bsz - 1.0f - psz);
  const int num_iter = 2, inner = (int)std::floor(c.gd / (float)num_iter), nstripes = 8;
  const int stripe = (int)std::ceil(hs / (double)nstripes);
  for (int sn = 0; sn < nstripes; sn++) {
    for (int iter = 0; iter < num_iter; iter++) {
      int dir, sis, eis, sjs, ejs, si, sj;
      if (iter % 2 == 0) { dir = 1; sis = std::min(sn * stripe, hs); eis = std::min((sn + 1) * stripe, hs); sjs = 0; ejs = ws; si = sis * c.stride; sj = 0; }
      else { dir = -1; sis = std::min((sn + 1) * stripe, hs) - 1; eis = std::min(sn * stripe, hs) - 1; sjs = ws - 1; ejs = -1; si = sis * c.stride; sj = (ws - 1) * c.stride; }
      int i = si;
      for (int is = sis; dir * is < dir * eis; is += dir) {
        int j = sj;
        for (int js = sjs; dir * js < dir * ejs; js += dir) {
          const size_t o = (size_t)is * ws + js;
          if (iter == 0) { Sx[o] = Ux[(size_t)(i + psz2) * w + j + psz2]; Sy[o] = Uy[(size_t)(i + psz2) * w + j + psz2]; }
          float w00, w01, w10, w11; float iI, jI;
          auto weights = [&](float ux, float uy) {
            iI = std::min(std::max(i + uy + bsz, il), iu); jI = std::min(std::max(j + ux + bsz, jl), ju);
            const float di = iI - std::floor(iI), dj = jI - std::floor(jI);
            w11 = di * dj; w10 = di * (1 - dj); w01 = (1 - di) * dj; w00 = (1 - di) * (1 - dj);
          };
          auto ssd = [&](float ux, float uy) {
            weights(ux, uy);
            return ssd_mn(&L.I0[(size_t)i * w + j], I1e + (size_t)(int)iI * wext + (int)jI, w, wext, w00, w01, w10, w11);
          };
          float mins = ssd(Sx[o], Sy[o]), cur;
          if (dir * js > dir * sjs) {
            cur = ssd(Sx[o - dir], Sy[o - dir]);
            if (cur < mins) { mins = cur; Sx[o] = Sx[o - dir]; Sy[o] = Sy[o - dir]; }
          }
          if (dir * is > dir * sis) {
            const size_t q = (size_t)(is - dir) * ws + js;
            cur = ssd(Sx[q], Sy[q]);
            if (cur < mins) { mins = cur; Sx[o] = Sx[q]; Sy[o] = Sy[q]; }
          }
          float cx = Sx[o], cy = Sy[o];
          float det = L.xx[o] * L.yy[o] - L.xy[o] * L.xy[o];
          if (std::fabs(det) < DEPS) det = DEPS;
          const float h11 = L.yy[o] / det, h12 = -L.xy[o] / det, h22 = L.xx[o] / det;
          float prev = DINF;
          for (int t = 0; t < inner; t++) {
            weights(cx, cy);
            float dux, duy;
            const float s = patch_mn(dux, duy, &L.I0[(size_t)i * w + j], I1e + (size_t)(int)iI * wext + (int)jI, &L.Ix[(size_t)i * w + j], &L.Iy[(size_t)i * w + j],
                                     w, wext, w00, w01, w10, w11, L.sx[o], L.sy[o]);
            const float dx = h11 * dux + h12 * duy, dy = h12 * dux + h22 * duy;
            cx -= dx; cy -= dy;
            if (s >= prev) break;
            prev = s;
          }
          const float ex = cx - Sx[o], ey = cy - Sy[o];
          if (std::sqrt(ex * ex + ey * ey) <= (float)psz) { Sx[o] = cx; Sy[o] = cy; }
          j += dir * c.stride;
        }
        i += dir * c.stride;
      }
    }
  }
}
static void densify(const DisCfg& c, const DisLevel& L, const uint8_t* I1, const float* Sx, const float* Sy, float* Ux, float* Uy) {
  const int psz = c.ps, pstr = c.stride, w = L.w, h = L.h, ws = L.ws;
  int sis = 0, eis = -1;
  for (int i = 0; i < h; i++) {
    if (i % pstr == 0 && i + psz <= h) eis++;
    if (i - psz >= 0 && (i - psz) % pstr == 0 && sis < eis) sis++;
    int sjs = 0, ejs = -1;
    for (int j = 0; j < w; j++) {
      if (j % pstr == 0 && j + psz <= w) ejs++;
      if (j - psz >= 0 && (j - psz) % pstr == 0 && sjs < ejs) sjs++;
      float sc = 0.f, su = 0.f, sv = 0.f;
      for (int is = sis; is <= eis; is++)
        for (int js = sjs; js <= ejs; js++) {
          const float sx = Sx[(size_t)is * ws + js], sy = Sy[(size_t)is * ws + js];
          const float jm = std::min(std::max(j + sx, 0.0f), w - 1.0f - DEPS), im = std::min(std::max(i + sy, 0.0f), h - 1.0f - DEPS);
          const int jl_ = (int)jm, ju_ = jl_ + 1, il_ = (int)im, iu_ = il_ + 1;
          const float diff = (jm - jl_) * (im - il_) * I1[(size_t)iu_ * w + ju_] + (ju_ - jm) * (im - il_) * I1[(size_t)iu_ * w + jl_] +
                             (jm - jl_) * (iu_ - im) * I1[(size_t)il_ * w + ju_] + (ju_ - jm) * (iu_ - im) * I1[(size_t)il_ * w + jl_] - L.I0[(size_t)i * w + j];
          const float coef = 1 / std::max(1.0f, std::fabs(diff));
          su += coef * sx; sv += coef * sy; sc += coef;
        }
      Ux[(size_t)i * w + j] = su / sc; Uy[(size_t)i * w + j] = sv / sc;
    }
  }
}
// flow of frame I1 against the reference pyramid; out = finest-level U,V (size of R.lv[finest]); returns false on bad sizes
static bool dis_calc(const DisRef& R, const uint8_t* I1, DisWork& K, const std::atomic<bool>* cancel = nullptr) {
  const DisCfg& c = R.c;
  if (!R.ok()) return false;
  pyramid(I1, R.W, R.H, c, K.I1);
  for (int i = c.coarsest; i >= c.finest; i--) {
    if (cancel && cancel->load(std::memory_order_relaxed)) return false;
    const DisLevel& L = R.lv[i];
    const int w = L.w, h = L.h, b = 16, we = w + 2 * b;
    if (L.ws <= 0 || L.hs <= 0) return false;
    K.ext.resize((size_t)we * (h + 2 * b));
    for (int y = 0; y < h + 2 * b; y++) {
      const uint8_t* s = &K.I1[i][(size_t)std::min(std::max(y - b, 0), h - 1) * w]; uint8_t* d = &K.ext[(size_t)y * we];
      for (int x = 0; x < we; x++) d[x] = s[std::min(std::max(x - b, 0), w - 1)];
    }
    if (i == c.coarsest) { K.Ux.assign((size_t)w * h, 0.f); K.Uy.assign((size_t)w * h, 0.f); }
    K.Sx.assign((size_t)L.ws * L.hs, 0.f); K.Sy.assign(K.Sx.size(), 0.f);
    inverse_search(c, L, K.ext.data(), K.Ux.data(), K.Uy.data(), K.Sx.data(), K.Sy.data());
    densify(c, L, K.I1[i].data(), K.Sx.data(), K.Sy.data(), K.Ux.data(), K.Uy.data());
    if (c.vr > 0) K.vr.run(L.I0.data(), K.I1[i].data(), w, h, K.Ux.data(), K.Uy.data(), c.vr);
    if (i > c.finest) {
      const DisLevel& N = R.lv[i - 1];
      K.Nx.resize((size_t)N.w * N.h); K.Ny.resize(K.Nx.size());
      resize_linear_any(K.Ux.data(), w, h, K.Nx.data(), N.w, N.h);
      resize_linear_any(K.Uy.data(), w, h, K.Ny.data(), N.w, N.h);
      for (size_t q = 0; q < K.Nx.size(); q++) { K.Nx[q] *= 2; K.Ny[q] *= 2; }
      K.Ux.swap(K.Nx); K.Uy.swap(K.Ny);
    }
  }
  return true;
}

// ---------------------------------------------------------------- the sprite: mesh on frame 0, vertex tracks per frame
static const int TRK_MAX_V = 65000;
static const int64_t TRK_MAX_PX = 1 << 21;  // flow image pixels (about 50 bytes each per worker)
struct Sprite {
  int fw = 0, fh = 0, k = 0, nf = 0, W = 0, H = 0; bool bgra = false;
  double face_k = 1.0;                  // facet spacing scale (1.0 = v3 spacing)
  std::vector<uint8_t> px;              // nf frames, frame-major, fw*fh*4 each
  std::vector<float> p0; std::vector<int32_t> tri; int nv = 0, nt = 0;
  std::vector<int> vx, vy;              // rounded, clipped vertex pixel
  std::vector<uint8_t> g0;
  DisRef ref0, ref1;                    // first call / later calls of the one DIS object
  std::vector<float> kern; int kr = 0;  // cv2 Gaussian kernel for the flow blur
  std::vector<int16_t> dv;              // nf * nv * 2 (1/16 px, minus p0)
  std::vector<uint8_t> okb;             // nf * ((nt+7)/8)
  const uint8_t* frame(int i) const { return &px[(size_t)i * fw * fh * 4]; }
  // 0 ok; 21 empty body, 22 too many vertices, 23 triangulation failed, 24 too small for the flow, 26 too large
  int prepare() {
    W = fw * k; H = fh * k;
    if ((int64_t)W * H > TRK_MAX_PX) return 26;
    const size_t n1 = (size_t)fw * fh;
    std::vector<uint8_t> uni(n1, 0);
    for (int f = 0; f < nf; f++) { const uint8_t* p = frame(f); for (size_t i = 0; i < n1; i++) if (p[i * 4 + 3]) uni[i] = 1; }
    long area = 0; for (uint8_t u : uni) area += u;
    if (area == 0) return 21;
    const double spacing = 0.12 * std::sqrt((double)area) * face_k;
    const double s = spacing * k;
    if (!(s >= 1.0)) return 22;
    const double est = ((W + 2 * s) / s + 2) * ((H + 2 * s) / (s * 0.87) + 2) + 400;
    if (est > TRK_MAX_V) return 22;
    Pcg64 rng;
    std::vector<double> pts;
    double y = -s; int row = 0;
    while (y < H + s) {
      double x = -s + (row % 2 ? s * 0.5 : 0);
      while (x < W + s) {
        const double ax = x + rng.uniform(-.38, .38) * s;
        const double ay = y + rng.uniform(-.38, .38) * s;
        pts.push_back(ax); pts.push_back(ay);
        x += s;
      }
      y += s * 0.87; row++;
    }
    {
      std::vector<float> a1(n1), m((size_t)W * H);
      const uint8_t* f0 = frame(0);
      for (size_t i = 0; i < n1; i++) a1[i] = f0[i * 4 + 3] ? 1.f : 0.f;
      tmap::resize_linear_f32(a1.data(), fw, fh, k, m.data());
      tmap::gauss_f32(m.data(), W, H, k * 0.45);
      std::vector<uint8_t> mb((size_t)W * H);
      for (size_t i = 0; i < mb.size(); i++) mb[i] = m[i] > 0.5f;
      std::vector<double> dt((size_t)W * H);
      tmap::edt(mb.data(), W, H, dt.data());
      std::vector<int64_t> cand;
      const double lim = s * 0.45;
      for (size_t i = 0; i < mb.size(); i++) if (mb[i] && dt[i] < lim) cand.push_back((int64_t)i);
      if (!cand.empty()) {
        const int64_t q = std::max<int64_t>(1, (int64_t)(s * s * 0.9));
        const int64_t npick = std::min<int64_t>((int64_t)cand.size() / q + 1, 400);
        std::vector<int64_t> pick = rng.choice((int64_t)cand.size(), npick);
        for (int64_t pi : pick) {
          const int64_t c = cand[pi]; const int64_t py = c / W, pxx = c % W;
          const double ax = (double)pxx + rng.uniform(-.5, .5);
          const double ay = (double)py + rng.uniform(-.5, .5);
          pts.push_back(ax); pts.push_back(ay);
        }
      }
    }
    nv = (int)(pts.size() / 2);
    if (nv >= TRK_MAX_V) return 22;
    p0.resize(pts.size());
    for (size_t i = 0; i < pts.size(); i++) p0[i] = (float)pts[i];
    for (int i = 0; i < nv * 2; i++) if (!(std::fabs(p0[i] * 16.0f) < 32767.0f)) return 22;
    Delaunay D;
    if (D.run(p0.data(), nv) != 0) return 23;
    {  // canonical order: each triangle as ascending vertex ids, triangles sorted (pack.py can apply the same rule)
      std::vector<std::array<int32_t, 3>> T(D.tri.size() / 3);
      for (size_t t = 0; t < T.size(); t++) { T[t] = {D.tri[3 * t], D.tri[3 * t + 1], D.tri[3 * t + 2]}; std::sort(T[t].begin(), T[t].end()); }
      std::sort(T.begin(), T.end());
      tri.resize(T.size() * 3);
      for (size_t t = 0; t < T.size(); t++) { tri[3 * t] = T[t][0]; tri[3 * t + 1] = T[t][1]; tri[3 * t + 2] = T[t][2]; }
    }
    nt = (int)(tri.size() / 3);
    if (nt <= 0 || nt >= 65536) return 23;
    vx.resize(nv); vy.resize(nv);
    for (int i = 0; i < nv; i++) {
      const float rx = std::nearbyint(p0[2 * i]), ry = std::nearbyint(p0[2 * i + 1]);
      vx[i] = (int)std::min(std::max(rx, 0.f), (float)(W - 1)); vy[i] = (int)std::min(std::max(ry, 0.f), (float)(H - 1));
    }
    flow_img(frame(0), fw, fh, k, bgra, g0);
    const DisCfg c0 = dis_cfg(W, H, 0), c1 = dis_cfg(W, H, 1);
    if (c0.coarsest < 0 || c1.coarsest < 0) return 24;
    dis_ref(g0.data(), W, H, c0, ref0);
    if (c1.finest == c0.finest && c1.coarsest == c0.coarsest) ref1 = ref0; else dis_ref(g0.data(), W, H, c1, ref1);
    const double sg = k * 1.2;
    const int n = ((int)std::lround(sg * 4 * 2 + 1)) | 1;
    kern.resize(n); kr = n / 2;
    const double s2 = -0.5 / (sg * sg); double sum = 0;
    for (int i = 0; i < n; i++) { const double x = i - (n - 1) * 0.5; kern[i] = (float)std::exp(s2 * x * x); sum += kern[i]; }
    sum = 1. / sum;
    for (int i = 0; i < n; i++) kern[i] = (float)(kern[i] * sum);
    dv.assign((size_t)nf * nv * 2, 0); okb.assign((size_t)nf * ((nt + 7) / 8), 0);
    return 0;
  }
  // track one frame (thread-safe for distinct f)
  int track(int f, DisWork& K, const std::atomic<bool>* cancel = nullptr) {
    std::vector<uint8_t> g;
    flow_img(frame(f), fw, fh, k, bgra, g);
    const DisRef& R = f == 0 ? ref0 : ref1;
    if (!dis_calc(R, g.data(), K, cancel)) return 25;
    const DisLevel& L = R.lv[R.c.finest];
    std::vector<float> F[2];
    for (int ch = 0; ch < 2; ch++) {
      F[ch].resize((size_t)W * H);
      resize_linear_any(ch ? K.Uy.data() : K.Ux.data(), L.w, L.h, F[ch].data(), W, H);
      const float m = (float)(1 << R.c.finest);
      for (float& v : F[ch]) v *= m;
    }
    std::vector<float> v((size_t)nv * 2), rowv(2 * kr + 1);
    for (int i = 0; i < nv; i++) {
      for (int ch = 0; ch < 2; ch++) {
        const float* S = F[ch].data();
        for (int a = -kr; a <= kr; a++) {
          const float* r = S + (size_t)refl101(vy[i] + a, H) * W;
          float acc = 0.f;
          for (int b = -kr; b <= kr; b++) acc += kern[b + kr] * r[refl101(vx[i] + b, W)];
          rowv[a + kr] = acc;
        }
        float acc = 0.f;
        for (int a = 0; a <= 2 * kr; a++) acc += kern[a] * rowv[a];
        v[2 * i + ch] = p0[2 * i + ch] + acc;
      }
    }
    int16_t* d = &dv[(size_t)f * nv * 2];
    for (int i = 0; i < nv * 2; i++) {
      const long q = std::lrint(std::nearbyint(p0[i] * 16.0f)), r = std::lrint(std::nearbyint(v[i] * 16.0f));
      long dd = r - q; dd = std::min(32767L, std::max(-32768L, dd));
      d[i] = (int16_t)dd;
    }
    uint8_t* ok = &okb[(size_t)f * ((nt + 7) / 8)];
    for (int t = 0; t < nt; t++) {
      auto area = [&](const float* P) {
        const float ax = P[2 * tri[3 * t]], ay = P[2 * tri[3 * t] + 1], bx = P[2 * tri[3 * t + 1]], by = P[2 * tri[3 * t + 1] + 1];
        const float cx = P[2 * tri[3 * t + 2]], cy = P[2 * tri[3 * t + 2] + 1];
        return 0.5f * ((bx - ax) * (cy - ay) - (cx - ax) * (by - ay));
      };
      const float a0 = area(p0.data()), a1 = area(v.data());
      const float r = a1 / (std::fabs(a0) < 1e-6f ? 1e-6f : a0);
      if (r > 0.5f && r < 2.0f) ok[t >> 3] |= (uint8_t)(1u << (t & 7));
    }
    return 0;
  }
  // .tmv with the body inflated: "TMV1" k nv nt nf crc32 | p0 | tri | dv | ok
  void encode(std::vector<uint8_t>& out, uint32_t (*crc)(const uint8_t*, size_t)) const {
    const size_t nb = (size_t)nv * 4 + (size_t)nt * 6 + (size_t)nf * nv * 4 + (size_t)nf * ((nt + 7) / 8);
    out.assign(16 + nb, 0);
    uint8_t* b = out.data() + 16;
    for (int i = 0; i < nv * 2; i++) { const int16_t q = (int16_t)std::lrint(std::nearbyint(p0[i] * 16.0f)); std::memcpy(b + (size_t)i * 2, &q, 2); }
    b += (size_t)nv * 4;
    for (int i = 0; i < nt * 3; i++) { const uint16_t t = (uint16_t)tri[i]; std::memcpy(b + (size_t)i * 2, &t, 2); }
    b += (size_t)nt * 6;
    std::memcpy(b, dv.data(), dv.size() * 2); b += dv.size() * 2;
    std::memcpy(b, okb.data(), okb.size());
    std::memcpy(out.data(), "TMV1", 4);
    const uint16_t hd[4] = {(uint16_t)k, (uint16_t)nv, (uint16_t)nt, (uint16_t)nf};
    std::memcpy(out.data() + 4, hd, 8);
    const uint32_t c = crc(out.data() + 16, nb);
    std::memcpy(out.data() + 12, &c, 4);
  }
};

}  // namespace trk

}  // namespace tmap

// ---------------------------------------------------------------- C ABI (ctypes / engine binding)
extern "C" {
TMAP_EXPORT int tmap_version(void) { return TMAP_VERSION; }
TMAP_EXPORT int tm_build(const uint8_t* rgba, int w, int h, int k, int nv, const float* p0, int nt, const int32_t* tri,
             const float* v, unsigned sid, const char* par, uint8_t* out, int32_t* ids, double* timing9, const uint8_t* tri_ok) {
  tmap::Params P; tmap::parse_params(par ? par : "", P);
  tmap::Timing T;
  int r = tmap::build(rgba, w, h, k, nv, p0, nt, tri, v, sid, P, out, ids, &T, tri_ok);
  if (timing9) { double t[9] = {T.fill, T.normals, T.holes, T.stats, T.broad, T.thick, T.colour, T.rim, T.out}; std::memcpy(timing9, t, sizeof t); }
  return r;
}
// ---- MiniFFI entries (integers and pointers only, at most 10 arguments; no C++ exception crosses this line)
// ip: [sheet_w, h, frame_w, k, sid, frame]   rgba: whole sprite sheet (Bitmap#raw_data, RGBA)
// tmv: .tmv file with its zlib part already inflated: 16-byte header | p0 | tri | dv | ok
// par: params text   out: (h*k)*(frame_w*k)*4 bytes, written in full
static tmap::Job* job_from(const int32_t* ip, const uint8_t* rgba, const uint8_t* tmv, const char* par, int* err) {
  *err = 11;
  if (!ip || !rgba || !tmv) return nullptr;
  const int sw = ip[0], h = ip[1], fw = ip[2], k = ip[3], fi = ip[5]; const uint32_t sid = (uint32_t)ip[4];
  const int sx = ip[6] < 0 ? fi * fw : ip[6]; const bool bgra = (ip[7] & 1) != 0;
  *err = 12;
  if (std::memcmp(tmv, "TMV1", 4) != 0) return nullptr;
  uint16_t hk, nv, nt, nf; std::memcpy(&hk, tmv + 4, 2); std::memcpy(&nv, tmv + 6, 2); std::memcpy(&nt, tmv + 8, 2); std::memcpy(&nf, tmv + 10, 2);
  *err = 13;
  if (hk != k || k <= 0 || fw <= 0 || h <= 0 || fi < 0 || fi >= nf || sx < 0 || sw < sx + fw) return nullptr;
  const uint8_t* b = tmv + 16;
  std::vector<float> p0(nv * 2), v(nv * 2);
  std::vector<int32_t> tri(nt * 3);
  const size_t o_tri = (size_t)nv * 4, o_dv = o_tri + (size_t)nt * 6 + (size_t)fi * nv * 4;
  const size_t o_ok = o_tri + (size_t)nt * 6 + (size_t)nf * nv * 4 + (size_t)fi * ((nt + 7) / 8);
  for (int i = 0; i < nv * 2; i++) {
    int16_t q, d; std::memcpy(&q, b + (size_t)i * 2, 2); std::memcpy(&d, b + o_dv + (size_t)i * 2, 2);
    p0[i] = q / 16.0f; v[i] = (int)(q + d) / 16.0f;
  }
  for (int i = 0; i < nt * 3; i++) { uint16_t t; std::memcpy(&t, b + o_tri + (size_t)i * 2, 2); if (t >= nv) return nullptr; tri[i] = t; }
  std::vector<uint8_t> fr((size_t)fw * h * 4);
  for (int y = 0; y < h; y++) std::memcpy(&fr[(size_t)y * fw * 4], rgba + ((size_t)y * sw + (size_t)sx) * 4, (size_t)fw * 4);
  if (bgra) for (size_t i = 0; i < fr.size(); i += 4) std::swap(fr[i], fr[i + 2]);
  tmap::Params P; tmap::parse_params(par ? par : "", P);
  *err = 0;
  return new tmap::Job(fr.data(), fw, h, k, nv, p0.data(), nt, tri.data(), v.data(), sid, P, b + o_ok);
}
// error codes are positive (MiniFFI may not sign-extend a 32-bit return on 64-bit): 11 null input, 12 not TMV1,
// 13 size/frame mismatch, 14 job not finished, 19 out of memory or other failure
// one map in one call
TMAP_EXPORT int tmap_frame(const int32_t* ip, const uint8_t* rgba, const uint8_t* tmv, const char* par, uint8_t* out) {
  try {
    int err; tmap::Job* j = job_from(ip, rgba, tmv, par, &err);
    if (!j) return err;
    while (!j->step(1e300)) {}
    std::memcpy(out, j->out.data(), j->out.size());
    delete j; return 0;
  } catch (...) { return 19; }
}
// one map spread over game frames: new -> step(budget) until 1 -> take -> free
TMAP_EXPORT intptr_t tmap_job_new(const int32_t* ip, const uint8_t* rgba, const uint8_t* tmv, const char* par) {
  try { int err; return (intptr_t)job_from(ip, rgba, tmv, par, &err); } catch (...) { return 0; }
}
TMAP_EXPORT int tmap_job_step(intptr_t h, int budget_us) {
  if (!h) return 11;
  try { return ((tmap::Job*)h)->step(budget_us / 1000.0) ? 1 : 0; } catch (...) { return 19; }
}
TMAP_EXPORT int tmap_job_take(intptr_t h, uint8_t* out) {
  if (!h || !out) return 11;
  tmap::Job* j = (tmap::Job*)h;
  if (!j->done()) return 14;
  std::memcpy(out, j->out.data(), j->out.size());
  return 0;
}
TMAP_EXPORT void tmap_job_free(intptr_t h) { delete (tmap::Job*)h; }
struct CrcTab { uint32_t t[256]; CrcTab() { for (uint32_t n = 0; n < 256; n++) { uint32_t c = n; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[n] = c; } } };
static const uint32_t* crc_table() { static const CrcTab T; return T.t; }
TMAP_EXPORT int tmap_hash(const uint8_t* rgba, const int32_t* ip, uint32_t* io) {
  if (!rgba || !ip || !io || ip[0] < 0) return 11;
  const bool bgra = (ip[1] & 1) != 0;
  const uint32_t* tab = crc_table();
  uint32_t c = ~*io;
  for (int32_t i = 0; i < ip[0]; i++) {
    const uint8_t* p = rgba + (size_t)i * 4; uint8_t q[4] = {0, 0, 0, 0};
    if (p[3]) { q[0] = bgra ? p[2] : p[0]; q[1] = p[1]; q[2] = bgra ? p[0] : p[2]; q[3] = p[3]; }
    for (int j = 0; j < 4; j++) c = tab[(c ^ q[j]) & 0xFF] ^ (c >> 8);
  }
  *io = ~c;
  return 0;
}
TMAP_EXPORT void tm_resize_f32(const float* s, int w, int h, int k, float* d) { tmap::resize_linear_f32(s, w, h, k, d); }
TMAP_EXPORT void tm_gauss_f32(float* a, int w, int h, double s) { tmap::gauss_f32(a, w, h, s); }
TMAP_EXPORT void tm_gauss_f64(double* a, int w, int h, double s) { tmap::gauss_f64(a, w, h, s); }
TMAP_EXPORT void tm_edt(const uint8_t* m, int w, int h, double* o) { tmap::edt(m, w, h, o); }
TMAP_EXPORT void tm_fill(const float* v, int nt, const int32_t* tri, int W, int H, int32_t* ids) { tmap::fill_tris(v, nt, tri, nullptr, W, H, ids); }
TMAP_EXPORT void tm_hsv(const uint8_t* rgb, int n, uint8_t* hsv) { tmap::rgb2hsv8(rgb, 3, n, hsv); }
TMAP_EXPORT double tm_hmix(unsigned a, unsigned b, unsigned c) { return tmap::hmix(a, b, c); }
// ---- v3: tracks for pictures the pack does not have
static uint32_t tm_crc(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  const uint32_t* tab = crc_table();
  for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}
// checks (ctypes): whole sprite in one call. frames: nf frames frame-major, fw*fh*4 each. returns 0 / error code, *len = bytes
TMAP_EXPORT int tm_track(const uint8_t* frames, int fw, int fh, int nf, int k, int flags, uint8_t* out, int cap, int* len) {
  try {
    if (!frames || fw <= 0 || fh <= 0 || nf <= 0 || k <= 0) return 11;
    tmap::trk::Sprite S; S.fw = fw; S.fh = fh; S.nf = nf; S.k = k; S.bgra = (flags & 1) != 0;
    S.px.assign(frames, frames + (size_t)fw * fh * 4 * nf);
    int r = S.prepare(); if (r) return r;
    tmap::trk::DisWork K;
    for (int f = 0; f < nf; f++) { r = S.track(f, K); if (r) return r; }
    std::vector<uint8_t> o; S.encode(o, tm_crc);
    if (len) *len = (int)o.size();
    if (!out || cap < (int)o.size()) return 16;
    std::memcpy(out, o.data(), o.size());
    return 0;
  } catch (...) { return 19; }
}
TMAP_EXPORT int tm_mesh(const uint8_t* frames, int fw, int fh, int nf, int k, float* p0, int* nv, int32_t* tri, int* nt, int cap) {
  try {
    tmap::trk::Sprite S; S.fw = fw; S.fh = fh; S.nf = nf; S.k = k;
    S.px.assign(frames, frames + (size_t)fw * fh * 4 * nf);
    int r = S.prepare(); if (r) return r;
    *nv = S.nv; *nt = S.nt;
    if (S.nv * 2 > cap || S.nt * 3 > cap) return 16;
    std::memcpy(p0, S.p0.data(), S.p0.size() * 4); std::memcpy(tri, S.tri.data(), S.tri.size() * 4);
    return 0;
  } catch (...) { return 19; }
}
TMAP_EXPORT void tm_flowimg(const uint8_t* rgba, int w, int h, int k, uint8_t* out) {
  std::vector<uint8_t> g; tmap::trk::flow_img(rgba, w, h, k, false, g); std::memcpy(out, g.data(), g.size());
}
// full-resolution DIS flow (W*H*2 floats interleaved) as cv2 DISOpticalFlow.calc, call = how many calcs the object did before
TMAP_EXPORT int tm_dis(const uint8_t* I0, const uint8_t* I1, int W, int H, int call, int vr, float* flow) {
  try {
    tmap::trk::DisCfg c = tmap::trk::dis_cfg(W, H, call); c.vr = vr;
    tmap::trk::DisRef R; tmap::trk::dis_ref(I0, W, H, c, R);
    tmap::trk::DisWork K;
    if (!tmap::trk::dis_calc(R, I1, K)) return 25;
    const tmap::trk::DisLevel& L = R.lv[c.finest];
    std::vector<float> a((size_t)W * H), b((size_t)W * H);
    tmap::trk::resize_linear_any(K.Ux.data(), L.w, L.h, a.data(), W, H);
    tmap::trk::resize_linear_any(K.Uy.data(), L.w, L.h, b.data(), W, H);
    const float m = (float)(1 << c.finest);
    for (size_t i = 0; i < a.size(); i++) { flow[2 * i] = a[i] * m; flow[2 * i + 1] = b[i] * m; }
    return 0;
  } catch (...) { return 19; }
}
}  // extern "C"

// ---- background worker threads: whole sprites are queued, frames are shared out between the workers
namespace tmap { namespace trk {
struct Batch {
  int id = 0;
  Sprite S;
  std::atomic<bool> cancel{false};
  std::atomic<int> state{0};            // 0 queued, 1 preparing, 2 tracking, 3 done, >= 11 error
  std::atomic<int> done{0};
  int next = 0;                         // next frame to hand out (under Pool::mu)
  std::vector<uint8_t> out;
};
struct Pool {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::shared_ptr<Batch>> q;
  std::vector<std::pair<int, std::shared_ptr<Batch>>> table;
  int started = 0, limit = 0, next_id = 1;
  std::shared_ptr<Batch> find(int id) {
    for (auto& e : table) if (e.first == id) return e.second;
    return nullptr;
  }
  // a unit of work for worker `wi`: kind 1 prepare, 2 frame f
  bool pick(int wi, std::shared_ptr<Batch>& b, int& kind, int& f) {
    if (wi >= limit) return false;
    for (auto it = q.begin(); it != q.end();) {
      auto& x = *it;
      if (x->cancel.load()) { it = q.erase(it); continue; }
      const int st = x->state.load();
      if (st == 0) { x->state = 1; b = x; kind = 1; return true; }
      if (st == 2 && x->next < x->S.nf) { b = x; kind = 2; f = x->next++; return true; }
      if (st >= 3) { it = q.erase(it); continue; }
      ++it;
    }
    return false;
  }
  void finish_frame(Batch& b) {
    if (b.done.fetch_add(1) + 1 == b.S.nf && b.state.load() == 2) {
      try { b.S.encode(b.out, tm_crc); b.state = 3; } catch (...) { b.state = 19; }
      std::vector<uint8_t>().swap(b.S.px); std::vector<uint8_t>().swap(b.S.g0);
      b.S.ref0 = DisRef(); b.S.ref1 = DisRef();
    }
  }
  static void lower_priority() {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__) || defined(__ANDROID__)
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
  }
  void worker(int wi) {
    lower_priority();
    DisWork K;
    for (;;) {
      std::shared_ptr<Batch> b; int kind = 0, f = 0;
      {
        std::unique_lock<std::mutex> lk(mu);
        while (!pick(wi, b, kind, f)) {
          if (q.empty()) K = DisWork();  // nothing queued: give the buffers back
          cv.wait(lk);
        }
      }
      try {
        if (kind == 1) {
          const int r = b->S.prepare();
          if (r) b->state = r;
          else { b->state = 2; }
          std::lock_guard<std::mutex> lk(mu); cv.notify_all();
        } else {
          const int r = b->S.track(f, K, &b->cancel);
          if (r) { if (!b->cancel.load()) b->state = r; }
          else finish_frame(*b);
        }
      } catch (...) { b->state = 19; }
    }
  }
  bool ensure(int want) {
    if (want < 1) {
      unsigned hc = std::thread::hardware_concurrency();
      want = hc > 1 ? (int)hc - 1 : 1;
      if (want > 3) want = 3;
    }
    limit = want;
    try {
      while (started < want) { std::thread t(&Pool::worker, this, started); t.detach(); started++; }
    } catch (...) { if (started == 0) return false; limit = started; }
    cv.notify_all();
    return true;
  }
};
static Pool& pool() { static Pool* p = new Pool(); return *p; }  // never destroyed: detached workers may outlive static destructors
}}  // namespace tmap::trk

extern "C" {
// ip = int32[8] {frame_w, frame_h, nf, k, flags (bit0 BGRA), threads (0 = cores-1, at most 3), face_k*1000 (0 = 1.0), 0}
// rgba = nf frames one after another (frame i at i*fw*fh*4), copied here. returns handle > 0, or 0 for bad input / no threads
TMAP_EXPORT int tmap_batch_new(const int32_t* ip, const uint8_t* rgba) {
  using namespace tmap::trk;
  try {
    if (!ip || !rgba) return 0;
    const int fw = ip[0], fh = ip[1], nf = ip[2], k = ip[3];
    if (fw <= 0 || fh <= 0 || nf <= 0 || nf > 4096 || k <= 0 || k > 8 || (int64_t)fw * k > 8192 || (int64_t)fh * k > 8192) return 0;
    auto b = std::make_shared<Batch>();
    b->S.fw = fw; b->S.fh = fh; b->S.nf = nf; b->S.k = k; b->S.bgra = (ip[4] & 1) != 0;
    if (ip[6] > 0) b->S.face_k = ip[6] / 1000.0;
    b->S.px.assign(rgba, rgba + (size_t)fw * fh * 4 * nf);
    Pool& P = pool();
    std::lock_guard<std::mutex> lk(P.mu);
    if (!P.ensure(ip[5])) return 0;
    b->id = P.next_id; P.next_id = P.next_id >= 0x3fffffff ? 1 : P.next_id + 1;
    P.table.push_back({b->id, b}); P.q.push_back(b);
    P.cv.notify_all();
    return b->id;
  } catch (...) { return 0; }
}
// 0 running, 1 done, >= 11 error (11 unknown handle, 19 out of memory, 21 empty picture, 22 too many vertices,
// 23 triangulation, 24 picture too small, 25 flow, 26 picture too large). io (optional) int32[4] {frames done, nf, .tmv bytes, state}
TMAP_EXPORT int tmap_batch_poll(int h, int32_t* io) {
  using namespace tmap::trk;
  Pool& P = pool();
  std::shared_ptr<Batch> b;
  { std::lock_guard<std::mutex> lk(P.mu); b = P.find(h); }
  if (!b) return 11;
  const int st = b->state.load();
  if (io) { io[0] = b->done.load(); io[1] = b->S.nf; io[2] = st == 3 ? (int)b->out.size() : 0; io[3] = st; }
  return st == 3 ? 1 : (st >= 11 ? st : 0);
}
// .tmv (16-byte header + inflated body; the caller deflates the body to store it). out: io[2] bytes
TMAP_EXPORT int tmap_batch_take(int h, uint8_t* out) {
  using namespace tmap::trk;
  if (!out) return 11;
  Pool& P = pool();
  std::shared_ptr<Batch> b;
  { std::lock_guard<std::mutex> lk(P.mu); b = P.find(h); }
  if (!b) return 11;
  if (b->state.load() != 3) return 14;
  std::memcpy(out, b->out.data(), b->out.size());
  return 0;
}
// cancel (if running) and forget; memory goes when the last worker lets go of it
TMAP_EXPORT int tmap_batch_free(int h) {
  using namespace tmap::trk;
  Pool& P = pool();
  std::lock_guard<std::mutex> lk(P.mu);
  for (size_t i = 0; i < P.table.size(); i++)
    if (P.table[i].first == h) { P.table[i].second->cancel = true; P.table.erase(P.table.begin() + i); return 0; }
  return 11;
}
TMAP_EXPORT void tm_resize_area(const uint8_t* s, int sw, int sh, uint8_t* d, int dw, int dh) { tmap::trk::resize_area_u8(s, sw, sh, d, dw, dh); }
TMAP_EXPORT void tm_resize_lin(const float* s, int sw, int sh, float* d, int dw, int dh) { tmap::trk::resize_linear_any(s, sw, sh, d, dw, dh); }
}

// ---- v5: whole body map on background threads + disk cache
// Same steps as the game's Ruby path (tmap_job per frame, then eye cut, body crop with a 1 px margin, frame-0 centre,
// shelf pack into atlases of side <= 4096), so the bytes match. Blob (little endian):
//   "TMA1" u32 nf u32 mw u32 mh u32 natl u32 0 f64 dc | nf x i32 {row0, row1, atlas (-1 none), rect_x, rect_y} |
//   natl x u32 {w, h} | atlas pixels (RGBA, w*h*4 each)
// v6 maps (ip[11] bit0): "TMA2" u32 nf u32 mw u32 mh u32 natl u32 naux f64 dc |
//   nf x i32 {row0, row1, atlas, rect_x, rect_y, aux_atlas (-1 none), aux_dx, aux_dy, body_top, body_bottom} |
//   natl x u32 {w, h} | naux x u32 {w, h} | map atlases | facet atlases (same layout) | aux atlases
//   map pixel: R = level (frame 0 where the sprite has not moved), G,B = frame-0 facet normal (moved with the mesh),
//              A = 0 keep original, 1 facet without a frame-0 facet, 2..255 frame-0 facet level
//   facet pixel: four bytes of facet hashes (frame-0 facet ids), 0 = none
//   aux pixel (sprite resolution, body box + 1): R = edge distance x16, G = max distance within 6 px x16, B,A = outward normal x127+128
// Cache file: "TBC1" u32 TMAP_VERSION u32 key u32 blob_len u32 crc(blob) | blob as runs of 32-bit pixels
//   (u32 n: bit31 set = n&0x7fffffff copies of the next pixel, else n literal pixels)
namespace tmap { namespace mp {
static const int MARGIN = 1, MAX_SIDE = 4096;
static std::atomic<long long> g_mem{0}, g_peak{0};
static void mem_add(long long d) {
  long long v = g_mem.fetch_add(d) + d, p = g_peak.load();
  while (v > p && !g_peak.compare_exchange_weak(p, v)) {}
}

struct Map {
  int id = 0;
  int fw = 0, fh = 0, nf = 0, k = 0, sid = 0;
  double grad = 0, lr = 0;
  std::vector<int32_t> ip;
  std::vector<uint8_t> frames, have, tmv, eyes;
  std::string par, path;
  std::atomic<bool> cancel{false};
  std::atomic<int> state{0};  // 0 collecting, 1 queued, 2 cache check, 3 frames, 4 done, >= 11 error
  std::atomic<int> done{0};
  std::atomic<int> hit{0};
  int next = 0;               // next frame to hand out (under Pool::mu)
  int urgent = 0;
  uint32_t key = 0;
  std::vector<int32_t> rows;
  std::vector<int32_t> crop;  // x0, y0, cw, ch per frame (cw 0 = none)
  std::vector<std::vector<uint8_t>> px;
  double dc = 0.5;
  std::vector<uint8_t> blob;
  bool v6 = false;
  int nv = 0, nt = 0;
  std::vector<int16_t> q0, dv;
  std::vector<int32_t> tri;
  std::vector<double> rfx, rfy, rtm;
  std::vector<float> rcnt;
  std::vector<uint8_t> out0;
  std::vector<int32_t> ids0;
  std::vector<std::vector<uint8_t>> fpx, apx;
  std::vector<int32_t> acrop;  // x0, y0, w, h, top, bottom per frame (w 0 = none)
  size_t fsz() const { return (size_t)fw * fh * 4; }
  ~Map() {
    long long n = (long long)(frames.size() + tmv.size() + eyes.size() + blob.size());
    for (auto& p : px) n += (long long)p.size();
    for (auto& p : fpx) n += (long long)p.size();
    for (auto& p : apx) n += (long long)p.size();
    n += (long long)(out0.size() + ids0.size() * 4);
    mem_add(-n);
  }
};

static uint32_t crc_more(uint32_t c, const void* d, size_t n) {
  const uint32_t* tab = crc_table(); const uint8_t* p = (const uint8_t*)d; c = ~c;
  for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

#if defined(_WIN32)
static std::wstring wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n : 1, L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}
static FILE* fopen8(const std::string& p, const char* m) { return _wfopen(wide(p).c_str(), wide(m).c_str()); }
static bool replace8(const std::string& a, const std::string& b) { return MoveFileExW(wide(a).c_str(), wide(b).c_str(), MOVEFILE_REPLACE_EXISTING) != 0; }
static void remove8(const std::string& p) { _wremove(wide(p).c_str()); }
#else
static FILE* fopen8(const std::string& p, const char* m) { return std::fopen(p.c_str(), m); }
static bool replace8(const std::string& a, const std::string& b) { return std::rename(a.c_str(), b.c_str()) == 0; }
static void remove8(const std::string& p) { std::remove(p.c_str()); }
#endif

static void put32(std::vector<uint8_t>& o, uint32_t v) { uint8_t b[4]; std::memcpy(b, &v, 4); o.insert(o.end(), b, b + 4); }

static bool load_cache(Map& m) {
  if (m.path.empty()) return false;
  FILE* f = fopen8(m.path, "rb");
  if (!f) return false;
  std::vector<uint8_t> raw;
  std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
  if (sz < 20 || sz > (1L << 30)) { std::fclose(f); return false; }
  raw.resize((size_t)sz);
  const bool rd = std::fread(raw.data(), 1, raw.size(), f) == raw.size();
  std::fclose(f);
  if (!rd || std::memcmp(raw.data(), "TBC1", 4) != 0) return false;
  uint32_t ver, key, len, crc; std::memcpy(&ver, &raw[4], 4); std::memcpy(&key, &raw[8], 4); std::memcpy(&len, &raw[12], 4); std::memcpy(&crc, &raw[16], 4);
  if (ver != TMAP_VERSION || key != m.key || len % 4 != 0 || len > (1u << 30)) return false;
  mem_add((long long)raw.size() + len);
  std::vector<uint8_t> b(len);
  size_t i = 20, o = 0;
  bool ok = true;
  while (o < len) {
    if (i + 4 > raw.size()) { ok = false; break; }
    uint32_t c; std::memcpy(&c, &raw[i], 4); i += 4;
    const bool run = (c & 0x80000000u) != 0; const size_t cnt = (size_t)(c & 0x7fffffffu) * 4;
    if (cnt == 0 || o + cnt > len) { ok = false; break; }
    if (run) {
      if (i + 4 > raw.size()) { ok = false; break; }
      for (size_t j = 0; j < cnt; j += 4) std::memcpy(&b[o + j], &raw[i], 4);
      i += 4;
    } else {
      if (i + cnt > raw.size()) { ok = false; break; }
      std::memcpy(&b[o], &raw[i], cnt); i += cnt;
    }
    o += cnt;
  }
  mem_add(-(long long)raw.size());
  std::vector<uint8_t>().swap(raw);
  uint32_t nf = 0;
  if (ok && b.size() >= 32) std::memcpy(&nf, &b[4], 4);
  if (!ok || b.size() < 32 || std::memcmp(b.data(), m.v6 ? "TMA2" : "TMA1", 4) != 0 || (int)nf != m.nf || crc_more(0, b.data(), b.size()) != crc) {
    mem_add(-(long long)len);
    return false;
  }
  m.blob.swap(b);
  return true;
}

static void save_cache(const Map& m) {
  if (m.path.empty() || m.blob.size() % 4 != 0) return;
  std::vector<uint8_t> o;
  o.reserve(m.blob.size() / 4);
  o.insert(o.end(), {'T', 'B', 'C', '1'});
  put32(o, TMAP_VERSION); put32(o, m.key); put32(o, (uint32_t)m.blob.size()); put32(o, crc_more(0, m.blob.data(), m.blob.size()));
  const size_t np = m.blob.size() / 4; const uint8_t* p = m.blob.data();
  size_t i = 0;
  while (i < np) {
    size_t r = 1;
    while (i + r < np && r < 0x7fffffff && std::memcmp(p + (i + r) * 4, p + i * 4, 4) == 0) r++;
    if (r >= 3) { put32(o, 0x80000000u | (uint32_t)r); o.insert(o.end(), p + i * 4, p + i * 4 + 4); i += r; continue; }
    size_t s = i, e = i;
    while (e < np && e - s < 0x7fffffff) {
      if (e + 2 < np && std::memcmp(p + e * 4, p + (e + 1) * 4, 4) == 0 && std::memcmp(p + e * 4, p + (e + 2) * 4, 4) == 0) break;
      e++;
    }
    put32(o, (uint32_t)(e - s)); o.insert(o.end(), p + s * 4, p + e * 4); i = e;
  }
  mem_add((long long)o.capacity());
  const std::string tmp = m.path + ".tmp";
  FILE* f = fopen8(tmp, "wb");
  if (f) {
    const bool ok = std::fwrite(o.data(), 1, o.size(), f) == o.size();
    if (std::fclose(f) != 0 || !ok || !replace8(tmp, m.path)) remove8(tmp);
  }
  mem_add(-(long long)o.capacity());
}

// the Ruby finish: eyes off, body rows, frame-0 centre, crop
static int finish(Map& m, int i, std::vector<uint8_t>& out, const std::vector<uint8_t>* face = nullptr) {
  const int fw = m.fw, fh = m.fh, k = m.k, mw = fw * k, mh = fh * k;
  const uint8_t* fr = &m.frames[(size_t)i * m.fsz()];
  int top = -1, bot = -1, left = fw, right = -1;
  for (int y = 0; y < fh; y++) {
    int x0 = -1, x1 = -1;
    for (int x = 0; x < fw; x++) { if (fr[((size_t)y * fw + x) * 4 + 3] == 0) continue; if (x0 < 0) x0 = x; x1 = x; }
    if (x0 < 0) continue;
    if (top < 0) top = y;
    bot = y;
    if (x0 < left) left = x0;
    if (x1 > right) right = x1;
  }
  int32_t* cr = &m.crop[(size_t)i * 4];
  if (top < 0) { m.rows[i * 2] = 0; m.rows[i * 2 + 1] = 0; cr[2] = 0; return 0; }
  if (!m.eyes.empty()) {
    const size_t rb = (fw + 7) / 8, base = (size_t)i * fh * rb, len = m.eyes.size();
    for (int y = 0; y < fh; y++) {
      const size_t o = base + y * rb;
      if (o > len) continue;
      if (o + rb > len) return 31;
      for (size_t bx = 0; bx < rb; bx++) {
        const uint8_t v = m.eyes[o + bx];
        if (!v) continue;
        for (int b = 0; b < 8; b++) {
          if (!(v & (0x80 >> b))) continue;
          const int x = (int)bx * 8 + b;
          if (x >= fw) continue;
          for (int dy = 0; dy < k; dy++) {
            const size_t q = (((size_t)y * k + dy) * mw + (size_t)x * k) * 4 + 3;
            for (int dx = 0; dx < k; dx++) out[q + dx * 4] = 0;
          }
        }
      }
    }
  }
  const int r0 = top * k, r1 = bot * k + k - 1;
  m.rows[i * 2] = r0; m.rows[i * 2 + 1] = r1;
  if (i == 0) {
    const int h = k / 2;
    const double span = std::max((double)(r1 - r0), 1.0);
    std::vector<double> vals;
    int st = (int)std::ceil(std::sqrt((bot - top + 1) * (right - left + 1) / 1024.0));
    if (st < 1) st = 1;
    for (int y = top; y <= bot; y += st)
      for (int x = left; x <= right; x += st) {
        const size_t o = ((size_t)y * fw + x) * 4;
        if (fr[o + 3] == 0) continue;
        const size_t mo = (((size_t)y * k + h) * mw + (size_t)x * k + h) * 4;
        if (out[mo + 3] == 0) continue;
        const double yy = (double)((y * k + h) - r0) / span;
        const double rv = std::round(out[mo] + 255.0 * m.grad * (0.5 - yy));
        const int r = (int)clip(rv, 0.0, 255.0);
        const double lum = (fr[o] * 0.299 + fr[o + 1] * 0.587 + fr[o + 2] * 0.114) / 255.0;
        vals.push_back(clip(r / 255.0 + (lum - 0.42) * m.lr, 0.0, 1.0));
      }
    if (!vals.empty()) {
      std::sort(vals.begin(), vals.end());
      const size_t n = vals.size();
      m.dc = (n & 1) ? vals[n / 2] : (vals[n / 2 - 1] + vals[n / 2]) / 2.0;
    }
  }
  const int x0 = left * k - MARGIN, y0 = top * k - MARGIN;
  const int cw = (right - left + 1) * k + MARGIN * 2, ch = (bot - top + 1) * k + MARGIN * 2;
  std::vector<uint8_t>& buf = m.px[i];
  buf.assign((size_t)cw * ch * 4, 0);
  mem_add((long long)buf.size());
  const int sx0 = std::max(x0, 0), sx1 = std::min(x0 + cw, mw);
  for (int r = 0; r < ch; r++) {
    const int y = y0 + r;
    if (y < 0 || y >= mh) continue;
    std::memcpy(&buf[((size_t)r * cw + (sx0 - x0)) * 4], &out[((size_t)y * mw + sx0) * 4], (size_t)(sx1 - sx0) * 4);
  }
  cr[0] = x0; cr[1] = y0; cr[2] = cw; cr[3] = ch;
  if (face) {
    std::vector<uint8_t>& fb = m.fpx[i];
    fb.assign((size_t)cw * ch * 4, 0);
    mem_add((long long)fb.size());
    for (int r = 0; r < ch; r++) {
      const int y = y0 + r;
      if (y < 0 || y >= mh) continue;
      std::memcpy(&fb[((size_t)r * cw + (sx0 - x0)) * 4], &(*face)[((size_t)y * mw + sx0) * 4], (size_t)(sx1 - sx0) * 4);
    }
  }
  return 0;
}

// ---- v6: mesh of the .tmv (frame vertices), frame-0 reference, frame-0 facet attributes moved with the mesh, aux values
static bool tmv_mesh(Map& m) {
  if (m.tmv.size() < 16 || std::memcmp(m.tmv.data(), "TMV1", 4) != 0) return false;
  uint16_t hk, nv, nt, nf; std::memcpy(&hk, &m.tmv[4], 2); std::memcpy(&nv, &m.tmv[6], 2); std::memcpy(&nt, &m.tmv[8], 2); std::memcpy(&nf, &m.tmv[10], 2);
  const size_t need = 16 + (size_t)nv * 4 + (size_t)nt * 6 + (size_t)nf * nv * 4;
  if (hk != m.k || nf != m.nf || m.tmv.size() < need) return false;
  const uint8_t* b = &m.tmv[16];
  m.nv = nv; m.nt = nt;
  m.q0.resize((size_t)nv * 2); std::memcpy(m.q0.data(), b, (size_t)nv * 4);
  m.tri.resize((size_t)nt * 3);
  for (size_t i = 0; i < (size_t)nt * 3; i++) { uint16_t t; std::memcpy(&t, b + (size_t)nv * 4 + i * 2, 2); if (t >= nv) return false; m.tri[i] = t; }
  m.dv.resize((size_t)nf * nv * 2); std::memcpy(m.dv.data(), b + (size_t)nv * 4 + (size_t)nt * 6, (size_t)nf * nv * 4);
  return true;
}
static inline void vert(const Map& m, int f, int v, double& x, double& y) {
  const size_t o = (size_t)f * m.nv * 2 + (size_t)v * 2;
  x = (m.q0[(size_t)v * 2] + m.dv[o]) / 16.0; y = (m.q0[(size_t)v * 2 + 1] + m.dv[o + 1]) / 16.0;
}
static void cut_eyes(Map& m, int i, std::vector<uint8_t>& out) {
  if (m.eyes.empty()) return;
  const int fw = m.fw, fh = m.fh, k = m.k, mw = fw * k;
  const size_t rb = (fw + 7) / 8, base = (size_t)i * fh * rb, len = m.eyes.size();
  for (int y = 0; y < fh; y++) {
    const size_t o = base + y * rb;
    if (o + rb > len) return;
    for (size_t bx = 0; bx < rb; bx++) {
      const uint8_t v = m.eyes[o + bx];
      if (!v) continue;
      for (int b = 0; b < 8; b++) {
        if (!(v & (0x80 >> b))) continue;
        const int x = (int)bx * 8 + b;
        if (x >= fw) continue;
        for (int dy = 0; dy < k; dy++) {
          const size_t q = (((size_t)y * k + dy) * mw + (size_t)x * k) * 4 + 3;
          for (int dx = 0; dx < k; dx++) out[q + dx * 4] = 0;
        }
      }
    }
  }
}
// frame 0 with its own vertices: facet normals / mean thickness for the other frames, its map and facet ids
static int ref_frame(Map& m) {
  int ip[8] = {m.fw, m.fh, m.fw, m.k, m.sid, 0, 0, 0};
  int err = 0;
  std::unique_ptr<tmap::Job> j(job_from(ip, &m.frames[0], m.tmv.data(), m.par.c_str(), &err));
  if (!j) return err ? err : 19;
  j->stable_holes = true;
  while (!j->step(5.0)) if (m.cancel.load()) return 0;
  if (j->empty) return 0;
  m.rfx = j->fx; m.rfy = j->fy; m.rcnt = j->cnt; m.rtm.assign(j->tm_.begin(), j->tm_.end());
  m.out0.swap(j->out);
  m.ids0.swap(j->ids);
  cut_eyes(m, 0, m.out0);
  mem_add((long long)(m.out0.size() + m.ids0.size() * 4));
  return 0;
}
static inline uint8_t fhash8(int32_t id, uint32_t o) {
  uint32_t x = (uint32_t)id + o + 1u;
  x = (x ^ 61u) ^ (x >> 16); x *= 9u; x ^= x >> 4; x *= 0x27d4eb2du; x ^= x >> 15;
  return (uint8_t)((x & 0xffffu) >> 8);
}
// rule23 c0: where the sprite equals frame 0 within 1 sprite px use frame 0 as it is, elsewhere frame-0 facet attributes
// found through this frame's triangle (barycentric -> frame-0 position); the level keeps this frame's map there
static void c0(Map& m, int f, tmap::Job& j, std::vector<uint8_t>& face) {
  const int fw = m.fw, fh = m.fh, k = m.k, W = fw * k, H = fh * k;
  const size_t N = (size_t)W * H;
  std::vector<uint8_t>& out = j.out;
  face.assign(N * 4, 0);
  const bool ref = !m.out0.empty() && m.ids0.size() == N && j.ids.size() == N;
  std::vector<uint8_t> diff((size_t)fw * fh, 0);
  if (ref && f != 0) {
    const uint8_t* a = &m.frames[(size_t)f * m.fsz()]; const uint8_t* b = &m.frames[0];
    for (size_t i = 0; i < (size_t)fw * fh; i++) diff[i] = std::memcmp(a + i * 4, b + i * 4, 4) != 0;
  }
  for (int Y = 0; Y < H; Y++)
    for (int X = 0; X < W; X++) {
      const size_t I = (size_t)Y * W + X;
      uint8_t mb[4], mf[4];
      int32_t idf;
      std::memcpy(mb, &out[I * 4], 4); std::memcpy(mf, mb, 4);
      idf = j.ids.size() == N ? j.ids[I] : -1;
      if (ref) {
        bool same = true;
        if (f != 0) {
          const int sx0 = std::max((X - k) / k, 0), sx1 = std::min((X + k) / k, fw - 1);
          const int sy0 = std::max((Y - k) / k, 0), sy1 = std::min((Y + k) / k, fh - 1);
          for (int sy = sy0; sy <= sy1 && same; sy++) for (int sx = sx0; sx <= sx1; sx++) if (diff[(size_t)sy * fw + sx]) { same = false; break; }
        }
        if (same) {
          std::memcpy(mb, &m.out0[I * 4], 4); std::memcpy(mf, mb, 4); idf = m.ids0[I];
        } else if (idf >= 0 && idf < m.nt) {
          double ax, ay, bx, by, cx, cy, a0x, a0y, b0x, b0y, c0x, c0y;
          vert(m, f, m.tri[idf * 3], ax, ay); vert(m, f, m.tri[idf * 3 + 1], bx, by); vert(m, f, m.tri[idf * 3 + 2], cx, cy);
          vert(m, 0, m.tri[idf * 3], a0x, a0y); vert(m, 0, m.tri[idf * 3 + 1], b0x, b0y); vert(m, 0, m.tri[idf * 3 + 2], c0x, c0y);
          const double e1x = bx - ax, e1y = by - ay, e2x = cx - ax, e2y = cy - ay, rx = X + 0.5 - ax, ry = Y + 0.5 - ay;
          double den = e1x * e2y - e1y * e2x;
          if (std::fabs(den) < 1e-6) den = 1e-6;
          const double l1 = (rx * e2y - ry * e2x) / den, l2 = (e1x * ry - e1y * rx) / den;
          const double qx = a0x + l1 * (b0x - a0x) + l2 * (c0x - a0x), qy = a0y + l1 * (b0y - a0y) + l2 * (c0y - a0y);
          const int ix = std::min(std::max((int)std::floor(qx), 0), W - 1), iy = std::min(std::max((int)std::floor(qy), 0), H - 1);
          const size_t Q = (size_t)iy * W + ix;
          if (m.out0[Q * 4 + 3] > 0) std::memcpy(mf, &m.out0[Q * 4], 4);
          if (m.ids0[Q] >= 0) idf = m.ids0[Q];
        }
      }
      out[I * 4] = mb[0]; out[I * 4 + 1] = mf[1]; out[I * 4 + 2] = mf[2];
      out[I * 4 + 3] = mb[3] == 0 ? 0 : (mf[3] > 0 ? std::max<uint8_t>(mf[0], 2) : 1);
      if (idf >= 0) {
        face[I * 4] = fhash8(idf, 0); face[I * 4 + 1] = fhash8(idf, 7919); face[I * 4 + 2] = fhash8(idf, 104729); face[I * 4 + 3] = fhash8(idf, 15485863);
      }
    }
}
// aux values of one frame at sprite resolution (same steps as the game's Ruby ARCTeraBodyAux)
static void edt1(const std::vector<double>& f, int n, std::vector<int>& v, std::vector<double>& z, std::vector<double>& d) {
  const double INF = 1e9;
  int k = 0; v[0] = 0; z[0] = -INF; z[1] = INF;
  for (int q = 1; q < n; q++) {
    const double fq = f[q] + (double)q * q;
    double s = 0;
    for (;;) {
      const int p = v[k];
      s = (fq - (f[p] + (double)p * p)) / (2.0 * (q - p));
      if (s > z[k] || k == 0) break;
      k--;
    }
    if (s <= z[k]) { v[0] = q; z[0] = -INF; z[1] = INF; k = 0; }
    else { k++; v[k] = q; z[k] = s; z[k + 1] = INF; }
  }
  k = 0;
  for (int q = 0; q < n; q++) { while (z[k + 1] < q) k++; const int p = v[k]; d[q] = (double)(q - p) * (q - p) + f[p]; }
}
static void aux_frame(Map& m, int i) {
  const int fw = m.fw, fh = m.fh;
  const uint8_t* fr = &m.frames[(size_t)i * m.fsz()];
  int32_t* ac = &m.acrop[(size_t)i * 6];
  int top = -1, bot = -1, left = fw, right = -1;
  for (int y = 0; y < fh; y++) for (int x = 0; x < fw; x++) if (fr[((size_t)y * fw + x) * 4 + 3] > 127) { if (top < 0) top = y; bot = y; left = std::min(left, x); right = std::max(right, x); }
  if (top < 0) { ac[2] = 0; return; }
  const int x0 = left - 1, y0 = top - 1, w = right - left + 3, h = bot - top + 3;
  const size_t n = (size_t)w * h;
  std::vector<uint8_t> a(n, 0);
  for (int yy = 1; yy < h - 1; yy++) for (int xx = 1; xx < w - 1; xx++) a[(size_t)yy * w + xx] = fr[(((size_t)(y0 + yy)) * fw + x0 + xx) * 4 + 3] > 127;
  const int mx = std::max(w, h);
  std::vector<int> v(mx); std::vector<double> z(mx + 1), f(mx), d(mx), g(n), dc(n);
  for (int x = 0; x < w; x++) {
    for (int y = 0; y < h; y++) f[y] = a[(size_t)y * w + x] ? 1e9 : 0.0;
    edt1(f, h, v, z, d);
    for (int y = 0; y < h; y++) g[(size_t)y * w + x] = d[y];
  }
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) f[x] = g[(size_t)y * w + x];
    edt1(f, w, v, z, d);
    for (int x = 0; x < w; x++) dc[(size_t)y * w + x] = std::sqrt(d[x]);
  }
  static const int SPAN[7] = {6, 5, 5, 5, 4, 3, 0};
  std::vector<std::vector<double>> span(7);
  span[0] = dc;
  for (int s = 1; s <= 6; s++) {
    span[s].assign(n, 0.0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
      const size_t I = (size_t)y * w + x;
      double b = span[s - 1][I];
      if (x >= s) b = std::max(b, dc[I - s]);
      if (x + s < w) b = std::max(b, dc[I + s]);
      span[s][I] = b;
    }
  }
  std::vector<double> th = span[6];
  for (int y = 0; y < h; y++) for (int dy = 1; dy <= 6; dy++) for (int yy : {y - dy, y + dy}) {
    if (yy < 0 || yy >= h) continue;
    const std::vector<double>& src = span[SPAN[dy]];
    for (int x = 0; x < w; x++) th[(size_t)y * w + x] = std::max(th[(size_t)y * w + x], src[(size_t)yy * w + x]);
  }
  double G[13], gs = 0;
  for (int t = -6; t <= 6; t++) { G[t + 6] = std::exp(-t * t / (2.0 * 1.5 * 1.5)); gs += G[t + 6]; }
  for (double& gv : G) gv /= gs;
  std::vector<double> bh(n, 0.0), bl(n, 0.0);
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    double s = 0;
    for (int t = 0; t < 13; t++) { const int xx = x + t - 6; if (xx >= 0 && xx < w && a[(size_t)y * w + xx]) s += G[t]; }
    bh[(size_t)y * w + x] = s;
  }
  for (int x = 0; x < w; x++) for (int y = 0; y < h; y++) {
    double s = 0;
    for (int t = 0; t < 13; t++) { const int yy = y + t - 6; if (yy >= 0 && yy < h) s += G[t] * bh[(size_t)yy * w + x]; }
    bl[(size_t)y * w + x] = s;
  }
  std::vector<uint8_t>& o = m.apx[i];
  o.assign(n * 4, 0);
  mem_add((long long)o.size());
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    const size_t I = (size_t)y * w + x;
    const double gx = x == 0 ? bl[I + 1] - bl[I] : (x == w - 1 ? bl[I] - bl[I - 1] : (bl[I + 1] - bl[I - 1]) * 0.5);
    const double gy = y == 0 ? bl[I + w] - bl[I] : (y == h - 1 ? bl[I] - bl[I - w] : (bl[I + w] - bl[I - w]) * 0.5);
    const double mg = std::sqrt(gx * gx + gy * gy);
    o[I * 4] = (uint8_t)std::min(255.0, std::floor(dc[I] * 16 + 0.5));
    o[I * 4 + 1] = (uint8_t)std::min(255.0, std::floor(th[I] * 16 + 0.5));
    o[I * 4 + 2] = mg > 1e-5 ? (uint8_t)(std::lround(-gx / mg * 127) + 128) : 128;
    o[I * 4 + 3] = mg > 1e-5 ? (uint8_t)(std::lround(-gy / mg * 127) + 128) : 128;
  }
  ac[0] = x0; ac[1] = y0; ac[2] = w; ac[3] = h; ac[4] = top; ac[5] = bot;
}

// shelf pack (as the Ruby pack) and the blob
static void pack(Map& m) {
  struct Sheet { int w = 0, h = 0, x = 0, y = 0, row = 0; std::vector<std::array<int, 3>> list; };
  std::vector<Sheet> sheets; bool cur = false; Sheet s;
  for (int i = 0; i < m.nf; i++) {
    const int32_t* c = &m.crop[(size_t)i * 4];
    if (c[2] == 0) continue;
    const int w = c[2], h = c[3];
    if (!cur) { s = Sheet(); cur = true; }
    if (s.x + w > MAX_SIDE) { s.y += s.row; s.x = 0; s.row = 0; }
    if (s.y + h > MAX_SIDE) { sheets.push_back(s); s = Sheet(); }
    s.list.push_back({i, s.x, s.y});
    s.w = std::max(s.w, s.x + w); s.h = std::max(s.h, s.y + h); s.row = std::max(s.row, h); s.x += w;
  }
  if (cur) sheets.push_back(s);
  std::vector<int32_t> cell((size_t)m.nf * 3, 0);
  for (int i = 0; i < m.nf; i++) cell[(size_t)i * 3] = -1;
  // v6 aux atlases: own shelf pack, 1 px gap
  std::vector<Sheet> xs; std::vector<int32_t> xcell((size_t)m.nf * 3, 0);
  for (int i = 0; i < m.nf; i++) xcell[(size_t)i * 3] = -1;
  if (m.v6) {
    bool xc = false; Sheet t;
    for (int i = 0; i < m.nf; i++) {
      const int32_t* c = &m.acrop[(size_t)i * 6];
      if (c[2] == 0) continue;
      const int w = c[2] + 1, h = c[3] + 1;
      if (!xc) { t = Sheet(); xc = true; }
      if (t.x + w > MAX_SIDE) { t.y += t.row; t.x = 0; t.row = 0; }
      if (t.y + h > MAX_SIDE) { xs.push_back(t); t = Sheet(); }
      t.list.push_back({i, t.x, t.y});
      t.w = std::max(t.w, t.x + w); t.h = std::max(t.h, t.y + h); t.row = std::max(t.row, h); t.x += w;
    }
    if (xc) xs.push_back(t);
  }
  const size_t rec = m.v6 ? 40 : 20, hdr = 32 + (size_t)m.nf * rec + (sheets.size() + xs.size()) * 8;
  size_t total = hdr;
  for (auto& t : sheets) total += (size_t)std::max(t.w, 1) * std::max(t.h, 1) * 4 * (m.v6 ? 2 : 1);
  for (auto& t : xs) total += (size_t)std::max(t.w, 1) * std::max(t.h, 1) * 4;
  std::vector<uint8_t>& b = m.blob;
  b.assign(total, 0);
  mem_add((long long)total);
  std::memcpy(&b[0], m.v6 ? "TMA2" : "TMA1", 4);
  uint32_t hd[5] = {(uint32_t)m.nf, (uint32_t)(m.fw * m.k), (uint32_t)(m.fh * m.k), (uint32_t)sheets.size(), (uint32_t)xs.size()};
  std::memcpy(&b[4], hd, m.v6 ? 20 : 16); std::memcpy(&b[24], &m.dc, 8);
  const size_t wo = 32 + (size_t)m.nf * rec;
  size_t po = hdr;
  for (int pass = 0; pass < (m.v6 ? 2 : 1); pass++)  // map atlases, then facet atlases in the same layout
    for (size_t ai = 0; ai < sheets.size(); ai++) {
      const Sheet& t = sheets[ai]; const int aw = std::max(t.w, 1), ah = std::max(t.h, 1);
      uint32_t wh[2] = {(uint32_t)aw, (uint32_t)ah};
      if (pass == 0) std::memcpy(&b[wo + ai * 8], wh, 8);
      for (auto& e : t.list) {
        const int i = e[0], ax = e[1], ay = e[2];
        const int32_t* c = &m.crop[(size_t)i * 4];
        std::vector<uint8_t>& src = pass == 0 ? m.px[i] : m.fpx[i];
        if (src.size() == (size_t)c[2] * c[3] * 4)
          for (int r = 0; r < c[3]; r++) std::memcpy(&b[po + ((size_t)(ay + r) * aw + ax) * 4], &src[(size_t)r * c[2] * 4], (size_t)c[2] * 4);
        cell[(size_t)i * 3] = (int32_t)ai; cell[(size_t)i * 3 + 1] = ax - c[0]; cell[(size_t)i * 3 + 2] = ay - c[1];
        mem_add(-(long long)src.size());
        std::vector<uint8_t>().swap(src);
      }
      po += (size_t)aw * ah * 4;
    }
  for (size_t xi = 0; xi < xs.size(); xi++) {
    const Sheet& t = xs[xi]; const int aw = std::max(t.w, 1), ah = std::max(t.h, 1);
    uint32_t wh[2] = {(uint32_t)aw, (uint32_t)ah};
    std::memcpy(&b[wo + (sheets.size() + xi) * 8], wh, 8);
    for (auto& e : t.list) {
      const int i = e[0], ax = e[1], ay = e[2];
      const int32_t* c = &m.acrop[(size_t)i * 6];
      for (int r = 0; r < c[3]; r++) std::memcpy(&b[po + ((size_t)(ay + r) * aw + ax) * 4], &m.apx[i][(size_t)r * c[2] * 4], (size_t)c[2] * 4);
      xcell[(size_t)i * 3] = (int32_t)xi; xcell[(size_t)i * 3 + 1] = ax - c[0]; xcell[(size_t)i * 3 + 2] = ay - c[1];
      mem_add(-(long long)m.apx[i].size());
      std::vector<uint8_t>().swap(m.apx[i]);
    }
    po += (size_t)aw * ah * 4;
  }
  for (int i = 0; i < m.nf; i++) {
    int32_t v[10] = {m.rows[i * 2], m.rows[i * 2 + 1], cell[(size_t)i * 3], cell[(size_t)i * 3 + 1], cell[(size_t)i * 3 + 2],
                     xcell[(size_t)i * 3], xcell[(size_t)i * 3 + 1], xcell[(size_t)i * 3 + 2], m.v6 ? m.acrop[(size_t)i * 6 + 4] : 0, m.v6 ? m.acrop[(size_t)i * 6 + 5] : 0};
    std::memcpy(&b[32 + (size_t)i * rec], v, rec);
  }
}

static void drop_inputs(Map& m) {
  mem_add(-(long long)(m.frames.size() + m.tmv.size() + m.eyes.size()));
  m.frames.clear(); m.tmv.clear(); m.eyes.clear();
  std::vector<uint8_t>().swap(m.frames); std::vector<uint8_t>().swap(m.tmv); std::vector<uint8_t>().swap(m.eyes);
  for (auto& p : m.px) { mem_add(-(long long)p.size()); std::vector<uint8_t>().swap(p); }
  std::vector<std::vector<uint8_t>>().swap(m.px);
  for (auto* v : {&m.fpx, &m.apx}) { for (auto& p : *v) { mem_add(-(long long)p.size()); std::vector<uint8_t>().swap(p); } std::vector<std::vector<uint8_t>>().swap(*v); }
  mem_add(-(long long)(m.out0.size() + m.ids0.size() * 4));
  std::vector<uint8_t>().swap(m.out0); std::vector<int32_t>().swap(m.ids0);
}

struct Pool {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::shared_ptr<Map>> q;
  std::vector<std::pair<int, std::shared_ptr<Map>>> table;
  int started = 0, limit = 0, next_id = 1;
  std::shared_ptr<Map> find(int id) {
    for (auto& e : table) if (e.first == id) return e.second;
    return nullptr;
  }
  // kind 1 cache check, 2 frame f
  bool pick(int wi, std::shared_ptr<Map>& b, int& kind, int& f) {
    if (wi >= limit) return false;
    for (auto it = q.begin(); it != q.end();) {
      if ((*it)->cancel.load() || (*it)->state.load() >= 4) it = q.erase(it); else ++it;
    }
    for (int pass = 0; pass < 2; pass++)  // urgent maps (cut-in) first, then in queue order
      for (auto& x : q) {
        if ((x->urgent != 0) != (pass == 0)) continue;
        const int st = x->state.load();
        if (st == 1) { x->state = 2; b = x; kind = 1; return true; }
        if (st == 3 && x->next < x->nf) { b = x; kind = 2; f = x->next++; return true; }
      }
    return false;
  }
  void check(Map& m) {
    uint32_t c = crc_more(0, m.ip.data(), m.ip.size() * 4);
    c = crc_more(c, m.tmv.data(), m.tmv.size());
    c = crc_more(c, m.par.data(), m.par.size());
    c = crc_more(c, m.eyes.data(), m.eyes.size());
    c = crc_more(c, m.frames.data(), m.frames.size());
    m.key = c;
    if (load_cache(m)) { drop_inputs(m); m.hit = 1; m.done = m.nf; m.state = 4; return; }
    if (m.v6) {
      const int r = ref_frame(m);
      if (r) { m.state = r; return; }
    }
    m.state = 3;
  }
  void frame(Map& m, int f) {
    int ip[8] = {m.fw, m.fh, m.fw, m.k, m.sid, f, 0, 0};
    int err = 0;
    std::unique_ptr<tmap::Job> j(job_from(ip, &m.frames[(size_t)f * m.fsz()], m.tmv.data(), m.par.c_str(), &err));
    if (!j) { m.state = err ? err : 19; return; }
    if (m.v6) {
      j->stable_holes = true;
      if (f != 0 && !m.rfx.empty()) { j->ref_fx = &m.rfx; j->ref_fy = &m.rfy; j->ref_tm = &m.rtm; j->ref_cnt = &m.rcnt; }
    }
    const long long jm = (long long)j->N * 120;  // rough working set of one frame job (about 30 planes of W*H)
    mem_add(jm);
    while (!j->step(5.0)) if (m.cancel.load()) { mem_add(-jm); return; }
    std::vector<uint8_t> face;
    if (m.v6) { c0(m, f, *j, face); aux_frame(m, f); }
    const int r = finish(m, f, j->out, m.v6 ? &face : nullptr);
    j.reset();
    mem_add(-jm);
    if (r) { m.state = r; return; }
    if (m.done.fetch_add(1) + 1 == m.nf && m.state.load() == 3) {
      pack(m);
      drop_inputs(m);
      m.state = 4;
      if (!m.cancel.load()) save_cache(m);
    }
  }
  static void lower_priority() {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__) || defined(__ANDROID__)
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
  }
  void worker(int wi) {
    lower_priority();
    for (;;) {
      std::shared_ptr<Map> b; int kind = 0, f = 0;
      {
        std::unique_lock<std::mutex> lk(mu);
        while (!pick(wi, b, kind, f)) cv.wait(lk);
      }
      try {
        if (kind == 1) { check(*b); std::lock_guard<std::mutex> lk(mu); cv.notify_all(); }
        else frame(*b, f);
      } catch (...) { b->state = 19; }
    }
  }
  bool ensure(int want) {
    if (want < 1) {
      unsigned hc = std::thread::hardware_concurrency();
      want = hc > 1 ? (int)hc - 1 : 1;
      if (want > 3) want = 3;
    }
    if (want > limit) limit = want;
    try {
      while (started < limit) { std::thread t(&Pool::worker, this, started); t.detach(); started++; }
    } catch (...) { if (started == 0) return false; limit = started; }
    cv.notify_all();
    return true;
  }
};
static Pool& pool() { static Pool* p = new Pool(); return *p; }
}}  // namespace tmap::mp

extern "C" {
// ip = int32[12] {frame_w, frame_h, nf, k, sid, threads, tmv_len, eyes_len, grad*1e6, lr*1000, urgent (1 = before the others),
//                  flags (bit0 = v6 output TMA2: frame-0 facet attributes, facet atlases, aux atlases)}.
// handle > 0, 0 = refused
TMAP_EXPORT int tmap_map_new(const int32_t* ip, const uint8_t* tmv, const char* par, const uint8_t* eyes, const char* path) {
  using namespace tmap::mp;
  try {
    if (!ip || !tmv || !par) return 0;
    const int fw = ip[0], fh = ip[1], nf = ip[2], k = ip[3];
    if (fw <= 0 || fh <= 0 || nf <= 0 || nf > 4096 || k <= 0 || k > 8 || (int64_t)fw * k > 8192 || (int64_t)fh * k > 8192) return 0;
    if (ip[6] < 16 || ip[7] < 0 || (ip[7] > 0 && !eyes)) return 0;
    auto m = std::make_shared<Map>();
    m->fw = fw; m->fh = fh; m->nf = nf; m->k = k; m->sid = ip[4];
    m->grad = ip[8] / 1e6; m->lr = ip[9] / 1000.0;
    m->urgent = ip[10];
    m->ip.assign(ip, ip + 12); m->ip[5] = 0; m->ip[10] = 0;
    m->tmv.assign(tmv, tmv + ip[6]);
    if (ip[7] > 0) m->eyes.assign(eyes, eyes + ip[7]);
    m->par = par;
    if (path) m->path = path;
    m->frames.assign((size_t)nf * m->fsz(), 0);
    mem_add((long long)(m->frames.size() + m->tmv.size() + m->eyes.size()));
    m->have.assign(nf, 0);
    m->rows.assign((size_t)nf * 2, 0); m->crop.assign((size_t)nf * 4, 0); m->px.resize(nf);
    m->v6 = (ip[11] & 1) != 0;
    if (m->v6) {
      if (!tmv_mesh(*m)) return 0;
      m->fpx.resize(nf); m->apx.resize(nf); m->acrop.assign((size_t)nf * 6, 0);
    }
    Pool& P = pool();
    std::lock_guard<std::mutex> lk(P.mu);
    if (!P.ensure(ip[5])) return 0;
    m->id = P.next_id; P.next_id = P.next_id >= 0x3fffffff ? 1 : P.next_id + 1;
    P.table.push_back({m->id, m});
    return m->id;
  } catch (...) { return 0; }
}
TMAP_EXPORT int tmap_map_add(int h, int f, const uint8_t* rgba) {
  using namespace tmap::mp;
  Pool& P = pool();
  std::shared_ptr<Map> m;
  { std::lock_guard<std::mutex> lk(P.mu); m = P.find(h); }
  if (!m || !rgba) return 11;
  if (m->state.load() != 0 || f < 0 || f >= m->nf) return 13;
  std::memcpy(&m->frames[(size_t)f * m->fsz()], rgba, m->fsz());
  m->have[f] = 1;
  return 0;
}
TMAP_EXPORT int tmap_map_go(int h) {
  using namespace tmap::mp;
  Pool& P = pool();
  std::lock_guard<std::mutex> lk(P.mu);
  std::shared_ptr<Map> m = P.find(h);
  if (!m) return 11;
  if (m->state.load() != 0) return 13;
  for (uint8_t v : m->have) if (!v) return 13;
  m->state = 1;
  P.q.push_back(m);
  P.cv.notify_all();
  return 0;
}
// 0 running, 1 done, >= 11 error (13 bad call, 19 out of memory, 31 eye bits short; job errors 11..14)
// io int32[4] {frames done, nf, blob bytes, from cache}
TMAP_EXPORT int tmap_map_poll(int h, int32_t* io) {
  using namespace tmap::mp;
  Pool& P = pool();
  std::shared_ptr<Map> m;
  { std::lock_guard<std::mutex> lk(P.mu); m = P.find(h); }
  if (!m) return 11;
  const int st = m->state.load();
  if (io) { io[0] = m->done.load(); io[1] = m->nf; io[2] = st == 4 ? (int)m->blob.size() : 0; io[3] = m->hit.load(); }
  return st == 4 ? 1 : (st >= 11 ? st : 0);
}
TMAP_EXPORT int tmap_map_read(int h, int off, int len, uint8_t* out) {
  using namespace tmap::mp;
  if (!out || off < 0 || len < 0) return 11;
  Pool& P = pool();
  std::shared_ptr<Map> m;
  { std::lock_guard<std::mutex> lk(P.mu); m = P.find(h); }
  if (!m) return 11;
  if (m->state.load() != 4) return 14;
  if ((size_t)off + (size_t)len > m->blob.size()) return 13;
  std::memcpy(out, m->blob.data() + off, len);
  return 0;
}
// cancels if still running
TMAP_EXPORT int tmap_map_free(int h) {
  using namespace tmap::mp;
  Pool& P = pool();
  std::shared_ptr<Map> m;
  {
    std::lock_guard<std::mutex> lk(P.mu);
    for (size_t i = 0; i < P.table.size(); i++)
      if (P.table[i].first == h) { m = P.table[i].second; m->cancel = true; P.table.erase(P.table.begin() + i); break; }
  }
  return m ? 0 : 11;  // the blob goes with the last holder (a worker may still be writing the cache file)
}
// bytes the map threads hold now / at most since start (inputs, crops, blobs, cache buffers, rough job planes)
TMAP_EXPORT int tmap_map_mem(int32_t* io) {
  if (!io) return 11;
  using namespace tmap::mp;
  long long a = g_mem.load(), b = g_peak.load();
  io[0] = (int32_t)(a >> 10); io[1] = (int32_t)(b >> 10);
  return 0;
}
}
