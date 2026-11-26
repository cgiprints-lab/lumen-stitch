// Shared tile-capture geometry for Lumen gigapixel capture.
//
// Used by BOTH the in-game capture adapter (to aim tiles) and the stitcher (to
// recombine them), so the models cannot drift apart. World convention matches
// Dusklight: Y-up, yaw about +Y, pitch from the XZ plane (verified in
// d_camera.cpp: mUp.set(0,1,0)).
//
// Model: a "master" pinhole camera (eye, yaw, pitch, vertical FOV in degrees,
// viewport aspect) defines the final image, rendered N times larger than the
// viewport as a grid of S x S tiles. Every tile shares the master EYE (pure
// rotation, no translation parallax) and is itself a pinhole image:
//   - its axis points at the master-pixel center of its sub-rect,
//   - its FOV is the tightest that covers the sub-rect at the viewport aspect.
// Because all cameras share a center, tile <-> master correspondence is exact
// projective geometry: every master ray maps to a tile pixel analytically,
// which is what makes the FOV=90 panorama test line up seamlessly (Ansel's
// alignment contract).
//
// Continuous pixel coordinates: pixel i spans [i, i+1), its center is i+0.5.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lumen::tilemath {

inline constexpr double kPi = 3.14159265358979323846;
inline double rad(double deg) { return deg * kPi / 180.0; }
inline double deg(double r) { return r * 180.0 / kPi; }

inline double dot(const double a[3], const double b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Orthonormal basis for (yaw, pitch): forward, right, up.
inline void basis(double yaw, double pitch, double f[3], double r[3], double u[3]) {
    const double cy = std::cos(yaw), sy = std::sin(yaw);
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    f[0] = cp * sy; f[1] = sp; f[2] = cp * cy;
    r[0] = cy; r[1] = 0.0; r[2] = -sy;
    // up = f x r (unit)
    u[0] = -sp * sy; u[1] = cp; u[2] = -sp * cy;
}

// Master camera parameters.
struct Master {
    double yaw = 0;      // radians
    double pitch = 0;    // radians
    double fovy = 90;    // degrees, vertical
    uint32_t vw = 0;     // native viewport width
    uint32_t vh = 0;     // native viewport height
    uint32_t width = 0;  // final image width  (vw * scale)
    uint32_t height = 0; // final image height (vh * scale)
};

// Ray (unnormalized) through continuous master pixel (x, y).
inline void master_ray(const Master& m, double x, double y, double d[3]) {
    double f[3], r[3], u[3];
    basis(m.yaw, m.pitch, f, r, u);
    const double hy = std::tan(rad(m.fovy) / 2.0);
    const double hx = hy * static_cast<double>(m.vw) / static_cast<double>(m.vh);
    const double mx = ((x + 0.5) / static_cast<double>(m.width) * 2.0 - 1.0) * hx;
    const double my = (1.0 - (y + 0.5) / static_cast<double>(m.height) * 2.0) * hy;
    d[0] = f[0] + mx * r[0] + my * u[0];
    d[1] = f[1] + mx * r[1] + my * u[1];
    d[2] = f[2] + mx * r[2] + my * u[2];
}

// One captured tile.
struct Tile {
    int row = 0, col = 0;
    double yaw = 0;    // radians
    double pitch = 0;  // radians
    double fovy = 90;  // degrees
    uint32_t w = 0, h = 0;
    // Master-pixel sub-rect owned by this tile: [x0, x1) x [y0, y1).
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Build the tile that owns master sub-rect [x0,x1) x [y0,y1), rendered at
// native viewport size (m.vw x m.vh). Its axis hits the rect center; its FOV
// covers the rect EXPANDED by `margin_native` tile pixels (clamped at the
// master bounds). The expansion guarantees that every owned pixel maps
// strictly inside the tile image with a full bilinear stencil - seam pixels
// can interpolate from the tile that genuinely rendered the ray, which is
// what keeps adjacent tiles continuous across the boundary. (The max corner
// angular extent bounds the whole rect: the tan-extent is fractional-linear
// and hence maximized at the corners.)
inline Tile make_tile(const Master& m, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
    int row, int col, int margin_native = 4) {
    Tile t;
    t.row = row;
    t.col = col;
    t.x0 = x0;
    t.y0 = y0;
    t.x1 = x1;
    t.y1 = y1;
    t.w = m.vw;
    t.h = m.vh;

    double c[3];
    master_ray(m, (static_cast<double>(x0) + x1) / 2.0, (static_cast<double>(y0) + y1) / 2.0, c);
    t.yaw = std::atan2(c[0], c[2]);
    t.pitch = std::atan2(c[1], std::sqrt(c[0] * c[0] + c[2] * c[2]));
    // Keep the tile pose inside the camera operator's flight clamp so the
    // game camera can represent it.
    if (t.pitch > 1.55) t.pitch = 1.55;
    if (t.pitch < -1.55) t.pitch = -1.55;

    double ft[3], rt[3], ut[3];
    basis(t.yaw, t.pitch, ft, rt, ut);
    const double aspect = static_cast<double>(m.vw) / static_cast<double>(m.vh);
    // Margin in master pixels: the rect is vw x vh rendered at vw x vh... the
    // master grid is `scale = W / vw` master pixels per tile pixel.
    const double per = static_cast<double>(m.width) / static_cast<double>(m.vw);
    const double mx = margin_native * per;
    const double xs[4] = {x0 >= mx ? x0 - mx : 0.0, x1 + mx, x0 >= mx ? x0 - mx : 0.0, x1 + mx};
    const double ys[4] = {y0 >= mx ? y0 - mx : 0.0, y0 >= mx ? y0 - mx : 0.0, y1 + mx, y1 + mx};
    double max_tu = 0.0, max_tr = 0.0;
    for (int i = 0; i < 4; ++i) {
        // Clamp the expansion at the outer master bounds; pixels outside the
        // master image are never sampled, and image-edge clamp handles the
        // border stencil there.
        const double xc = xs[i] > static_cast<double>(m.width) ? static_cast<double>(m.width) : xs[i];
        const double yc = ys[i] > static_cast<double>(m.height) ? static_cast<double>(m.height) : ys[i];
        double d[3];
        master_ray(m, xc, yc, d);
        const double den = dot(d, ft);
        if (den <= 1e-9)
            continue;  // rect edge at/behind the axis: prevented by the pitch clamp
        const double tr = dot(d, rt) / den;
        const double tu = dot(d, ut) / den;
        max_tu = std::max(max_tu, std::fabs(tu));
        max_tr = std::max(max_tr, std::fabs(tr));
    }
    // tan(hfov/2) = tan(fovy/2) * aspect must cover max_tr; tan(fovy/2) covers max_tu.
    const double thty = std::max(max_tu, max_tr / aspect);
    t.fovy = 2.0 * deg(std::atan(thty));
    return t;
}

// --- cube 360 capture model ---------------------------------------------------
// Six axis-aligned faces around the capture eye (Ansel's 360 contract: cube
// faces at FOV 90, fixed pivot): four at the horizon (F/R/B/L: pitch 0, yaw
// offset 0/+90/+180/-90 from the capture yaw) plus U/D at pitch +/-90. Each
// face is FOV 90 VERTICAL at the native viewport aspect, so the horizontal
// extent is wider than 90 and neighbours overlap; the equirectangular
// compositor assigns each panorama pixel to the face whose axis is closest,
// which puts every seam inside an overlap where both faces genuinely saw the
// same rays. Atlas slots (4 x 2) are manifest bookkeeping only.
struct FacePose {
    double yaw;   // radians
    double pitch; // radians
    int row, col;
};
inline constexpr double kCube360Fovy = 90.0; // degrees, vertical

inline void cube360_faces(double yaw, FacePose out[6]) {
    out[0] = {yaw, 0.0, 0, 1};               // front
    out[1] = {yaw + rad(90.0), 0.0, 0, 0};   // right
    out[2] = {yaw + rad(180.0), 0.0, 0, 3};  // back
    out[3] = {yaw - rad(90.0), 0.0, 0, 2};   // left
    out[4] = {yaw, rad(90.0), 1, 1};         // up
    out[5] = {yaw, -rad(90.0), 1, 3};        // down
}

// Precomputed linear map from continuous master pixel coords to tile pixels:
// the master ray through pixel (x, y) is d = A + B*x + C*y, and the tile
// projection divides through-linearly, so tile px/py are ratios of linear
// functions of (x, y) - a homography, evaluated with 9 multiplies + 1 divide.
struct RayMap {
    double rn[3] = {0, 0, 0};  // dot(d, r_t)
    double un[3] = {0, 0, 0};  // dot(d, u_t)
    double dn[3] = {0, 0, 0};  // dot(d, f_t)
    double sx = 0, sy = 0;     // px = w/2 + rn/dn * sx ; py = h/2 - un/dn * sy
    uint32_t w = 0, h = 0;

    RayMap() = default;
    RayMap(const Master& m, const Tile& t) : w(t.w), h(t.h) {
        double f[3], r[3], u[3];
        basis(m.yaw, m.pitch, f, r, u);
        const double hy = std::tan(rad(m.fovy) / 2.0);
        const double hx = hy * static_cast<double>(m.vw) / static_cast<double>(m.vh);
        const double W = static_cast<double>(m.width), H = static_cast<double>(m.height);
        // mx(x) = (2x - (W-1))/W * hx ; my(y) = ((H-1) - 2y)/H * hy
        double A[3], B[3], C[3];
        for (int i = 0; i < 3; ++i) {
            A[i] = f[i] - ((W - 1.0) / W) * hx * r[i] + ((H - 1.0) / H) * hy * u[i];
            B[i] = (2.0 / W) * hx * r[i];
            C[i] = -(2.0 / H) * hy * u[i];
        }
        double ft[3], rt[3], ut[3];
        basis(t.yaw, t.pitch, ft, rt, ut);
        rn[0] = dot(A, rt); rn[1] = dot(B, rt); rn[2] = dot(C, rt);
        un[0] = dot(A, ut); un[1] = dot(B, ut); un[2] = dot(C, ut);
        dn[0] = dot(A, ft); dn[1] = dot(B, ft); dn[2] = dot(C, ft);
        const double thty = std::tan(rad(t.fovy) / 2.0);
        const double thtx = thty * static_cast<double>(t.w) / static_cast<double>(t.h);
        sx = (static_cast<double>(w) / 2.0) / thtx;
        sy = (static_cast<double>(h) / 2.0) / thty;
    }

    // Continuous tile pixel (px, py) for master pixel (x, y). False if the
    // ray is degenerate (behind the tile plane).
    bool eval(double x, double y, double& px, double& py) const {
        const double d = dn[0] + dn[1] * x + dn[2] * y;
        if (!(d > 1e-9))
            return false;
        px = static_cast<double>(w) / 2.0 + (rn[0] + rn[1] * x + rn[2] * y) / d * sx;
        py = static_cast<double>(h) / 2.0 - (un[0] + un[1] * x + un[2] * y) / d * sy;
        return true;
    }
};

}  // namespace lumen::tilemath
