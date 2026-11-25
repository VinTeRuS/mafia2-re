package com.mafia2re.game;

import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;

/** Drawing helpers in a 1940s noir look: near-black bodies, a thin aged-gold double edge, oxblood when active, cream serif type. */
final class UiStyle {
    static final int EDGE = 0xffc2a15a, STEEL_TOP = 0xff24201c, STEEL_BOTTOM = 0xff0e0c0a, RED_TOP = 0xff8c1c1c,
            RED_BOTTOM = 0xff4a0c0c, TEXT = 0xfff0e6cf;

    private UiStyle() {}

    static int alpha(int color, float a) {
        return (Math.round(Math.max(0, Math.min(1, a)) * 255) << 24) | (color & 0xffffff);
    }

    /** A bar/button: gradient body, light edge. `radius` in px. */
    static void bar(Canvas c, Paint p, RectF r, boolean selected, float alpha, float radius, float density) {
        p.setStyle(Paint.Style.FILL);
        p.setShader(new LinearGradient(0, r.top, 0, r.bottom, alpha(selected ? RED_TOP : STEEL_TOP, alpha),
                alpha(selected ? RED_BOTTOM : STEEL_BOTTOM, alpha), Shader.TileMode.CLAMP));
        c.drawRoundRect(r, radius, radius, p);
        p.setShader(null);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(1.6f * density);
        p.setStrokeWidth(1.3f * density);
        p.setColor(alpha(EDGE, Math.min(1f, alpha + 0.2f)));
        c.drawRoundRect(r, radius, radius, p);
        // inner hairline, as on a period title card
        float in = 3.2f * density;
        p.setStrokeWidth(0.6f * density);
        p.setColor(alpha(EDGE, alpha * 0.6f));
        c.drawRoundRect(new RectF(r.left + in, r.top + in, r.right - in, r.bottom - in), Math.max(0, radius - in),
                Math.max(0, radius - in), p);
        p.setStyle(Paint.Style.FILL);
    }

    /** The title bar of the settings panel. */
    static void titleBar(Canvas c, Paint p, RectF r, float density) {
        p.setStyle(Paint.Style.FILL);
        p.setShader(new LinearGradient(0, r.top, 0, r.bottom, 0xff3a2f22, 0xff1c1610, Shader.TileMode.CLAMP));
        c.drawRoundRect(r, r.height() / 2, r.height() / 2, p);
        p.setShader(null);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(1.6f * density);
        p.setColor(EDGE);
        c.drawRoundRect(r, r.height() / 2, r.height() / 2, p);
        p.setStyle(Paint.Style.FILL);
    }

    /** Label: bold serif, slightly spaced, cream, centred on the rectangle. */
    static void label(Canvas c, Paint t, String s, RectF r, float size, float alpha) {
        t.setTypeface(Typeface.create(Typeface.SERIF, Typeface.BOLD));
        t.setLetterSpacing(0.06f);
        t.setTextSize(size);
        t.setColor(alpha(TEXT, Math.max(0.7f, alpha)));
        c.drawText(s, r.centerX(), r.centerY() + size * 0.35f, t);
        t.setLetterSpacing(0f);
    }
}
