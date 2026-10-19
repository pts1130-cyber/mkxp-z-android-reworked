/* arcfog — Rogue Aura(폭주 안개)를 칸 해상도 RGBA 그림으로 그리는 네이티브 라이브러리.
 * 현재 프레임 알파에서 윤곽 → 거리 → 번짐/일렁임/그라데이션까지 한 번에 계산해 out 에 쓴다.
 * 루비 쪽(ARCRogueFogFlow)과 같은 식을 쓴다. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

#define ARCFOG_VERSION 1
#define GRAD 4
#define LEVELS 12
#define W1 1.5
#define W2 1.2
#define CUT 0.03
#define REF_H 316.0
#define TABLE_STEP 0.05
#define SLOTS 4

static const double TOP[3] = {150.0, 60.0, 235.0};
static const double LOW[3] = {255.0, 70.0, 215.0};
static const double KX[3] = {0.09, 0.075, 0.10};
static const double KY[3] = {-0.02, 0.03, -0.04};
static const double PHS[3] = {1.1, 3.9, 5.2};

typedef struct {
  int used;
  int gw, gh, mg, g;
  double sc, s1, s2;
  float *amp, *ph;     /* gw*gh */
  float *tab;          /* base(dd) 표 */
  int tabn;
  float *d;            /* 거리 작업장 gw*gh */
  unsigned char *solid; /* gw1*gh1 작업장 */
  int gw1, gh1;
  unsigned long stamp;
} Slot;

static Slot slots[SLOTS];
static unsigned long tick = 0;

static void slot_free(Slot *s) {
  free(s->amp); free(s->ph); free(s->tab); free(s->d); free(s->solid);
  memset(s, 0, sizeof(Slot));
}

static Slot *slot_get(int gw, int gh, int mg, int g, double sc, double s1, double s2, int gw1, int gh1) {
  int i, victim = 0;
  unsigned long oldest = (unsigned long)-1;
  for (i = 0; i < SLOTS; i++) {
    Slot *s = &slots[i];
    if (s->used && s->gw == gw && s->gh == gh && s->mg == mg && s->g == g &&
        s->sc == sc && s->s1 == s1 && s->s2 == s2 && s->gw1 == gw1 && s->gh1 == gh1) {
      s->stamp = ++tick;
      return s;
    }
  }
  for (i = 0; i < SLOTS; i++) {
    if (!slots[i].used) { victim = i; oldest = 0; break; }
    if (slots[i].stamp < oldest) { oldest = slots[i].stamp; victim = i; }
  }
  {
    Slot *s = &slots[victim];
    long n = (long)gw * gh;
    int k, r, c;
    double sq2 = sqrt(2.0);
    if (s->used) slot_free(s);
    s->amp = (float *)malloc(sizeof(float) * n);
    s->ph = (float *)malloc(sizeof(float) * n);
    s->d = (float *)malloc(sizeof(float) * n);
    s->solid = (unsigned char *)malloc((size_t)gw1 * gh1);
    s->tabn = (int)ceil((2.6 * s2 + 2.0) / TABLE_STEP) + 2;
    s->tab = (float *)malloc(sizeof(float) * s->tabn);
    if (!s->amp || !s->ph || !s->d || !s->solid || !s->tab) { slot_free(s); return NULL; }
    for (k = 0; k < s->tabn; k++) {
      double dd = k * TABLE_STEP;
      s->tab[k] = (float)(W1 * 0.5 * erfc(dd / (s1 * sq2)) + W2 * 0.5 * erfc(dd / (s2 * sq2)));
    }
    for (r = 0; r < gh; r++) {
      for (c = 0; c < gw; c++) {
        double xf = (double)(c - mg) * g, yf = (double)(r - mg) * g;
        double re = 0.0, im = 0.0;
        for (k = 0; k < 3; k++) {
          double a = (KX[k] / sc * xf) + (KY[k] / sc * yf) + PHS[k];
          re += cos(a); im += sin(a);
        }
        s->amp[(long)r * gw + c] = (float)(sqrt(re * re + im * im) / 3.0);
        s->ph[(long)r * gw + c] = (float)atan2(im, re);
      }
    }
    s->gw = gw; s->gh = gh; s->mg = mg; s->g = g; s->sc = sc; s->s1 = s1; s->s2 = s2;
    s->gw1 = gw1; s->gh1 = gh1; s->used = 1; s->stamp = ++tick;
    return s;
  }
}

EXPORT int arcfog_version(void) { return ARCFOG_VERSION; }

/* dp: [0]=시각(초) [1]=주기(초) [2]=방향(-1:3시, 1:9시) [3]=sc [4]=s1 [5]=s2
 * ip: [0]=sw [1]=sh [2]=rx [3]=ry [4]=rw [5]=rh [6]=알파자리 [7]=위아래뒤집힘 [8..10]=R,G,B 자리
 *     [11]=step [12]=g [13]=gw1 [14]=gh1 [15]=mg [16]=gw [17]=gh [18]=rep(칸 하나를 rep×rep 점으로 씀)
 * src: 현재 프레임 RGBA(sw*sh*4) / out: (gw*rep)*(gh*rep)*4 (전부 덮어씀)
 * 반환: 칠한 칸 수(0 이상) / 음수 = 오류 */
EXPORT int arcfog_paint(const double *dp, const int *ip, const unsigned char *src, unsigned char *out) {
  int sw = ip[0], sh = ip[1], rx = ip[2], ry = ip[3], rw = ip[4], rh = ip[5];
  int ai = ip[6], flip = ip[7], ri = ip[8], gi_ = ip[9], bi = ip[10];
  int step = ip[11], g = ip[12], gw1 = ip[13], gh1 = ip[14], mg = ip[15], gw = ip[16], gh = ip[17], rp = ip[18];
  double t = dp[0], period = dp[1], dir = dp[2], sc = dp[3], s1 = dp[4], s2 = dp[5];
  int half, r, c, rmin = -1, rmax = -1, painted = 0;
  long n;
  Slot *s;
  float *d;
  const float INF = 1.0e9f, DG = 1.4142f;
  double bh, top, omega, th, pulse;

  if (!dp || !ip || !src || !out) return -1;
  if (sw <= 0 || sh <= 0 || gw1 <= 0 || gh1 <= 0 || gw != gw1 + 2 * mg || gh != gh1 + 2 * mg || step < 1 || g < 1 || rp < 1) return -1;
  if (sc <= 0.0 || s1 <= 0.0 || s2 <= 0.0 || period <= 0.0) return -1;
  n = (long)gw * gh;
  memset(out, 0, (size_t)n * rp * rp * 4);
  s = slot_get(gw, gh, mg, g, sc, s1, s2, gw1, gh1);
  if (!s) return -2;
  d = s->d;
  half = step / 2;
  for (r = 0; r < gh; r++) for (c = 0; c < gw; c++) d[(long)r * gw + c] = INF;
  for (r = 0; r < gh1; r++) {
    int yy = ry + (r * g * step) + half;
    if (yy >= ry + rh || yy >= sh) continue;
    for (c = 0; c < gw1; c++) {
      int xx = rx + (c * g * step) + half;
      if (xx >= rx + rw || xx >= sw) continue;
      if (src[((long)yy * sw + xx) * 4 + ai] > 128) {
        d[(long)(r + mg) * gw + (c + mg)] = 0.0f;
        if (rmin < 0) rmin = r;
        rmax = r;
      }
    }
  }
  if (rmin < 0) return 0;
  /* 거리(체스·대각 근사, 루비와 같은 방식) */
  for (r = 0; r < gh; r++) {
    for (c = 0; c < gw; c++) {
      long i = (long)r * gw + c;
      float di = d[i], v;
      if (c > 0) { v = d[i - 1] + 1.0f; if (v < di) di = v; }
      if (r > 0) {
        long j = i - gw;
        v = d[j] + 1.0f; if (v < di) di = v;
        if (c > 0) { v = d[j - 1] + DG; if (v < di) di = v; }
        if (c < gw - 1) { v = d[j + 1] + DG; if (v < di) di = v; }
      }
      d[i] = di;
    }
  }
  for (r = gh - 1; r >= 0; r--) {
    for (c = gw - 1; c >= 0; c--) {
      long i = (long)r * gw + c;
      float di = d[i], v;
      if (c < gw - 1) { v = d[i + 1] + 1.0f; if (v < di) di = v; }
      if (r < gh - 1) {
        long j = i + gw;
        v = d[j] + 1.0f; if (v < di) di = v;
        if (c > 0) { v = d[j - 1] + DG; if (v < di) di = v; }
        if (c < gw - 1) { v = d[j + 1] + DG; if (v < di) di = v; }
      }
      d[i] = di;
    }
  }
  bh = (double)(rmax - rmin + 1) * g;
  top = (double)rmin * g;
  omega = 6.283185307179586 / period;
  th = omega * t * dir;
  pulse = 0.85 + 0.15 * sin(t * 6.283185307179586 / period);
  for (r = 0; r < gh; r++) {
    int orow = flip ? (gh - 1 - r) : r;
    double yf = (double)(r - mg) * g;
    double low = (yf - (top + 0.38 * bh)) / (0.63 * bh);
    int gi;
    double col[3];
    if (low < 0.0) low = 0.0;
    if (low > 1.0) low = 1.0;
    gi = (int)floor(low * (GRAD - 1) + 0.5);
    col[0] = TOP[0] + (LOW[0] - TOP[0]) * gi / (GRAD - 1.0);
    col[1] = TOP[1] + (LOW[1] - TOP[1]) * gi / (GRAD - 1.0);
    col[2] = TOP[2] + (LOW[2] - TOP[2]) * gi / (GRAD - 1.0);
    for (c = 0; c < gw; c++) {
      long i = (long)r * gw + c;
      double dd, f, base, rag, a;
      int k, lv;
      if (d[i] <= 0.0f) continue;
      dd = d[i] - 0.5; if (dd < 0.0) dd = 0.0;
      f = dd / TABLE_STEP;
      k = (int)f;
      if (k >= s->tabn - 1) continue;
      base = s->tab[k] + (s->tab[k + 1] - s->tab[k]) * (f - k);
      if (base < CUT) continue;
      rag = 0.9 + 1.12 * s->amp[i] * sin(th + s->ph[i]);
      if (rag < 0.6) rag = 0.6;
      if (rag > 1.0) rag = 1.0;
      a = base * rag * pulse;
      if (a > 1.0) a = 1.0;
      lv = (int)floor(a * LEVELS + 0.5);
      if (lv <= 0) continue;
      {
        unsigned char vr = (unsigned char)floor(col[0] + 0.5);
        unsigned char vg = (unsigned char)floor(col[1] + 0.5);
        unsigned char vb = (unsigned char)floor(col[2] + 0.5);
        unsigned char va = (unsigned char)floor(lv * 255.0 / LEVELS + 0.5);
        int dy, dx;
        long rowlen = (long)gw * rp;
        for (dy = 0; dy < rp; dy++) {
          unsigned char *p = out + (((long)orow * rp + dy) * rowlen + (long)c * rp) * 4;
          for (dx = 0; dx < rp; dx++, p += 4) {
            p[ri] = vr; p[gi_] = vg; p[bi] = vb; p[ai] = va;
          }
        }
      }
      painted++;
    }
  }
  return painted;
}

EXPORT void arcfog_release(void) {
  int i;
  for (i = 0; i < SLOTS; i++) if (slots[i].used) slot_free(&slots[i]);
}
