package io.github.iamwilmar.mhriseport;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;

// Los dibujos de los controles táctiles, hechos con trazos (sin imágenes): en los colores del menú del juego, vino oscuro
// con borde de bronce y letras doradas; un control pulsado se llena de oro con las letras en vino. alpha (0..255) es la
// opacidad de todo el control.
final class Glyphs {
    static final int WINE = 0xFF4A1816, DEEP = 0xFF281010, BRONZE = 0xFFC49646, GOLD = 0xFFE2B65C, CREAM = 0xFFF0E4C8;

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint edge = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ink = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final Paint imagePaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final RectF box = new RectF();

    Glyphs() {
        fill.setStyle(Paint.Style.FILL);
        edge.setStyle(Paint.Style.STROKE);
        ink.setStyle(Paint.Style.FILL);
        ink.setStrokeCap(Paint.Cap.ROUND);
        ink.setStrokeJoin(Paint.Join.ROUND);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Typeface.create(Typeface.SERIF, Typeface.BOLD));
    }

    private static int withAlpha(int color, int alpha) {
        return (color & 0x00FFFFFF) | ((((color >>> 24) * alpha) / 255) << 24);
    }

    // Plato y borde de un control: vino (o oro pulsado), con un aro de bronce
    private void plate(int alpha, boolean pressed, float stroke) {
        fill.setColor(withAlpha(pressed ? GOLD : WINE, pressed ? alpha : alpha * 9 / 10));
        edge.setColor(withAlpha(BRONZE, alpha));
        edge.setStrokeWidth(stroke);
    }
    private int inkColor(int alpha, boolean pressed) { return withAlpha(pressed ? DEEP : GOLD, alpha); }

    private void label(Canvas c, String s, float cx, float cy, float size, int color) {
        text.setTextSize(size);
        text.setColor(color);
        Paint.FontMetrics m = text.getFontMetrics();
        c.drawText(s, cx, cy - (m.ascent + m.descent) / 2, text);
    }

    // A, B, X, Y: un círculo con su letra y un segundo aro fino por dentro
    void face(Canvas c, float cx, float cy, float r, String letter, boolean pressed, int alpha) {
        plate(alpha, pressed, r * 0.07f);
        c.drawCircle(cx, cy, r, fill);
        c.drawCircle(cx, cy, r, edge);
        edge.setStrokeWidth(r * 0.025f);
        edge.setColor(withAlpha(BRONZE, alpha / 2));
        c.drawCircle(cx, cy, r * 0.82f, edge);
        label(c, letter, cx, cy, r * 1.05f, inkColor(alpha, pressed));
    }

    // L, R: una pastilla; ZL, ZR: un gatillo, con la punta de fuera más alta y redonda
    void shoulder(Canvas c, float cx, float cy, float w, float h, String name, boolean trigger, boolean left, boolean pressed,
                  int alpha) {
        plate(alpha, pressed, h * 0.08f);
        path.reset();
        float l = cx - w / 2, r = cx + w / 2, t = cy - h / 2, b = cy + h / 2;
        if (trigger) {
            float lift = h * 0.35f;  // (el lado de fuera sube: la forma del gatillo de un mando)
            float outer = left ? l : r, inner = left ? r : l, dir = left ? 1 : -1;
            path.moveTo(inner, b);
            path.lineTo(outer + dir * h * 0.45f, b);
            path.quadTo(outer, b, outer, b - h * 0.45f);
            path.lineTo(outer, t + h * 0.1f);
            path.quadTo(outer, t - lift, outer + dir * w * 0.45f, t - lift * 0.4f);
            path.quadTo(inner - dir * w * 0.1f, t, inner, t + h * 0.15f);
            path.close();
        } else {
            box.set(l, t, r, b);
            path.addRoundRect(box, h / 2, h / 2, Path.Direction.CW);
        }
        c.drawPath(path, fill);
        c.drawPath(path, edge);
        label(c, name, cx, cy + (trigger ? h * 0.05f : 0), h * 0.62f, inkColor(alpha, pressed));
    }

    // Botón pequeño redondo: − y + (sus barras), el menú (un engranaje), L3 y R3 (un stick visto desde arriba con su letra)
    enum Small { MINUS, PLUS, MENU, L3, R3 }

    void small(Canvas c, float cx, float cy, float r, Small kind, boolean pressed, int alpha) {
        plate(alpha, pressed, r * 0.09f);
        c.drawCircle(cx, cy, r, fill);
        c.drawCircle(cx, cy, r, edge);
        int color = inkColor(alpha, pressed);
        ink.setColor(color);
        float bar = r * 0.5f, thick = r * 0.16f;
        switch (kind) {
            case MINUS:
                box.set(cx - bar, cy - thick / 2, cx + bar, cy + thick / 2);
                c.drawRoundRect(box, thick / 2, thick / 2, ink);
                break;
            case PLUS:
                box.set(cx - bar, cy - thick / 2, cx + bar, cy + thick / 2);
                c.drawRoundRect(box, thick / 2, thick / 2, ink);
                box.set(cx - thick / 2, cy - bar, cx + thick / 2, cy + bar);
                c.drawRoundRect(box, thick / 2, thick / 2, ink);
                break;
            case MENU:
                gear(c, cx, cy, r * 0.62f, color, pressed ? GOLD : WINE, alpha);
                break;
            case L3:
            case R3:
                // (la seta de un stick vista desde arriba, arriba, y su nombre debajo)
                edge.setColor(color);
                edge.setStrokeWidth(r * 0.08f);
                c.drawCircle(cx, cy - r * 0.22f, r * 0.36f, edge);
                c.drawCircle(cx, cy - r * 0.22f, r * 0.14f, ink);
                label(c, kind == Small.L3 ? "L3" : "R3", cx, cy + r * 0.45f, r * 0.46f, color);
                break;
        }
    }

    // Un engranaje de ocho dientes con su agujero (el botón del menú)
    void gear(Canvas c, float cx, float cy, float r, int color, int hole, int alpha) {
        path.reset();
        int teeth = 8;
        for (int i = 0; i < teeth * 2; i++) {
            double a0 = Math.PI * 2 * i / (teeth * 2) - Math.PI / (teeth * 2);
            double a1 = a0 + Math.PI / teeth;
            float rr = i % 2 == 0 ? r : r * 0.76f;
            float x0 = cx + (float) Math.cos(a0) * rr, y0 = cy + (float) Math.sin(a0) * rr;
            if (i == 0) path.moveTo(x0, y0);
            else path.lineTo(x0, y0);
            path.lineTo(cx + (float) Math.cos(a1) * rr, cy + (float) Math.sin(a1) * rr);
        }
        path.close();
        ink.setColor(color);
        c.drawPath(path, ink);
        ink.setColor(withAlpha(hole, alpha));
        c.drawCircle(cx, cy, r * 0.34f, ink);
    }

    // La cruceta: una cruz con una flecha en cada brazo; dirs, los brazos pulsados (bit 0 arriba, 1 abajo, 2 izquierda,
    // 3 derecha). plateColor, outline, picture and ownShape as in custom (a picture is cut to the cross, or alone in its
    // own shape; it darkens while an arm is pressed)
    void dpad(Canvas c, float cx, float cy, float half, int dirs, int alpha, int plateColor, float outline,
              android.graphics.Bitmap picture, boolean ownShape) {
        if (picture != null && ownShape) {
            fitted(picture, cx, cy, half);
            drawPicture(c, picture, alpha, dirs != 0);
            return;
        }
        float arm = half * 0.36f;
        path.reset();
        path.moveTo(cx - arm, cy - half);
        path.lineTo(cx + arm, cy - half);
        path.lineTo(cx + arm, cy - arm);
        path.lineTo(cx + half, cy - arm);
        path.lineTo(cx + half, cy + arm);
        path.lineTo(cx + arm, cy + arm);
        path.lineTo(cx + arm, cy + half);
        path.lineTo(cx - arm, cy + half);
        path.lineTo(cx - arm, cy + arm);
        path.lineTo(cx - half, cy + arm);
        path.lineTo(cx - half, cy - arm);
        path.lineTo(cx - arm, cy - arm);
        path.close();
        plate(alpha, false, half * 0.05f * outline);
        if (plateColor != 0) fill.setColor(withAlpha(plateColor, alpha * 9 / 10));
        if (picture != null) {
            c.save();
            c.clipPath(path);
            box.set(cx - half, cy - half, cx + half, cy + half);
            imagePaint.setAlpha(alpha);
            c.drawBitmap(picture, null, box, imagePaint);
            c.restore();
        } else {
            c.drawPath(path, fill);
        }
        // los brazos pulsados, en oro
        fill.setColor(withAlpha(GOLD, picture != null ? alpha / 2 : alpha));
        if ((dirs & 1) != 0) { box.set(cx - arm, cy - half, cx + arm, cy - arm); c.drawRect(box, fill); }
        if ((dirs & 2) != 0) { box.set(cx - arm, cy + arm, cx + arm, cy + half); c.drawRect(box, fill); }
        if ((dirs & 4) != 0) { box.set(cx - half, cy - arm, cx - arm, cy + arm); c.drawRect(box, fill); }
        if ((dirs & 8) != 0) { box.set(cx + arm, cy - arm, cx + half, cy + arm); c.drawRect(box, fill); }
        if (outline > 0) c.drawPath(path, edge);
        if (picture != null) return;  // (the picture has its own arrows, or none)
        float tip = half * 0.82f, base = half * 0.52f, w = arm * 0.62f;
        for (int d = 0; d < 4; d++) {
            ink.setColor(inkColor(alpha, (dirs & (1 << d)) != 0));
            float sx = d == 2 ? -1 : d == 3 ? 1 : 0, sy = d == 0 ? -1 : d == 1 ? 1 : 0;
            path.reset();
            path.moveTo(cx + sx * tip, cy + sy * tip);
            path.lineTo(cx + sx * base + sy * w, cy + sy * base + sx * w);
            path.lineTo(cx + sx * base - sy * w, cy + sy * base - sx * w);
            path.close();
            c.drawPath(path, ink);
        }
        ink.setColor(withAlpha(BRONZE, alpha));
        c.drawCircle(cx, cy, arm * 0.35f, ink);
    }

    // Un stick: la base con un aro de marcas cada 45 grados, y la seta donde va el dedo (kx, ky de -1 a 1, y hacia abajo
    // positivo); con un dedo encima la seta se aclara, y en oro mientras es L3 / R3
    // shape (Controls.SHAPES, 0 or 1 a circle) and plateColor are the base's, outline the width of its edges; picture is
    // the knob, cut round or (ownShape) alone in its own shape
    void stick(Canvas c, float cx, float cy, float r, float kx, float ky, boolean held, boolean pressed, int alpha,
               int shape, int plateColor, float outline, android.graphics.Bitmap picture, boolean ownShape) {
        float round = shape <= 1 ? r : shape == 2 ? r * 0.38f : r * 0.08f;
        box.set(cx - r, cy - r, cx + r, cy + r);
        fill.setColor(withAlpha(plateColor != 0 ? plateColor : DEEP, alpha * 7 / 10));
        c.drawRoundRect(box, round, round, fill);
        if (outline > 0) {
            edge.setColor(withAlpha(BRONZE, alpha));
            edge.setStrokeWidth(r * 0.035f * outline);
            c.drawRoundRect(box, round, round, edge);
            edge.setStrokeWidth(r * 0.015f * outline);
            edge.setColor(withAlpha(BRONZE, alpha / 2));
            c.drawCircle(cx, cy, r * 0.70f, edge);
        }
        ink.setColor(withAlpha(BRONZE, alpha * 3 / 4));
        for (int i = 0; i < 8 && outline > 0; i++) {  // (las ocho direcciones: rombos pequeños en el borde)
            double a = Math.PI / 4 * i;
            float mx = cx + (float) Math.cos(a) * r * 0.86f, my = cy + (float) Math.sin(a) * r * 0.86f, d = r * (i % 2 == 0 ? 0.06f : 0.04f);
            path.reset();
            path.moveTo(mx, my - d);
            path.lineTo(mx + d, my);
            path.lineTo(mx, my + d);
            path.lineTo(mx - d, my);
            path.close();
            c.drawPath(path, ink);
        }
        float knob = r * 0.42f, x = cx + kx * (r - knob * 0.6f), y = cy + ky * (r - knob * 0.6f);
        if (picture != null) {
            if (ownShape) {
                fitted(picture, x, y, knob);
                drawPicture(c, picture, alpha, pressed);
            } else {
                path.reset();
                path.addCircle(x, y, knob, Path.Direction.CW);
                c.save();
                c.clipPath(path);
                box.set(x - knob, y - knob, x + knob, y + knob);
                imagePaint.setAlpha(pressed ? alpha * 6 / 10 : held ? alpha : alpha * 9 / 10);
                c.drawBitmap(picture, null, box, imagePaint);
                c.restore();
                if (outline > 0) {
                    edge.setColor(withAlpha(pressed ? GOLD : BRONZE, alpha));
                    edge.setStrokeWidth(knob * 0.1f * outline);
                    c.drawCircle(x, y, knob, edge);
                }
            }
            return;
        }
        plate(alpha, pressed, knob * 0.1f * outline);
        if (plateColor != 0 && !pressed) fill.setColor(withAlpha(plateColor, alpha));
        if (held && !pressed) fill.setColor(withAlpha(plateColor != 0 ? plateColor : 0xFF6A2A22, alpha));
        c.drawCircle(x, y, knob, fill);
        if (outline > 0) c.drawCircle(x, y, knob, edge);
        edge.setColor(inkColor(alpha, pressed));
        edge.setStrokeWidth(knob * 0.06f);
        c.drawCircle(x, y, knob * 0.62f, edge);  // (el agarre de goma de la seta)
        ink.setColor(inkColor(alpha, pressed));
        c.drawCircle(x, y, knob * 0.12f, ink);
    }

    // A button with the look chosen in the layout editor: shape 1 a circle, 2 a rounded square, 3 a square; an icon
    // (Controls.ICONS, 0 none) or its text; hold (tap to hold) a small diamond at the top right
    // plateColor: a fill instead of the wine (0: the wine); picture: the player's image, filling the shape (with it the
    // letters and icon aren't drawn; pressed, it's tinted gold)
    void custom(Canvas c, float cx, float cy, float r, int shape, int icon, String text, boolean hold, boolean pressed,
                int alpha, int plateColor, android.graphics.Bitmap picture, float outline, boolean ownShape) {
        plate(alpha, pressed, r * 0.07f * outline);
        if (plateColor != 0 && !pressed) fill.setColor(withAlpha(plateColor, alpha * 9 / 10));
        float round = shape == 1 ? r : shape == 2 ? r * 0.38f : r * 0.08f;
        box.set(cx - r, cy - r, cx + r, cy + r);
        if (picture != null && ownShape) {
            // (the picture alone, in its own shape and proportions inside the button's square: no plate, no edge; pressed
            // it darkens)
            fitted(picture, cx, cy, r);
            drawPicture(c, picture, alpha, pressed);
        } else {
            if (plateColor != 0 || pressed || picture == null) c.drawRoundRect(box, round, round, fill);
            if (picture != null) {  // (clipped to the shape)
                path.reset();
                path.addRoundRect(box, round, round, Path.Direction.CW);
                c.save();
                c.clipPath(path);
                imagePaint.setAlpha(pressed ? alpha * 6 / 10 : alpha);
                c.drawBitmap(picture, null, box, imagePaint);
                c.restore();
            }
            if (outline > 0) c.drawRoundRect(box, round, round, edge);
        }
        int color = inkColor(alpha, pressed);
        if (picture != null) {
            // (the picture is the label)
        } else if (icon > 0) {
            icon(c, cx, cy, r * 0.55f, icon, color);
        } else if (text == null) {  // (the menu: its gear)
            gear(c, cx, cy, r * 0.62f, color, pressed ? GOLD : plateColor != 0 ? plateColor : WINE, alpha);
        } else {
            float size = r * (text.length() <= 2 ? 0.95f : text.length() <= 4 ? 0.62f : text.length() <= 7 ? 0.44f : 0.32f);
            label(c, text, cx, cy, size, color);
        }
        if (hold) {
            float mx = cx + r * 0.62f, my = cy - r * 0.62f, d = r * 0.16f;
            path.reset();
            path.moveTo(mx, my - d);
            path.lineTo(mx + d, my);
            path.lineTo(mx, my + d);
            path.lineTo(mx - d, my);
            path.close();
            ink.setColor(color);
            c.drawPath(path, ink);
        }
    }

    // A picture in its own shape, in box (fitted): no plate, no edge; pressed, it darkens
    private void drawPicture(Canvas c, android.graphics.Bitmap picture, int alpha, boolean pressed) {
        imagePaint.setAlpha(alpha);
        imagePaint.setColorFilter(pressed ? new android.graphics.PorterDuffColorFilter(0x90281010,
                android.graphics.PorterDuff.Mode.SRC_ATOP) : null);
        c.drawBitmap(picture, null, box, imagePaint);
        imagePaint.setColorFilter(null);
    }

    // Where a picture in its own shape goes: as large as fits a 2r square, its proportions kept, centred (in box)
    void fitted(android.graphics.Bitmap picture, float cx, float cy, float r) {
        float w = picture.getWidth(), h = picture.getHeight(), k = 2 * r / Math.max(w, h);
        box.set(cx - w * k / 2, cy - h * k / 2, cx + w * k / 2, cy + h * k / 2);
    }
    RectF fittedBox() { return box; }

    // The icons: plain shapes drawn with paths, s half their size
    private void icon(Canvas c, float cx, float cy, float s, int icon, int color) {
        ink.setColor(color);
        edge.setColor(color);
        edge.setStrokeWidth(s * 0.16f);
        path.reset();
        switch (icon) {
            case 1: case 2: case 3: case 4: {  // arrows: up, down, left, right
                float sx = icon == 3 ? -1 : icon == 4 ? 1 : 0, sy = icon == 1 ? -1 : icon == 2 ? 1 : 0;
                // (the head from the tip back to the middle, the shaft from there to the far end)
                path.moveTo(cx + sx * s, cy + sy * s);
                path.lineTo(cx + sy * s * 0.8f, cy + sx * s * 0.8f);
                path.lineTo(cx - sy * s * 0.8f, cy - sx * s * 0.8f);
                path.close();
                c.drawPath(path, ink);
                float half = s * 0.22f;
                float ax = cx - sx * s * 0.9f, ay = cy - sy * s * 0.9f;  // (the shaft's far end)
                box.set(Math.min(ax, cx) - (sx == 0 ? half : 0), Math.min(ay, cy) - (sy == 0 ? half : 0),
                        Math.max(ax, cx) + (sx == 0 ? half : 0), Math.max(ay, cy) + (sy == 0 ? half : 0));
                c.drawRect(box, ink);
                break;
            }
            case 5:  // a five-pointed star
                for (int i = 0; i < 10; i++) {
                    double a = -Math.PI / 2 + Math.PI * i / 5;
                    float rr = i % 2 == 0 ? s : s * 0.42f;
                    float x = cx + (float) Math.cos(a) * rr, y = cy + (float) Math.sin(a) * rr;
                    if (i == 0) path.moveTo(x, y);
                    else path.lineTo(x, y);
                }
                path.close();
                c.drawPath(path, ink);
                break;
            case 6:  // a diamond
                path.moveTo(cx, cy - s);
                path.lineTo(cx + s * 0.75f, cy);
                path.lineTo(cx, cy + s);
                path.lineTo(cx - s * 0.75f, cy);
                path.close();
                c.drawPath(path, ink);
                break;
            case 7:  // a shield: flat top, pointed bottom
                path.moveTo(cx - s * 0.8f, cy - s * 0.8f);
                path.lineTo(cx + s * 0.8f, cy - s * 0.8f);
                path.lineTo(cx + s * 0.8f, cy);
                path.quadTo(cx + s * 0.7f, cy + s * 0.65f, cx, cy + s);
                path.quadTo(cx - s * 0.7f, cy + s * 0.65f, cx - s * 0.8f, cy);
                path.close();
                c.drawPath(path, edge);
                break;
            case 8:  // a blade pointing up, with its guard and grip
                path.moveTo(cx, cy - s);
                path.lineTo(cx + s * 0.2f, cy - s * 0.7f);
                path.lineTo(cx + s * 0.2f, cy + s * 0.35f);
                path.lineTo(cx - s * 0.2f, cy + s * 0.35f);
                path.lineTo(cx - s * 0.2f, cy - s * 0.7f);
                path.close();
                c.drawPath(path, ink);
                box.set(cx - s * 0.6f, cy + s * 0.35f, cx + s * 0.6f, cy + s * 0.5f);
                c.drawRect(box, ink);
                box.set(cx - s * 0.12f, cy + s * 0.5f, cx + s * 0.12f, cy + s);
                c.drawRect(box, ink);
                break;
            case 9:  // a crosshair
                c.drawCircle(cx, cy, s * 0.62f, edge);
                box.set(cx - s, cy - s * 0.07f, cx - s * 0.3f, cy + s * 0.07f);
                c.drawRect(box, ink);
                box.set(cx + s * 0.3f, cy - s * 0.07f, cx + s, cy + s * 0.07f);
                c.drawRect(box, ink);
                box.set(cx - s * 0.07f, cy - s, cx + s * 0.07f, cy - s * 0.3f);
                c.drawRect(box, ink);
                box.set(cx - s * 0.07f, cy + s * 0.3f, cx + s * 0.07f, cy + s);
                c.drawRect(box, ink);
                break;
            default:  // a dash: two chevrons to the right
                for (int k = 0; k < 2; k++) {
                    float ox = cx - s * 0.45f + k * s * 0.7f;
                    path.reset();
                    path.moveTo(ox - s * 0.3f, cy - s * 0.75f);
                    path.lineTo(ox + s * 0.35f, cy);
                    path.lineTo(ox - s * 0.3f, cy + s * 0.75f);
                    c.drawPath(path, edge);
                }
                break;
        }
    }

    // Un combo: dos aros que se cruzan, y debajo los botones que pulsa ("A+X")
    void combo(Canvas c, float cx, float cy, float r, String buttons, boolean pressed, int alpha) {
        plate(alpha, pressed, r * 0.07f);
        box.set(cx - r, cy - r * 0.8f, cx + r, cy + r * 0.8f);
        c.drawRoundRect(box, r * 0.45f, r * 0.45f, fill);
        c.drawRoundRect(box, r * 0.45f, r * 0.45f, edge);
        int color = inkColor(alpha, pressed);
        edge.setColor(color);
        edge.setStrokeWidth(r * 0.07f);
        c.drawCircle(cx - r * 0.2f, cy - r * 0.28f, r * 0.26f, edge);
        c.drawCircle(cx + r * 0.2f, cy - r * 0.28f, r * 0.26f, edge);
        float size = r * (buttons.length() <= 3 ? 0.55f : buttons.length() <= 6 ? 0.42f : 0.32f);
        label(c, buttons, cx, cy + r * 0.38f, size, color);
    }
}
