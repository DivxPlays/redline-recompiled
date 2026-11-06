/* Button prompts for the device in use. The game draws the original Xbox controller's buttons from one 256x64 DXT5
 * atlas of 18x18 glyphs (found by its texture-pack key, below), the same glyph wherever a prompt names a button.
 * While the keyboard was the last thing used its button glyphs become key caps with the menu keys bound to them; with
 * a PlayStation pad its symbols and names; with an Xbox One/Series pad the names of the buttons standing in for the
 * original's Black, White, Start, Back and triggers. The replacement is the atlas at 4x, those cells redrawn with GDI
 * (4x4 supersampled), made again whenever the device or a key binding changes.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "icons.h"

#define ATLAS_KEY 0xBEB57C2ECC2E8E2Full /* the prompt glyphs (D:\MEDIA\Fonts are its neighbours) */
#define SCALE 4                          /* the replacement's size over the original's */
#define SS 4                             /* supersampling of the GDI drawing */

/* the glyph cells (x0, y0, x1, y1 in the 256x64 atlas) of the buttons a device can rename */
static const struct {
    int button, x0, y0, x1, y1;
} g_cells[] = {
    {BTN_A, 7, 2, 25, 20},       {BTN_B, 28, 2, 46, 20},       {BTN_X, 49, 2, 67, 20},
    {BTN_Y, 70, 2, 88, 20},      {BTN_LT, 93, 2, 107, 20},     {BTN_RT, 114, 2, 128, 20},
    {BTN_BACK, 133, 2, 151, 20}, {BTN_START, 154, 2, 172, 20}, {BTN_BLACK, 175, 2, 193, 20},
    {BTN_WHITE, 196, 2, 214, 20}, {BTN_LEFT, 217, 2, 235, 20},  {BTN_DOWN, 2, 23, 20, 41},
    {BTN_RIGHT, 23, 23, 41, 41}, {BTN_UP, 44, 23, 62, 41},
};
#define NCELLS (int)(sizeof g_cells / sizeof g_cells[0])

static uint32_t *g_atlas, g_aw, g_ah; /* the original, BGRA */

int icons_is_prompt(uint64_t key) { return key == ATLAS_KEY; }

void icons_original(uint64_t key, const TexImage *img) {
    if (key != ATLAS_KEY || g_atlas) return;
    g_atlas = malloc((size_t)img->width * img->height * 4);
    g_aw = img->width, g_ah = img->height;
    texture_bgra(img, g_atlas);
}

typedef struct {
    HDC dc;
    uint32_t *px;
    int w, h;
} Canvas;

static void canvas(Canvas *c, int w, int h) {
    c->w = w, c->h = h;
    c->dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
    SelectObject(c->dc, CreateDIBSection(c->dc, &bi, DIB_RGB_COLORS, (void **)&c->px, NULL, 0));
    memset(c->px, 0, (size_t)w * h * 4);
    SetBkMode(c->dc, TRANSPARENT);
}

static void canvas_free(Canvas *c) {
    HBITMAP bm = (HBITMAP)GetCurrentObject(c->dc, OBJ_BITMAP);
    DeleteDC(c->dc);
    DeleteObject(bm);
}

static void fill(HDC dc, COLORREF c, int ellipse, int x0, int y0, int x1, int y1, int r) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    if (ellipse) Ellipse(dc, x0, y0, x1, y1);
    else RoundRect(dc, x0, y0, x1, y1, r, r);
    SelectObject(dc, ob), SelectObject(dc, op);
    DeleteObject(b), DeleteObject(p);
}

static void stroke(HDC dc, COLORREF c, int width, const POINT *pts, int n, int closed) {
    LOGBRUSH lb = {BS_SOLID, c, 0};
    HPEN p = ExtCreatePen(PS_SOLID | PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, width, &lb, 0, NULL);
    HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (closed) Polygon(dc, pts, n);
    else Polyline(dc, pts, n);
    SelectObject(dc, op), SelectObject(dc, ob);
    DeleteObject(p);
}

/* text centred in the box, as large as fits */
static void label(HDC dc, const wchar_t *t, COLORREF c, int x0, int y0, int x1, int y1) {
    for (int size = (y1 - y0) * 9 / 10;; size = size * 9 / 10) {
        HFONT f = CreateFontW(-size, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0,
                              L"Segoe UI Symbol");
        HGDIOBJ of = SelectObject(dc, f);
        SIZE sz;
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        int ink = tm.tmAscent - tm.tmInternalLeading; /* cap height, roughly */
        if ((sz.cx <= x1 - x0 && ink <= y1 - y0) || size < 6) {
            SetTextColor(dc, c);
            TextOutW(dc, (x0 + x1 - sz.cx) / 2, (y0 + y1 - ink) / 2 - tm.tmInternalLeading, t, (int)wcslen(t));
            SelectObject(dc, of), DeleteObject(f);
            return;
        }
        SelectObject(dc, of), DeleteObject(f);
    }
}

/* one button for a device in a w x h box: colour, or with `mask` its coverage in white */
static void draw(HDC dc, int mask, int device, int button, int w, int h) {
    int s = w < h ? w : h, cx = w / 2, cy = h / 2, r = s / 2 - s / 24;
#define COL(rgb) (mask ? RGB(255, 255, 255) : (rgb))
    if (device == DEV_KEYBOARD) { /* a key cap with the bound key's name */
        wchar_t name[16];
        icons_key_name(button, name, 16);
        int x0 = cx - r, x1 = cx + r, y0 = cy - r, y1 = cy + r, rr = s / 3;
        fill(dc, COL(RGB(30, 30, 30)), 0, x0, y0, x1, y1, rr);
        fill(dc, COL(RGB(140, 140, 140)), 0, x0 + s / 14, y0 + s / 14, x1 - s / 14, y1 - s / 14, rr);
        fill(dc, COL(RGB(245, 245, 245)), 0, x0 + s / 14, y0 + s / 14, x1 - s / 14, y1 - s / 5, rr);
        if (!mask) label(dc, name, RGB(15, 15, 15), x0 + s / 7, y0 + s / 6, x1 - s / 7, y1 - s / 4);
        return;
    }
    if (device == DEV_PLAYSTATION && button <= BTN_Y) { /* the face buttons: a dark disc and the coloured symbol */
        fill(dc, COL(RGB(235, 235, 235)), 1, cx - r, cy - r, cx + r, cy + r, 0);
        fill(dc, COL(RGB(25, 25, 25)), 1, cx - r + s / 14, cy - r + s / 14, cx + r - s / 14, cy + r - s / 14, 0);
        int k = r * 9 / 20, t = s / 8;
        if (button == BTN_A) {
            POINT a[2] = {{cx - k, cy - k}, {cx + k, cy + k}}, b[2] = {{cx + k, cy - k}, {cx - k, cy + k}};
            stroke(dc, COL(RGB(124, 178, 232)), t, a, 2, 0);
            stroke(dc, COL(RGB(124, 178, 232)), t, b, 2, 0);
        } else if (button == BTN_B) {
            POINT o[48];
            for (int i = 0; i < 48; i++)
                o[i] = (POINT){cx + (int)(k * 1.15 * cos(i * 6.2831853 / 48)), cy + (int)(k * 1.15 * sin(i * 6.2831853 / 48))};
            stroke(dc, COL(RGB(255, 102, 102)), t, o, 48, 1);
        } else if (button == BTN_X) {
            POINT q[4] = {{cx - k, cy - k}, {cx + k, cy - k}, {cx + k, cy + k}, {cx - k, cy + k}};
            stroke(dc, COL(RGB(255, 105, 248)), t, q, 4, 1);
        } else {
            POINT tri[3] = {{cx, cy - k * 6 / 5}, {cx + k * 11 / 10, cy + k * 4 / 5}, {cx - k * 11 / 10, cy + k * 4 / 5}};
            stroke(dc, COL(RGB(64, 226, 160)), t, tri, 3, 1);
        }
        return;
    }
    /* shoulder, trigger and centre buttons: a dark rounded plate with the name */
    static const wchar_t *const ps[BTN_COUNT] = {[BTN_BLACK] = L"R1", [BTN_WHITE] = L"L1", [BTN_LT] = L"L2", [BTN_RT] = L"R2",
                                                 [BTN_START] = L"\x2261", [BTN_BACK] = L"SH"};
    static const wchar_t *const xb[BTN_COUNT] = {[BTN_BLACK] = L"RB", [BTN_WHITE] = L"LB", [BTN_LT] = L"LT", [BTN_RT] = L"RT",
                                                 [BTN_START] = L"\x2261", [BTN_BACK] = L"\x29C9"};
    const wchar_t *name = (device == DEV_PLAYSTATION ? ps : xb)[button];
    fill(dc, COL(RGB(235, 235, 235)), 0, cx - r, cy - r, cx + r, cy + r, s / 3);
    fill(dc, COL(RGB(30, 30, 30)), 0, cx - r + s / 14, cy - r + s / 14, cx + r - s / 14, cy + r - s / 14, s / 3);
    if (!mask && name) label(dc, name, RGB(255, 255, 255), cx - r + s / 6, cy - r + s / 5, cx + r - s / 6, cy + r - s / 5);
#undef COL
}

/* which cells a device redraws: the keyboard all, the PlayStation all but the d-pad, Xbox One/Series pads the
 * buttons the original Xbox pad names differently */
static int redrawn(int device, int button) {
    if (device == DEV_KEYBOARD) return 1;
    if (device == DEV_PLAYSTATION) return button < BTN_LEFT;
    return device == DEV_XBOX && button >= BTN_LT && button < BTN_LEFT;
}

static ID3D11ShaderResourceView *make(ID3D11Device *dev, int device) {
    uint32_t w = g_aw * SCALE, h = g_ah * SCALE, *out = malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) /* the original, bilinear, colour weighted by alpha */
        for (uint32_t x = 0; x < w; x++) {
            float fx = (x + 0.5f) / SCALE - 0.5f, fy = (y + 0.5f) / SCALE - 0.5f, acc[4] = {0}, wsum = 0;
            int ix = (int)floorf(fx), iy = (int)floorf(fy);
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    int sx = ix + i < 0 ? 0 : ix + i >= (int)g_aw ? g_aw - 1 : ix + i;
                    int sy = iy + j < 0 ? 0 : iy + j >= (int)g_ah ? g_ah - 1 : iy + j;
                    float k = (i ? fx - ix : 1 - (fx - ix)) * (j ? fy - iy : 1 - (fy - iy));
                    uint32_t p = g_atlas[sy * g_aw + sx];
                    float a = (p >> 24) / 255.0f;
                    acc[0] += (p & 0xFF) * a * k, acc[1] += ((p >> 8) & 0xFF) * a * k, acc[2] += ((p >> 16) & 0xFF) * a * k;
                    acc[3] += a * k, wsum += k;
                }
            uint32_t a = (uint32_t)(acc[3] / wsum * 255 + 0.5f);
            out[y * w + x] = acc[3] > 0 ? a << 24 | (uint32_t)(acc[2] / acc[3]) << 16 | (uint32_t)(acc[1] / acc[3]) << 8 |
                                              (uint32_t)(acc[0] / acc[3])
                                        : 0;
        }
    for (int c = 0; c < NCELLS; c++) {
        if (!redrawn(device, g_cells[c].button)) continue;
        int x0 = g_cells[c].x0 * SCALE, y0 = g_cells[c].y0 * SCALE, cw = (g_cells[c].x1 - g_cells[c].x0) * SCALE,
            ch = (g_cells[c].y1 - g_cells[c].y0) * SCALE;
        if (cw < ch) x0 -= (ch - cw) / 2, cw = ch; /* the trigger squares are narrower: centred square boxes */
        Canvas col, cov;
        canvas(&col, cw * SS, ch * SS);
        canvas(&cov, cw * SS, ch * SS);
        draw(col.dc, 0, device, g_cells[c].button, cw * SS, ch * SS);
        draw(cov.dc, 1, device, g_cells[c].button, cw * SS, ch * SS);
        GdiFlush();
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++) { /* box filter: colour weighted by coverage, straight alpha */
                uint32_t a = 0, r = 0, g = 0, b = 0;
                for (int j = 0; j < SS; j++)
                    for (int i = 0; i < SS; i++) {
                        size_t o = (size_t)(y * SS + j) * cw * SS + x * SS + i;
                        uint32_t m = cov.px[o] & 0xFF, p = col.px[o];
                        a += m, r += ((p >> 16) & 0xFF) * m, g += ((p >> 8) & 0xFF) * m, b += (p & 0xFF) * m;
                    }
                int ox = x0 + x, oy = y0 + y;
                if (ox >= 0 && ox < (int)w && oy >= 0 && oy < (int)h)
                    out[oy * w + ox] = a ? (a / (SS * SS)) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a) : 0;
            }
        canvas_free(&col), canvas_free(&cov);
    }
    D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                               D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA init = {out, w * 4, 0};
    ID3D11Texture2D *t;
    ID3D11ShaderResourceView *srv = NULL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, &init, &t)))
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t, NULL, &srv), ID3D11Texture2D_Release(t);
    free(out);
    return srv;
}

ID3D11ShaderResourceView *icons_get(ID3D11Device *dev, uint64_t key, int state) {
    int device = state & 15;
    if (key != ATLAS_KEY || !g_atlas || device == DEV_OTHER) return NULL; /* the game's own */
    static int made_for = -1;
    static ID3D11ShaderResourceView *srv;
    if (made_for != state) {
        if (srv) ID3D11ShaderResourceView_Release(srv); /* the renderer holds its own reference */
        srv = make(dev, device);
        made_for = state;
    }
    return srv;
}
