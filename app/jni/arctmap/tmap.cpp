// Tera body material map builder (port of build.py make_map). C++17, no dependencies.
// Build with -ffp-contract=off so float32/float64 steps round like numpy (matters on arm64, which fuses by default).
#include "tmap.h"
#include <algorithm>
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
    F(white_frac) F(white_lev) F(white_comp) F(keep_small) F(keep_w) F(dark_frac) F(dark_lev) F(dark_w)
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
  static int sdiv[256], hdiv[256]; static bool init = false;
  const int sh = 12;
  if (!init) {
    sdiv[0] = hdiv[0] = 0;
    for (int i = 1; i < 256; i++) {
      sdiv[i] = (int)std::lround((255 << sh) / (1. * i));
      hdiv[i] = (int)std::lround((180 << sh) / (6. * i));
    }
    init = true;
  }
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
        for (int I : bp) if (lab[I] >= 0) ids[I] = nt0 + rank[lab[I]];
        nt = nt0 + nc;
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
      if (P.dark_frac > 0 && df[id]) { dark[I] = 1; lev[I] = std::min(lev[I], P.dark_lev); }
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
  *err = 12;
  if (std::memcmp(tmv, "TMV1", 4) != 0) return nullptr;
  uint16_t hk, nv, nt, nf; std::memcpy(&hk, tmv + 4, 2); std::memcpy(&nv, tmv + 6, 2); std::memcpy(&nt, tmv + 8, 2); std::memcpy(&nf, tmv + 10, 2);
  *err = 13;
  if (hk != k || k <= 0 || fw <= 0 || h <= 0 || fi < 0 || fi >= nf || sw < fw * (fi + 1)) return nullptr;
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
  for (int y = 0; y < h; y++) std::memcpy(&fr[(size_t)y * fw * 4], rgba + ((size_t)y * sw + (size_t)fi * fw) * 4, (size_t)fw * 4);
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
TMAP_EXPORT void tm_resize_f32(const float* s, int w, int h, int k, float* d) { tmap::resize_linear_f32(s, w, h, k, d); }
TMAP_EXPORT void tm_gauss_f32(float* a, int w, int h, double s) { tmap::gauss_f32(a, w, h, s); }
TMAP_EXPORT void tm_gauss_f64(double* a, int w, int h, double s) { tmap::gauss_f64(a, w, h, s); }
TMAP_EXPORT void tm_edt(const uint8_t* m, int w, int h, double* o) { tmap::edt(m, w, h, o); }
TMAP_EXPORT void tm_fill(const float* v, int nt, const int32_t* tri, int W, int H, int32_t* ids) { tmap::fill_tris(v, nt, tri, nullptr, W, H, ids); }
TMAP_EXPORT void tm_hsv(const uint8_t* rgb, int n, uint8_t* hsv) { tmap::rgb2hsv8(rgb, 3, n, hsv); }
TMAP_EXPORT double tm_hmix(unsigned a, unsigned b, unsigned c) { return tmap::hmix(a, b, c); }
}
