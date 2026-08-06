#include "OffscreenRenderer.h"

#include <vector>

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_TypeOfDeflection.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <Bnd_Box.hxx>
#include <Graphic3d_BufferType.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_MaterialAspect.hxx>
#include <Image_AlienPixMap.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_Drawer.hxx>
#include <Quantity_Color.hxx>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>
#include <V3d_ImageDumpOptions.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <Xw_Window.hxx>
#include <gp_GTrsf.hxx>

#include "anim/ForwardKinematics.h"
#include "io/OcctMessages.h"

namespace viki {
namespace anim {

namespace {

Quantity_Color fromRgb24(quint32 rgb)
{
    return Quantity_Color(((rgb >> 16) & 0xFF) / 255.0,
                          ((rgb >> 8) & 0xFF) / 255.0,
                          (rgb & 0xFF) / 255.0, Quantity_TOC_sRGB);
}

gp_Dir eyeDirection(CameraView view)
{
    switch (view) {
    case CameraView::Front:
        return gp_Dir(0, 1, 0);
    case CameraView::ThreeQuarter:
        // 30 deg azimuth from front toward the avatar's right, elevated a
        // touch for the aesthetic shot the contract mentions.
        return gp_Dir(0.5, 0.866, 0.25);
    case CameraView::Side:
    default:
        return gp_Dir(1, 0, 0);
    }
}

// Soft studio material derived from the avatar's PBR numbers: the raster
// keeps Phong (the probe showed OCCT's PBR mode flattens volumes without
// an environment and renders translucency opaque), the GLB carries the
// real metallic-roughness for downstream viewers.
Graphic3d_MaterialAspect softMaterial(double roughness, double metallic)
{
    Graphic3d_MaterialAspect mat(Graphic3d_NameOfMaterial_UserDefined);
    const double gloss = 1.0 - roughness;
    const double spec = 0.05 + 0.28 * gloss * gloss + 0.25 * metallic;
    mat.SetSpecularColor(Quantity_Color(spec, spec, spec, Quantity_TOC_RGB));
    mat.SetShininess(std::clamp(0.06 + 0.7 * gloss, 0.05, 1.0));
    return mat;
}

} // namespace

struct OffscreenRenderer::Impl {
    Handle(V3d_Viewer) viewer;
    Handle(V3d_View) view;
    Handle(AIS_InteractiveContext) context;
    std::vector<Handle(V3d_Light)> lights; // per-clip rig, camera-relative
    bool initFailed = false;
    QString initError;
};

OffscreenRenderer::OffscreenRenderer() : m_impl(std::make_unique<Impl>())
{
    silenceOcctMessages();
    try {
        Handle(Aspect_DisplayConnection) display =
            new Aspect_DisplayConnection();
        Handle(OpenGl_GraphicDriver) driver =
            new OpenGl_GraphicDriver(display, false);
        driver->ChangeOptions().buffersNoSwap = true;
        // The whole point: keep destination alpha, so the background dumps
        // at alpha 0 for the transparent WebP/PNG frames.
        driver->ChangeOptions().buffersOpaqueAlpha = false;
        if (!driver->InitContext())
            throw Standard_Failure("no GL context");

        m_impl->viewer = new V3d_Viewer(driver);
        // No default lights: renderClip installs a camera-relative
        // three-point rig (key + fill + ambient) per clip — the default
        // headlight flattens the volumes.
        m_impl->context = new AIS_InteractiveContext(m_impl->viewer);
        m_impl->view = m_impl->viewer->CreateView();
        // Never mapped: ToPixMap renders into its own FBO at the requested
        // size, the window is only the GL surface anchor.
        Handle(Xw_Window) window =
            new Xw_Window(display, "vikicad-anim", 0, 0, 64, 64);
        window->SetVirtual(true);
        m_impl->view->SetWindow(window);
        m_impl->view->SetBackgroundColor(Quantity_NOC_BLACK);
        // Per-pixel shading: the probe showed Gouraud/default washing out
        // the modelling and OCCT's PBR mode needing an environment map.
        m_impl->view->SetShadingModel(Graphic3d_TypeOfShadingModel_Phong);
        m_impl->view->MustBeResized();
    } catch (const Standard_Failure& e) {
        m_impl->initFailed = true;
        m_impl->initError = QString::fromUtf8(e.GetMessageString());
        m_impl->view.Nullify();
        m_impl->context.Nullify();
        m_impl->viewer.Nullify();
    } catch (...) {
        m_impl->initFailed = true;
        m_impl->initError = QStringLiteral("offscreen GL init failed");
        m_impl->view.Nullify();
        m_impl->context.Nullify();
        m_impl->viewer.Nullify();
    }
}

OffscreenRenderer::~OffscreenRenderer() = default;

bool OffscreenRenderer::valid() const
{
    return !m_impl->initFailed;
}

QString OffscreenRenderer::initError() const
{
    return m_impl->initError;
}

RenderClipResult OffscreenRenderer::renderClip(
    const Chain& chain, const AnimClip& clip,
    const AvatarProvider& provider, const AvatarSpec& avatar,
    const RenderOptions& options,
    const std::function<bool(int, const QImage&)>& sink)
{
    RenderClipResult result;
    if (!valid()) {
        result.error =
            QStringLiteral("no offscreen GL context (%1) — is a display "
                           "reachable?")
                .arg(m_impl->initError);
        return result;
    }
    if (options.width <= 0 || options.height <= 0) {
        result.error = QStringLiteral("frame size must be positive");
        return result;
    }

    const std::vector<double> times = clip.frameTimes();
    if (times.empty()) {
        result.error = QStringLiteral("the clip has no frames");
        return result;
    }
    // Numbered cap, refused before any allocation (LESSONS rule). The
    // parse-side 300 s clip cap keeps CLI inputs far below this; the cap
    // here guards programmatic callers.
    constexpr size_t kMaxFrames = 40000;
    if (times.size() > kMaxFrames) {
        result.error = QStringLiteral("clip yields %1 frames, the cap is "
                                      "%2")
                           .arg(times.size())
                           .arg(kMaxFrames);
        return result;
    }

    // ---- Lights: camera-relative three-point rig ---------------------
    // Key from above the camera's left shoulder, weak fill from its right,
    // low ambient to open the shadows. Fixed per clip, so frames stay
    // byte-deterministic; the modelling follows whichever camera is asked.
    const gp_Dir eye = eyeDirection(options.camera);
    {
        for (const Handle(V3d_Light)& light : m_impl->lights)
            m_impl->viewer->DelLight(light);
        m_impl->lights.clear();
        const gp_Vec e(eye);
        const gp_Vec up(0, 0, 1);
        gp_Vec right = e.Crossed(up);
        right.Normalize(); // never vertical: the three cameras are lateral
        const gp_Vec keyFrom = e * 1.0 + up * 0.85 - right * 0.5;
        Handle(V3d_DirectionalLight) key =
            new V3d_DirectionalLight(gp_Dir(keyFrom.Reversed()));
        key->SetIntensity(1.05f);
        const gp_Vec fillFrom = e * 1.0 - up * 0.10 + right * 0.9;
        Handle(V3d_DirectionalLight) fill =
            new V3d_DirectionalLight(gp_Dir(fillFrom.Reversed()));
        fill->SetIntensity(0.40f);
        Handle(V3d_AmbientLight) ambient = new V3d_AmbientLight();
        ambient->SetIntensity(0.42f);
        m_impl->lights = {key, fill, ambient};
        for (const Handle(V3d_Light)& light : m_impl->lights)
            m_impl->viewer->AddLight(light);
        m_impl->viewer->SetLightOn();
    }

    // ---- Scene: one AIS shape per avatar part, joint-local geometry ----
    struct PartHandle {
        int joint = 0;
        Handle(AIS_Shape) ais;
        Bnd_Box localBox;
    };
    std::vector<PartHandle> handles;
    const Quantity_Color base = fromRgb24(avatar.baseColor);
    const Quantity_Color accent = fromRgb24(avatar.accentColor);
    const Graphic3d_MaterialAspect material =
        softMaterial(avatar.roughness, avatar.metallic);
    for (int j = 0; j < static_cast<int>(chain.joints.size()); ++j) {
        for (const AvatarPart& part : provider.partsForJoint(chain, j)) {
            if (part.shape.IsNull())
                continue;
            PartHandle h;
            h.joint = j;
            h.ais = new AIS_Shape(part.shape);
            h.ais->SetMaterial(material); // before SetColor: colour wins
            h.ais->SetColor(part.accent ? accent : base);
            h.ais->Attributes()->SetIsoOnPlane(false);
            // Honour options.deflectionMm: absolute chordal deviation for
            // the display tessellation (the default is relative and can
            // facet small parts visibly).
            h.ais->Attributes()->SetTypeOfDeflection(Aspect_TOD_ABSOLUTE);
            h.ais->Attributes()->SetMaximalChordialDeviation(
                options.deflectionMm > 0 ? options.deflectionMm : 0.8);
            // AddOptimal, not Add: the sculpted parts are B-spline solids
            // and the fast bbox hulls their CONTROL points, which overshoot
            // the surface by hundreds of mm — the union box then dwarfs the
            // avatar and FitAll frames it small and off-centre.
            BRepBndLib::AddOptimal(part.shape, h.localBox);
            handles.push_back(h);
        }
    }
    if (handles.empty()) {
        result.error = QStringLiteral("the avatar produced no geometry");
        return result;
    }

    // Display shaded, non-selectable (display mode 1, no selection mode).
    for (const PartHandle& h : handles)
        m_impl->context->Display(h.ais, 1, -1, false);

    // ---- Fixed framing over the WHOLE animation --------------------------
    // Union of every frame's world bbox, floor plane z=0 included, so the
    // camera never moves between frames and the ground sits at the bottom
    // (contract 2026-08-05: side silhouette, sol z=0 en bas).
    Bnd_Box unionBox;
    std::vector<std::vector<gp_Trsf>> frameTransforms;
    frameTransforms.reserve(times.size());
    for (const double t : times) {
        const PoseSample pose = clip.sampleAt(t, options.applyBreath);
        frameTransforms.push_back(worldTransforms(chain, pose));
        const auto& world = frameTransforms.back();
        for (const PartHandle& h : handles)
            unionBox.Add(h.localBox.Transformed(
                world[static_cast<size_t>(h.joint)]));
    }
    {
        double xmin, ymin, zmin, xmax, ymax, zmax;
        unionBox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        unionBox.Update(xmin, ymin, std::min(zmin, 0.0), xmax, ymax,
                        std::max(zmax, 1e-3));
    }

    // ---- Ground shadow (avatar presentation.ground_shadow > 0) ----------
    // A squashed dark translucent ellipsoid at z=0 under the animation
    // footprint: composites as a soft contact shadow over any background
    // while the frame alpha stays partial. Static across frames (the
    // footprint covers every frame), removed with the rest of the scene.
    if (avatar.groundShadow > 0.0) {
        double xmin, ymin, zmin, zmaxScene, xmax, ymax;
        unionBox.Get(xmin, ymin, zmin, xmax, ymax, zmaxScene);
        const double sceneMax =
            std::max({xmax - xmin, ymax - ymin, zmaxScene - zmin});
        // Anchor the blob under the SUPPORTS: union of the parts that come
        // near the floor over the whole animation (downward dog: feet and
        // wrists, not the centroid — an off-centre shadow reads as a
        // floating figure). Fallback: the whole footprint.
        Bnd_Box contactBox;
        const double contactThreshold =
            zmin + std::max(20.0, 0.05 * sceneMax);
        for (const auto& world : frameTransforms)
            for (const PartHandle& h : handles) {
                const Bnd_Box b = h.localBox.Transformed(
                    world[static_cast<size_t>(h.joint)]);
                double bx0, by0, bz0, bx1, by1, bz1;
                b.Get(bx0, by0, bz0, bx1, by1, bz1);
                if (bz0 <= contactThreshold)
                    contactBox.Add(b);
            }
        if (!contactBox.IsVoid()) {
            double czMin, czMax;
            contactBox.Get(xmin, ymin, czMin, xmax, ymax, czMax);
        }
        const double hx = std::max(2.0, 0.56 * (xmax - xmin));
        const double hy = std::max(2.0, 0.56 * (ymax - ymin));
        // Thickness follows the LARGEST scene extent (a standing figure's
        // height), not the footprint: seen edge-on from the side camera a
        // footprint-scaled lens degenerates to a sub-pixel line.
        const double hz = std::max(2.0, 0.012 * sceneMax);
        TopoDS_Shape blob;
        try {
            blob = BRepPrimAPI_MakeSphere(1.0).Shape();
            if (!blob.IsNull()) {
                gp_GTrsf stretch;
                stretch.SetValue(1, 1, hx);
                stretch.SetValue(2, 2, hy);
                stretch.SetValue(3, 3, hz);
                blob = BRepBuilderAPI_GTransform(blob, stretch, true)
                           .Shape();
            }
            if (!blob.IsNull()) {
                gp_Trsf move;
                move.SetTranslation(gp_Vec(0.5 * (xmin + xmax),
                                           0.5 * (ymin + ymax), 0.0));
                blob = BRepBuilderAPI_Transform(blob, move, true).Shape();
            }
        } catch (...) {
            blob.Nullify(); // a lost shadow never loses the render
        }
        if (!blob.IsNull()) {
            Handle(AIS_Shape) shadow = new AIS_Shape(blob);
            shadow->SetColor(
                Quantity_Color(0.05, 0.05, 0.06, Quantity_TOC_sRGB));
            shadow->SetTransparency(1.0 - avatar.groundShadow);
            shadow->Attributes()->SetIsoOnPlane(false);
            m_impl->context->Display(shadow, 1, -1, false);
            Bnd_Box shadowBox;
            BRepBndLib::Add(blob, shadowBox);
            unionBox.Add(shadowBox);
        }
    }

    m_impl->view->SetProj(eye.X(), eye.Y(), eye.Z());
    m_impl->view->SetUp(0, 0, 1);
    m_impl->view->Camera()->SetProjectionType(
        Graphic3d_Camera::Projection_Orthographic);
    m_impl->view->Camera()->SetAspect(
        static_cast<double>(options.width) / options.height);
    m_impl->view->FitAll(unionBox, options.margin, false);

    // ---- Frames ---------------------------------------------------------
    V3d_ImageDumpOptions dump;
    dump.Width = options.width;
    dump.Height = options.height;
    dump.BufferType = Graphic3d_BT_RGBA;
    dump.ToAdjustAspect = false; // aspect already set from LxH

    for (size_t f = 0; f < times.size(); ++f) {
        const auto& world = frameTransforms[f];
        for (const PartHandle& h : handles)
            h.ais->SetLocalTransformation(
                world[static_cast<size_t>(h.joint)]);
        m_impl->context->UpdateCurrentViewer();

        Image_AlienPixMap pix;
        if (!m_impl->view->ToPixMap(pix, dump)) {
            m_impl->context->RemoveAll(false);
            result.error =
                QStringLiteral("frame %1: GL dump failed").arg(f);
            return result;
        }

        QImage image(options.width, options.height,
                     QImage::Format_RGBA8888);
        const bool rgba = pix.Format() == Image_Format_RGBA;
        for (int y = 0; y < options.height; ++y) {
            // Row()/PixelColor() count from the visual TOP whatever the
            // underlying storage order (the top-row pointer inside
            // Image_PixMap absorbs GL's bottom-up buffer) — compensating
            // for IsTopDown() here flips the avatar on its head.
            const Standard_Size srcRow = static_cast<Standard_Size>(y);
            uchar* dst = image.scanLine(y);
            if (rgba) {
                std::memcpy(dst, pix.Row(srcRow),
                            static_cast<size_t>(options.width) * 4);
            } else {
                for (int x = 0; x < options.width; ++x) {
                    const Quantity_ColorRGBA c = pix.PixelColor(
                        x, static_cast<Standard_Integer>(srcRow));
                    dst[x * 4 + 0] =
                        static_cast<uchar>(c.GetRGB().Red() * 255.0 + 0.5);
                    dst[x * 4 + 1] = static_cast<uchar>(
                        c.GetRGB().Green() * 255.0 + 0.5);
                    dst[x * 4 + 2] =
                        static_cast<uchar>(c.GetRGB().Blue() * 255.0 + 0.5);
                    dst[x * 4 + 3] =
                        static_cast<uchar>(c.Alpha() * 255.0f + 0.5f);
                }
            }
        }

        ++result.frames;
        if (!sink(static_cast<int>(f), image))
            break;
    }

    m_impl->context->RemoveAll(false);
    result.ok = true;
    return result;
}

} // namespace anim
} // namespace viki
