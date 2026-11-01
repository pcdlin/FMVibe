// What the editor's Canvas (src/gui/canvas_web.cpp) draws with: the page's 2D context, in the editor's logical
// units (the caller sets the transform). It follows canvas.cpp's Direct2D drawing: round caps and joins on
// lines and curves, a dash of twice the width, text centered in its box and cut with an ellipsis.

const FONT_FAMILY = '"Segoe UI", system-ui, -apple-system, "Helvetica Neue", Arial, sans-serif';
const ELLIPSIS = '…';

export function createCanvas2d(ctx) {
  const colors = new Map(); // 0xRRGGBBAA -> CSS color
  const widths = new Map(); // font + text -> width
  const fontBoxes = new Map(); // font -> {ascent, descent}
  let fill = null; // what the context is set to, so repeated styles cost nothing
  let stroke = null;
  let font = null;

  function css(rgba) {
    rgba >>>= 0;
    let c = colors.get(rgba);
    if (c === undefined) {
      c = `rgba(${rgba >>> 24},${(rgba >>> 16) & 255},${(rgba >>> 8) & 255},${(rgba & 255) / 255})`;
      colors.set(rgba, c);
    }
    return c;
  }
  function setFill(rgba) {
    if (fill !== rgba) {
      ctx.fillStyle = css(rgba);
      fill = rgba;
    }
  }
  // Round caps and joins, as canvas.cpp's roundStroke() and dashedStroke(); `square` is Direct2D's default style
  // (flat caps, mitered corners), which rectangles drawn without dashes get.
  function setStroke(rgba, width, dashed, square = false) {
    if (stroke !== rgba) {
      ctx.strokeStyle = css(rgba);
      stroke = rgba;
    }
    ctx.lineWidth = width;
    ctx.lineCap = square ? 'butt' : 'round';
    ctx.lineJoin = square ? 'miter' : 'round';
    ctx.setLineDash(dashed ? [2 * width, 2 * width] : []);
  }
  function setFont(size, bold) {
    const f = `${bold ? 600 : 400} ${size}px ${FONT_FAMILY}`;
    if (font !== f) {
      ctx.font = f;
      font = f;
    }
    return f;
  }
  function measure(text) {
    const key = font + '\u0001' + text;
    let w = widths.get(key);
    if (w === undefined) {
      w = ctx.measureText(text).width;
      if (widths.size > 20000)
        widths.clear();
      widths.set(key, w);
    }
    return w;
  }
  function fontBox() {
    let box = fontBoxes.get(font);
    if (!box) {
      const m = ctx.measureText('Hg');
      box = {ascent: m.fontBoundingBoxAscent ?? m.actualBoundingBoxAscent, descent: m.fontBoundingBoxDescent ?? m.actualBoundingBoxDescent};
      fontBoxes.set(font, box);
    }
    return box;
  }
  // Direct2D clamps radii that don't fit; the 2D context throws on negative ones.
  function roundRectPath(x, y, w, h, radius) {
    ctx.beginPath();
    if (radius > 0)
      ctx.roundRect(x, y, w, h, Math.max(0, Math.min(radius, Math.abs(w) / 2, Math.abs(h) / 2)));
    else
      ctx.rect(x, y, w, h);
  }
  function pointsPath(points) {
    ctx.beginPath();
    ctx.moveTo(points[0], points[1]);
    for (let i = 2; i < points.length; i += 2)
      ctx.lineTo(points[i], points[i + 1]);
  }

  return {
    // After the context was reset from outside (a resize).
    invalidate() {
      fill = stroke = font = null;
    },
    clear(rgba) {
      ctx.save();
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height);
      ctx.fillStyle = css(rgba);
      ctx.fillRect(0, 0, ctx.canvas.width, ctx.canvas.height);
      ctx.restore();
    },
    fillRect(x, y, w, h, rgba, radius) {
      setFill(rgba);
      if (radius > 0) {
        roundRectPath(x, y, w, h, radius);
        ctx.fill();
      } else {
        ctx.fillRect(x, y, w, h);
      }
    },
    strokeRect(x, y, w, h, rgba, width, radius, dashed) {
      setStroke(rgba, width, dashed, !dashed);
      roundRectPath(x, y, w, h, radius);
      ctx.stroke();
    },
    fillGradient(x, y, w, h, top, bottom, radius) {
      const g = ctx.createLinearGradient(x, y, x, y + h);
      g.addColorStop(0, css(top));
      g.addColorStop(1, css(bottom));
      ctx.fillStyle = g;
      fill = null;
      roundRectPath(x, y, w, h, radius);
      ctx.fill();
    },
    fillCircle(x, y, radius, rgba) {
      setFill(rgba);
      ctx.beginPath();
      ctx.arc(x, y, Math.max(0, radius), 0, 2 * Math.PI);
      ctx.fill();
    },
    strokeCircle(x, y, radius, rgba, width) {
      setStroke(rgba, width, false, true);
      ctx.beginPath();
      ctx.arc(x, y, Math.max(0, radius), 0, 2 * Math.PI);
      ctx.stroke();
    },
    line(x1, y1, x2, y2, rgba, width, dashed) {
      setStroke(rgba, width, dashed);
      ctx.beginPath();
      ctx.moveTo(x1, y1);
      ctx.lineTo(x2, y2);
      ctx.stroke();
    },
    // The editor's angles run clockwise from 12 o'clock, the context's from 3 o'clock.
    arc(x, y, radius, from, to, rgba, width) {
      setStroke(rgba, width, false);
      ctx.beginPath();
      ctx.arc(x, y, Math.max(0, radius), from - Math.PI / 2, to - Math.PI / 2, false);
      ctx.stroke();
    },
    curve(x1, y1, cx, cy, x2, y2, rgba, width, dashed) {
      setStroke(rgba, width, dashed);
      ctx.beginPath();
      ctx.moveTo(x1, y1);
      ctx.quadraticCurveTo(cx, cy, x2, y2);
      ctx.stroke();
    },
    polyline(points, rgba, width, dashed) {
      setStroke(rgba, width, dashed);
      pointsPath(points);
      ctx.stroke();
    },
    fillPolygon(points, rgba) {
      setFill(rgba);
      pointsPath(points);
      ctx.closePath();
      ctx.fill();
    },
    // One line, centered up and down in its box like DirectWrite's paragraph centering, and cut with an
    // ellipsis when it is wider than the box (DirectWrite's character trimming).
    text(x, y, w, h, text, size, rgba, align, bold) {
      setFont(size, bold);
      let shown = text;
      if (measure(text) > w) {
        const chars = Array.from(text);
        let lo = 0, hi = chars.length - 1; // the most characters that fit before the ellipsis
        while (lo < hi) {
          const mid = (lo + hi + 1) >> 1;
          if (measure(chars.slice(0, mid).join('') + ELLIPSIS) <= w)
            lo = mid;
          else
            hi = mid - 1;
        }
        shown = chars.slice(0, lo).join('') + ELLIPSIS;
      }
      const box = fontBox();
      setFill(rgba);
      ctx.textBaseline = 'alphabetic';
      ctx.textAlign = align === 0 ? 'left' : align === 2 ? 'right' : 'center';
      const tx = align === 0 ? x : align === 2 ? x + w : x + w / 2;
      ctx.fillText(shown, tx, y + (h - box.ascent - box.descent) / 2 + box.ascent);
    },
    textWidth(text, size, bold) {
      setFont(size, bold);
      return measure(text);
    },
    pushClip(x, y, w, h) {
      ctx.save();
      ctx.beginPath();
      ctx.rect(x, y, w, h);
      ctx.clip();
    },
    popClip() {
      ctx.restore();
      fill = stroke = font = null; // back to whatever was set before the clip
    },
  };
}
